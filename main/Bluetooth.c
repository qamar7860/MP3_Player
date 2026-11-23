#include "Bluetooth.h"
#include "Player.h"
#include "SD_Card.h"
#include <string.h>
#include <stdio.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"

static const char *TAG = "BT";

/* ---------------- State ---------------- */
static esp_bd_addr_t peer_bda;
static bool          connected  = false;
static bt_status_t   s_status   = BT_STATUS_DISCONNECTED;
static char          s_connected_name[64] = {0};

/* Discovered devices table */
static bt_dev_item_t s_devices[BT_MAX_DEVICES];
static int           s_device_count = 0;

/* Auto-connect target */
static esp_bd_addr_t s_target_bda;
static bool          s_has_target = false;
static char          s_target_name[64] = {0};

/* ---------------- Headphone Gesture Callback ---------------- */
static hp_button_cb_t s_hp_callback = bt_default_headphone_handler;
static esp_timer_handle_t s_click_timer = NULL;
static uint64_t s_press_time_us = 0;
static uint64_t s_last_click_ms = 0;
static int      s_click_count = 0;

void bt_default_headphone_handler(hp_action_t action)
{
    switch (action) {
    case HP_ACTION_PLAY_PAUSE:
        ESP_LOGI(TAG, "Headphone Button: Play/Pause");
        toggle_play_pause();
        break;
    case HP_ACTION_NEXT_SONG:
        ESP_LOGI(TAG, "Headphone Button: Next Song");
        player_next();
        break;
    case HP_ACTION_PREV_SONG:
        ESP_LOGI(TAG, "Headphone Button: Previous Song");
        player_prev();
        break;
    case HP_ACTION_VOL_UP:
        ESP_LOGI(TAG, "Headphone Button: Volume Up");
        volume_up();
        break;
    case HP_ACTION_VOL_DOWN:
        ESP_LOGI(TAG, "Headphone Button: Volume Down");
        volume_down();
        break;
    default:
        break;
    }
}

void bt_set_headphone_callback(hp_button_cb_t cb)
{
    s_hp_callback = cb ? cb : bt_default_headphone_handler;
}

static void click_timer_cb(void *arg)
{
    if (s_click_count == 1) {
        s_click_count = 0;
        if (s_hp_callback) {
            s_hp_callback(HP_ACTION_PLAY_PAUSE);
        }
    } else if (s_click_count == 2) {
        s_click_count = 0;
        if (s_hp_callback) {
            s_hp_callback(HP_ACTION_NEXT_SONG);
        }
    } else if (s_click_count >= 3) {
        s_click_count = 0;
        if (s_hp_callback) {
            s_hp_callback(HP_ACTION_PREV_SONG);
        }
    }
}

static uint64_t s_last_track_cmd_ms = 0;

static void handle_headphone_passthrough(uint8_t key_code, uint8_t key_state)
{
    uint64_t now_ms = esp_timer_get_time() / 1000;
    ESP_LOGI(TAG, "Headphone AVRCP key_code=0x%02X state=%d", key_code, key_state);

    /* 1. Direct standard AVRCP Next Track commands (Right Double Click on TWS) */
    if (key_code == ESP_AVRC_PT_CMD_FORWARD ||
        key_code == ESP_AVRC_PT_CMD_FAST_FORWARD ||
        key_code == ESP_AVRC_PT_CMD_CHAN_UP ||
        key_code == ESP_AVRC_PT_CMD_PAGE_UP) {
        if (key_state == 0 && (now_ms - s_last_track_cmd_ms > 450)) {
            s_last_track_cmd_ms = now_ms;
            ESP_LOGI(TAG, "-> Headphone AVRCP Next Song triggered");
            if (s_hp_callback) s_hp_callback(HP_ACTION_NEXT_SONG);
        }
        return;
    }

    /* 2. Direct standard AVRCP Previous Track commands (Left Double Click on TWS) */
    if (key_code == ESP_AVRC_PT_CMD_BACKWARD ||
        key_code == ESP_AVRC_PT_CMD_REWIND ||
        key_code == ESP_AVRC_PT_CMD_CHAN_DOWN ||
        key_code == ESP_AVRC_PT_CMD_PAGE_DOWN) {
        if (key_state == 0 && (now_ms - s_last_track_cmd_ms > 450)) {
            s_last_track_cmd_ms = now_ms;
            ESP_LOGI(TAG, "-> Headphone AVRCP Prev Song triggered");
            if (s_hp_callback) s_hp_callback(HP_ACTION_PREV_SONG);
        }
        return;
    }

    /* 3. Direct standard AVRCP Volume commands */
    if (key_code == ESP_AVRC_PT_CMD_VOL_UP) {
        if (key_state == 0) {
            if (s_hp_callback) s_hp_callback(HP_ACTION_VOL_UP);
        }
        return;
    }

    if (key_code == ESP_AVRC_PT_CMD_VOL_DOWN) {
        if (key_state == 0) {
            if (s_hp_callback) s_hp_callback(HP_ACTION_VOL_DOWN);
        }
        return;
    }

    /* 4. Multi-click and hold detection for Play/Pause buttons */
    if (key_code == ESP_AVRC_PT_CMD_PLAY ||
        key_code == ESP_AVRC_PT_CMD_PAUSE ||
        key_code == ESP_AVRC_PT_CMD_STOP) {

        if (key_state == 0) {
            /* Button pressed down */
            s_press_time_us = esp_timer_get_time();
        } else {
            /* Button released */
            uint64_t held_ms = (esp_timer_get_time() - s_press_time_us) / 1000;
            if (held_ms >= 500) {
                /* Long press / hold detected: Volume Up */
                s_click_count = 0;
                if (s_click_timer) esp_timer_stop(s_click_timer);
                if (s_hp_callback) s_hp_callback(HP_ACTION_VOL_UP);
            } else {
                /* Short press: count clicks */
                if (now_ms - s_last_click_ms <= 400) {
                    s_click_count++;
                } else {
                    s_click_count = 1;
                }
                s_last_click_ms = now_ms;

                if (s_click_timer) {
                    esp_timer_stop(s_click_timer);
                    esp_timer_start_once(s_click_timer, 320000); /* 320 ms window */
                }
            }
        }
    }
}

/* ============================================================
 * AVRCP Target & Controller Callbacks
 * ============================================================ */
static void avrc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param)
{
    switch (event) {
    case ESP_AVRC_TG_CONNECTION_STATE_EVT:
        ESP_LOGI(TAG, "AVRC TG connected: %d", param->conn_stat.connected);
        break;

    case ESP_AVRC_TG_PASSTHROUGH_CMD_EVT: {
        uint8_t key_code  = param->psth_cmd.key_code;
        uint8_t key_state = param->psth_cmd.key_state; /* 0 = pressed, 1 = released */
        ESP_LOGI(TAG, "Headphone AVRCP key_code=0x%02X state=%d", key_code, key_state);
        handle_headphone_passthrough(key_code, key_state);
        break;
    }

    case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT: {
        uint8_t vol_127 = param->set_abs_vol.volume;
        uint8_t vol_100 = (uint8_t)((vol_127 * 100) / 127);
        ESP_LOGI(TAG, "Headphone set volume: %d (scaled: %d%%)", vol_127, vol_100);
        volume_set(vol_100);
        break;
    }

    case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT: {
        if (param->reg_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
            esp_avrc_rn_param_t rn_param;
            rn_param.volume = (uint8_t)((player_get_state()->volume * 127) / 100);
            esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_INTERIM, &rn_param);
        }
        break;
    }

    default:
        break;
    }
}

static void avrc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    switch (event) {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
        ESP_LOGI(TAG, "AVRC CT connected: %d", param->conn_stat.connected);
        break;

    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
        if (param->change_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
            uint8_t vol = (uint8_t)((param->change_ntf.event_parameter.volume * 100) / 127);
            volume_set(vol);
        }
        break;

    default:
        break;
    }
}

/* ============================================================
 * State & Device Management
 * ============================================================ */
bool bt_is_connected(void)
{
    return connected;
}

bool bt_is_scanning(void)
{
    return (s_status == BT_STATUS_SCANNING);
}

bt_status_t bt_get_status(void)
{
    return s_status;
}

const char *bt_get_connected_name(void)
{
    if (connected && s_connected_name[0]) {
        return s_connected_name;
    }
    return "None";
}

int bt_get_device_count(void)
{
    return s_device_count;
}

const bt_dev_item_t *bt_get_device(int idx)
{
    if (idx >= 0 && idx < s_device_count) {
        return &s_devices[idx];
    }
    return NULL;
}

/* ---------------- NVS Storage ---------------- */
static void save_last_device(const esp_bd_addr_t bda, const char *name)
{
    nvs_handle_t h;
    if (nvs_open("bt_cfg", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "last_bda", bda, ESP_BD_ADDR_LEN);
        if (name && name[0]) {
            nvs_set_str(h, "last_name", name);
        }
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "Saved last device: %s", name ? name : "Unknown");
    }
}

static bool load_last_device(esp_bd_addr_t bda, char *name, size_t name_size)
{
    nvs_handle_t h;
    if (nvs_open("bt_cfg", NVS_READONLY, &h) == ESP_OK) {
        size_t bda_len = ESP_BD_ADDR_LEN;
        esp_err_t r1 = nvs_get_blob(h, "last_bda", bda, &bda_len);
        esp_err_t r2 = nvs_get_str(h, "last_name", name, &name_size);
        (void)r2;
        nvs_close(h);
        return (r1 == ESP_OK && bda_len == ESP_BD_ADDR_LEN);
    }
    return false;
}

/* ---------------- Discovery Control ---------------- */
void bt_start_scan(void)
{
    if (connected) {
        ESP_LOGI(TAG, "Disconnecting current device before scanning");
        bt_disconnect();
    }
    s_device_count = 0;
    s_status = BT_STATUS_SCANNING;
    ESP_LOGI(TAG, "Starting Bluetooth discovery...");
    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
}

void bt_stop_scan(void)
{
    if (s_status == BT_STATUS_SCANNING) {
        esp_bt_gap_cancel_discovery();
        s_status = BT_STATUS_DISCONNECTED;
    }
}

void bt_connect_index(int idx)
{
    if (idx < 0 || idx >= s_device_count) return;

    if (s_status == BT_STATUS_SCANNING) {
        esp_bt_gap_cancel_discovery();
    }

    memcpy(peer_bda, s_devices[idx].bda, ESP_BD_ADDR_LEN);
    strncpy(s_connected_name, s_devices[idx].name, sizeof(s_connected_name) - 1);
    s_connected_name[sizeof(s_connected_name) - 1] = '\0';

    s_status = BT_STATUS_CONNECTING;
    ESP_LOGI(TAG, "Connecting to [%d] %s ...", idx, s_connected_name);
    esp_a2d_source_connect(peer_bda);
}

void bt_disconnect(void)
{
    if (connected) {
        esp_a2d_source_disconnect(peer_bda);
        connected = false;
        s_status = BT_STATUS_DISCONNECTED;
        s_connected_name[0] = '\0';
    }
}

static void add_or_update_device(const esp_bd_addr_t bda, const char *name, int rssi)
{
    int found_idx = -1;
    for (int i = 0; i < s_device_count; i++) {
        if (memcmp(s_devices[i].bda, bda, ESP_BD_ADDR_LEN) == 0) {
            found_idx = i;
            break;
        }
    }

    if (found_idx >= 0) {
        if (name && name[0]) {
            strncpy(s_devices[found_idx].name, name, sizeof(s_devices[found_idx].name) - 1);
            s_devices[found_idx].name[sizeof(s_devices[found_idx].name) - 1] = '\0';
        }
        s_devices[found_idx].rssi = rssi;
    } else if (s_device_count < BT_MAX_DEVICES) {
        memcpy(s_devices[s_device_count].bda, bda, ESP_BD_ADDR_LEN);
        if (name && name[0]) {
            strncpy(s_devices[s_device_count].name, name, sizeof(s_devices[s_device_count].name) - 1);
        } else {
            snprintf(s_devices[s_device_count].name, sizeof(s_devices[s_device_count].name),
                     "%02X:%02X:%02X:%02X:%02X:%02X",
                     bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
        }
        s_devices[s_device_count].name[sizeof(s_devices[s_device_count].name) - 1] = '\0';
        s_devices[s_device_count].rssi = rssi;
        s_device_count++;
    }
}

/* ============================================================
 * GAP callback: discovery results + pairing/authentication
 * ============================================================ */
static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {

    case ESP_BT_GAP_DISC_RES_EVT: {
        char name[ESP_BT_GAP_MAX_BDNAME_LEN + 1] = {0};
        int rssi = -100;

        for (int i = 0; i < param->disc_res.num_prop; i++) {
            esp_bt_gap_dev_prop_t *p = &param->disc_res.prop[i];

            if (p->type == ESP_BT_GAP_DEV_PROP_BDNAME) {
                int n = p->len > ESP_BT_GAP_MAX_BDNAME_LEN ? ESP_BT_GAP_MAX_BDNAME_LEN : p->len;
                memcpy(name, p->val, n);
                name[n] = '\0';
            }
            else if (p->type == ESP_BT_GAP_DEV_PROP_EIR) {
                uint8_t len = 0;
                uint8_t *n = esp_bt_gap_resolve_eir_data((uint8_t *)p->val,
                                 ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &len);
                if (!n) {
                    n = esp_bt_gap_resolve_eir_data((uint8_t *)p->val,
                                 ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &len);
                }
                if (n) {
                    if (len > ESP_BT_GAP_MAX_BDNAME_LEN) len = ESP_BT_GAP_MAX_BDNAME_LEN;
                    memcpy(name, n, len);
                    name[len] = '\0';
                }
            }
            else if (p->type == ESP_BT_GAP_DEV_PROP_RSSI) {
                if (p->val && p->len >= sizeof(int8_t)) {
                    rssi = *(int8_t *)(p->val);
                }
            }
        }

        if (name[0]) {
            ESP_LOGI(TAG, "Discovered device: %s (RSSI: %d)", name, rssi);
            add_or_update_device(param->disc_res.bda, name, rssi);
        }

        /* Auto-connect logic: check target */
        if (!connected && s_status != BT_STATUS_CONNECTING) {
            bool matches_target = false;
            if (s_has_target && memcmp(param->disc_res.bda, s_target_bda, ESP_BD_ADDR_LEN) == 0) {
                matches_target = true;
            } else if (!s_has_target && name[0] && strstr(name, TARGET_NAME)) {
                matches_target = true;
            }

            if (matches_target) {
                memcpy(peer_bda, param->disc_res.bda, ESP_BD_ADDR_LEN);
                if (name[0]) {
                    strncpy(s_connected_name, name, sizeof(s_connected_name) - 1);
                }
                s_status = BT_STATUS_CONNECTING;
                ESP_LOGI(TAG, "Target found! Connecting to %s...", s_connected_name);
                esp_bt_gap_cancel_discovery();
                esp_a2d_source_connect(peer_bda);
            }
        }
        break;
    }

    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            ESP_LOGI(TAG, "Discovery stopped. Devices found: %d", s_device_count);
            if (!connected && s_status == BT_STATUS_SCANNING) {
                s_status = BT_STATUS_DISCONNECTED;
            }
        }
        break;

    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Paired with: %s", param->auth_cmpl.device_name);
        } else {
            ESP_LOGE(TAG, "Pairing failed, status %d", param->auth_cmpl.stat);
        }
        break;

    default:
        break;
    }
}

/* ============================================================
 * A2DP callback: connection state + start/stop streaming
 * ============================================================ */
static void a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event) {

    case ESP_A2D_PROF_STATE_EVT:
        if (param->a2d_prof_stat.init_state == ESP_A2D_INIT_SUCCESS) {
            ESP_LOGI(TAG, "A2DP source ready");
            bt_start_scan();
        }
        break;

    case ESP_A2D_CONNECTION_STATE_EVT:
        if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            ESP_LOGI(TAG, "Connected to headphones!");
            connected = true;
            s_status  = BT_STATUS_CONNECTED;
            save_last_device(peer_bda, s_connected_name);
            esp_bt_gap_cancel_discovery();
            esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
        }
        else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            ESP_LOGW(TAG, "Disconnected");
            connected = false;
            s_status  = BT_STATUS_DISCONNECTED;
            s_connected_name[0] = '\0';
        }
        break;

    case ESP_A2D_MEDIA_CTRL_ACK_EVT:
        if (param->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY &&
            param->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
            ESP_LOGI(TAG, "Source ready, starting stream");
            esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
        }
        break;

    case ESP_A2D_AUDIO_STATE_EVT:
        ESP_LOGI(TAG, "Audio state: %d", param->audio_stat.state);
        break;

    default:
        break;
    }
}

static int32_t a2d_data_cb(uint8_t *data, int32_t len)
{
    if (len < 0 || data == NULL) {
        return 0;
    }
    return (int32_t)player_read_pcm(data, (size_t)len);
}

/* ============================================================
 * Init: bring up controller -> host -> GAP -> AVRCP -> A2DP
 * ============================================================ */
void bt_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Create timer for headphone multi-click handling */
    esp_timer_create_args_t tmr_args = {
        .callback = click_timer_cb,
        .name     = "hp_click_tmr"
    };
    esp_timer_create(&tmr_args, &s_click_timer);

    /* Load last connected device */
    s_has_target = load_last_device(s_target_bda, s_target_name, sizeof(s_target_name));
    if (s_has_target) {
        ESP_LOGI(TAG, "Loaded target device: %s", s_target_name);
    } else {
        ESP_LOGI(TAG, "No target saved, default fallback: %s", TARGET_NAME);
    }

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));

    esp_bluedroid_config_t bd_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bluedroid_init_with_cfg(&bd_cfg));
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    esp_bt_gap_register_callback(gap_cb);
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    esp_bt_gap_set_device_name("ESP32_MP3");

    /* 5. AVRCP Controller & Target for Headphone Button Gestures */
    esp_avrc_ct_register_callback(avrc_ct_cb);
    ESP_ERROR_CHECK(esp_avrc_ct_init());

    esp_avrc_tg_register_callback(avrc_tg_cb);
    ESP_ERROR_CHECK(esp_avrc_tg_init());

    /* Enable allowed Passthrough commands on AVRC Target */
    esp_avrc_psth_bit_mask_t cmd_set = {0};
    if (esp_avrc_tg_get_psth_cmd_filter(ESP_AVRC_PSTH_FILTER_ALLOWED_CMD, &cmd_set) == ESP_OK) {
        esp_avrc_tg_set_psth_cmd_filter(ESP_AVRC_PSTH_FILTER_SUPPORTED_CMD, &cmd_set);
    }

    /* Support volume change notification */
    esp_avrc_rn_evt_cap_mask_t evt_set = {0};
    esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &evt_set, ESP_AVRC_RN_VOLUME_CHANGE);
    ESP_ERROR_CHECK(esp_avrc_tg_set_rn_evt_cap(&evt_set));

    /* 6. A2DP source */
    esp_a2d_register_callback(a2d_cb);
    esp_a2d_source_register_data_callback(a2d_data_cb);
    ESP_ERROR_CHECK(esp_a2d_source_init());
}
