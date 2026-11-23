#include "UI.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "SD_Card.h"
#include "Player.h"
#include "Bluetooth.h"
#include "Battery.h"
#include "Temperature.h"
#include "Display.h"
#include "esp_sleep.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "UI";

/* External Background Image Descriptor from BG.c */
extern const lv_image_dsc_t BG;

/* ---------------- Colors (OLED Cyberpunk Palette) ---------------- */
#define COLOR_BG          lv_color_hex(0x080810) /* Matches BG.c canvas background */
#define COLOR_PANEL       lv_color_hex(0x080F1A) /* Deep Cyber Panel */
#define COLOR_PANEL_ALT   lv_color_hex(0x16304C) /* Vibrant Pressable Button Fill */
#define COLOR_ACCENT      lv_color_hex(0x00E5FF) /* Electric Neon Cyan-Blue */
#define COLOR_TEXT        lv_color_hex(0xFFFFFF)
#define COLOR_TEXT_MUTED  lv_color_hex(0xB0BEC5) /* High-contrast soft silver-cyan */
#define COLOR_AMBER       lv_color_hex(0xFFA500)
#define COLOR_GREEN       lv_color_hex(0x00E676)

/* ---------------- Robot Face Colors (Matched to Background Palette) ---------------- */
#define COLOR_ROBOT_BLUE  lv_color_hex(0x00E5FF) /* High-contrast Electric Neon Cyan-Blue */
#define COLOR_ROBOT_DARK  lv_color_hex(0x080810) /* Dark core matching BG wallpaper */
#define COLOR_ROBOT_BG    lv_color_hex(0x080810) /* OLED Face Bezel matching BG wallpaper */

/* ---------------- Screens ---------------- */
lv_obj_t *Home;
lv_obj_t *Library;
lv_obj_t *Setting;
lv_obj_t *Bluetooth;

/* ---------------- Home Screen Widgets ---------------- */
static lv_obj_t *home_track_pill;
static lv_obj_t *home_shuf_badge;
static lv_obj_t *home_bt_badge;
static lv_obj_t *home_battery_badge;
static lv_obj_t *track_title_label;
static lv_obj_t *track_meta_label;
static lv_obj_t *progress_slider;
static lv_obj_t *time_cur_label;
static lv_obj_t *time_rem_label;
static lv_obj_t *play_btn_icon;
static lv_obj_t *play_btn_obj;

/* Interactive Vertical Volume Bar HUD */
static lv_obj_t *vol_bar_cont;
static lv_obj_t *vol_icon_label;
static lv_obj_t *vol_bar;
static uint32_t  vol_bar_expiry = 0;
static uint8_t   last_displayed_vol = 0xFF; /* 0xFF indicates uninitialized, prevents popping up on boot */

/* ---------------- Library Screen Widgets (Virtual Windowed List) ---------------- */
#define VISIBLE_TRACKS 6
static lv_obj_t *track_list;
static lv_obj_t *track_buttons[VISIBLE_TRACKS];
static int       lib_window_start = 0;
static lv_obj_t *lib_title_label;

/* ---------------- Bluetooth Screen Widgets ---------------- */
static lv_obj_t *bt_status_card;
static lv_obj_t *bt_status_label;
static lv_obj_t *bt_list;
static lv_obj_t *bt_scan_btn;
static lv_obj_t *bt_dev_buttons[BT_MAX_DEVICES];
static int       bt_selected_index = 0; /* 0 = scan button, 1..N = devices */
static int       last_bt_count = -1;
static bt_status_t last_bt_status = (bt_status_t)-1;

/* ---------------- Settings Screen Widgets ---------------- */
#define SET_ITEMS_COUNT 15
static lv_obj_t *set_items[SET_ITEMS_COUNT];
static int       set_selected_index = 0;
static lv_obj_t *set_list;
static lv_obj_t *set_vol_label;
static lv_obj_t *set_bt_label;
static lv_obj_t *set_sd_label;
static lv_obj_t *set_bat_label;
static lv_obj_t *set_ram_label;
static lv_obj_t *set_temp_label;
static lv_obj_t *set_saver_label;

/* ---------------- Battery Saver & Power Management ---------------- */
static bool     s_battery_saver = false;
static uint32_t s_click_window_start = 0;
static uint8_t  s_multi_clicks = 0;

void enter_battery_saver_mode(void)
{
    if (s_battery_saver) return;
    s_battery_saver = true;
    ESP_LOGI(TAG, "Entering Battery Saver Mode (Screen black, background audio active)");
    display_set_sleeping(true);
}

void exit_battery_saver_mode(void)
{
    if (!s_battery_saver) return;
    s_battery_saver = false;
    ESP_LOGI(TAG, "Exiting Battery Saver Mode (Display woke up)");
    display_set_sleeping(false);
}

bool is_battery_saver_mode(void)
{
    return s_battery_saver;
}

void power_off_device(void)
{
    ESP_LOGI(TAG, "Powering OFF -> Deep Sleep (Wake via PIN_POWER_SLEEP GPIO %d)...", PIN_POWER_SLEEP);
    player_stop();
    display_set_sleeping(true);
    ili9341_clear_black();
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_sleep_enable_ext0_wakeup(PIN_POWER_SLEEP, 0); // Active Low wakeup on PIN_POWER_SLEEP
    esp_deep_sleep_start();
}

/* Forward Declarations */
void update_library_selection(void);
void update_setting_selection(void);
void buttons_gpio_init(void);
static void button_poll_cb(lv_timer_t *t);
static void ui_periodic_timer(lv_timer_t *t);
static void refresh_bluetooth_list(void);

/* Format time as MM:SS */
static void format_time_str(char *buf, size_t sz, uint32_t sec)
{
    uint32_t m = sec / 60;
    uint32_t s = sec % 60;
    snprintf(buf, sz, "%02u:%02u", (unsigned)m, (unsigned)s);
}

/* Helper to initialize system RTC clock from compile timestamp if uninitialized */
static void init_system_clock(void)
{
    time_t now = time(NULL);
    if (now < 1700000000LL) {
        static const char *const months = "JanFebMarAprMayJunJulAugSepOctNovDec";
        char m_str[4] = { __DATE__[0], __DATE__[1], __DATE__[2], '\0' };
        const char *p = strstr(months, m_str);
        int month = p ? (int)(p - months) / 3 : 10;
        int day = atoi(&__DATE__[4]);
        int year = atoi(&__DATE__[7]);
        int hour = atoi(&__TIME__[0]);
        int min = atoi(&__TIME__[3]);
        int sec = atoi(&__TIME__[6]);

        struct tm t = {
            .tm_year = year - 1900,
            .tm_mon = month,
            .tm_mday = day,
            .tm_hour = hour,
            .tm_min = min,
            .tm_sec = sec,
            .tm_isdst = -1
        };
        time_t t_epoch = mktime(&t);
        if (t_epoch > 0) {
            struct timeval tv = { .tv_sec = t_epoch, .tv_usec = 0 };
            settimeofday(&tv, NULL);
            ESP_LOGI(TAG, "System clock initialized to %04d-%02d-%02d %02d:%02d:%02d",
                     year, month + 1, day, hour, min, sec);
        }
    }
}

/* Helper to strip .mp3 / .wav file extension for clean consumer display */
static void get_display_title(int song_idx, char *dst, size_t dst_sz)
{
    const char *raw = song_get_name(song_idx);
    if (!raw || !raw[0]) {
        snprintf(dst, dst_sz, "Unknown");
        return;
    }
    strncpy(dst, raw, dst_sz - 1);
    dst[dst_sz - 1] = '\0';
    char *dot = strrchr(dst, '.');
    if (dot && (strcasecmp(dot, ".mp3") == 0 || strcasecmp(dot, ".wav") == 0)) {
        *dot = '\0';
    }
}

/* Helper to create unified top status bar with bumped icons */
static lv_obj_t *create_top_bar(lv_obj_t *parent, const char *title, lv_obj_t **out_badge, lv_obj_t **out_shuf, lv_obj_t **out_bt, lv_obj_t **out_bat)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, 240, 32);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 2, 0);
    lv_obj_set_style_bg_color(bar, COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(bar, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* Left title / Screen name - subtle muted silver */
    lv_obj_t *title_lbl = lv_label_create(bar);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title_lbl, COLOR_TEXT_MUTED, 0);
    lv_obj_align(title_lbl, LV_ALIGN_LEFT_MID, 6, 0);
    if (out_badge) *out_badge = title_lbl;

    /* Shuffle Status Badge - clean centered tag */
    lv_obj_t *shuf_lbl = lv_label_create(bar);
    lv_label_set_text(shuf_lbl, "");
    lv_obj_set_style_text_font(shuf_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(shuf_lbl, COLOR_ACCENT, 0);
    lv_obj_align(shuf_lbl, LV_ALIGN_CENTER, 0, 0);
    if (out_shuf) *out_shuf = shuf_lbl;

    /* Bluetooth Status Icon - Montserrat 16 */
    lv_obj_t *bt_lbl = lv_label_create(bar);
    lv_label_set_text(bt_lbl, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_font(bt_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(bt_lbl, lv_color_hex(0x556677), 0);
    lv_obj_align(bt_lbl, LV_ALIGN_RIGHT_MID, -68, 0);
    if (out_bt) *out_bt = bt_lbl;

    /* Battery Pill - clean muted info */
    lv_obj_t *bat_lbl = lv_label_create(bar);
    lv_label_set_text(bat_lbl, LV_SYMBOL_BATTERY_FULL " 100%");
    lv_obj_set_style_text_font(bat_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(bat_lbl, COLOR_TEXT_MUTED, 0);
    lv_obj_align(bat_lbl, LV_ALIGN_RIGHT_MID, -6, 0);
    if (out_bat) *out_bat = bat_lbl;

    return bar;
}

/* ============================================================
 * Initialize UI Screens
 * ============================================================ */
void ui_init(void)
{
    ESP_LOGI(TAG, "Initializing Modernized MP3 UI with Blue OLED Robot Face...");
    init_system_clock();

    /****************************************************************************
     * 1. HOME SCREEN (Now Playing with Blue OLED Robot Face & Custom BG)
     ****************************************************************************/
    Home = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(Home, COLOR_BG, 0);

    /* Background Wallpaper: strictly below top header (y=26..320, 240x294) */
    lv_obj_t *home_bg = lv_image_create(Home);
    lv_image_set_src(home_bg, &BG);
    lv_obj_set_pos(home_bg, 0, -26);
    lv_obj_set_size(home_bg, 240, 294);

    /* Top Bar: sits at y=0..26 with solid background, Bluetooth icon, battery pill */
    create_top_bar(Home, "--:--", &home_track_pill, &home_shuf_badge, &home_bt_badge, &home_battery_badge);

    /* Body container: Translucent so custom BG wallpaper shines through */
    lv_obj_t *Home_body = lv_obj_create(Home);
    lv_obj_set_size(Home_body, 240, 190);
    lv_obj_set_pos(Home_body, 0, 28);
    lv_obj_set_style_radius(Home_body, 0, 0);
    lv_obj_set_style_border_width(Home_body, 0, 0);
    lv_obj_set_style_pad_all(Home_body, 0, 0);
    lv_obj_set_style_bg_opa(Home_body, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(Home_body, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(Home_body, LV_OBJ_FLAG_SCROLLABLE);

    /* Borderless Track Information Container (Zero border fatigue, clean typography) */
    lv_obj_t *info_card = lv_obj_create(Home_body);
    lv_obj_set_size(info_card, 226, 52);
    lv_obj_align(info_card, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_radius(info_card, 10, 0);
    lv_obj_set_style_border_width(info_card, 0, 0); /* NO BORDER! Let typography separate */
    lv_obj_set_style_pad_all(info_card, 4, 0);
    lv_obj_set_style_bg_color(info_card, lv_color_hex(0x050C16), 0);
    lv_obj_set_style_bg_opa(info_card, LV_OPA_60, 0);
    lv_obj_set_scrollbar_mode(info_card, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(info_card, LV_OBJ_FLAG_SCROLLABLE);

    /* Track Title: Bold Pure White, Full width marquee scroll */
    track_title_label = lv_label_create(info_card);
    lv_label_set_text(track_title_label, "Select a Song");
    lv_obj_set_width(track_title_label, 218);
    lv_label_set_long_mode(track_title_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_font(track_title_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(track_title_label, lv_color_white(), 0); /* Pure White title! */
    lv_obj_align(track_title_label, LV_ALIGN_TOP_LEFT, 4, 1);

    /* Track Meta: High-contrast soft muted silver-cyan */
    track_meta_label = lv_label_create(info_card);
    lv_label_set_text(track_meta_label, "MP3 44.1kHz • Paused");
    lv_obj_set_style_text_font(track_meta_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(track_meta_label, lv_color_hex(0x7A94A8), 0);
    lv_obj_align(track_meta_label, LV_ALIGN_BOTTOM_LEFT, 4, -1);

    /* Interactive Vertical Volume Bar HUD (Capsule Pill on Right Edge) */
    vol_bar_cont = lv_obj_create(Home);
    lv_obj_set_size(vol_bar_cont, 24, 110);
    lv_obj_align(vol_bar_cont, LV_ALIGN_RIGHT_MID, -6, -10);
    lv_obj_set_style_radius(vol_bar_cont, 12, 0);
    lv_obj_set_style_border_width(vol_bar_cont, 1, 0);
    lv_obj_set_style_border_color(vol_bar_cont, COLOR_ACCENT, 0);
    lv_obj_set_style_bg_color(vol_bar_cont, lv_color_hex(0x060B14), 0);
    lv_obj_set_style_bg_opa(vol_bar_cont, LV_OPA_90, 0);
    lv_obj_set_style_pad_all(vol_bar_cont, 2, 0);
    lv_obj_set_scrollbar_mode(vol_bar_cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(vol_bar_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_hidden(vol_bar_cont, true);

    vol_icon_label = lv_label_create(vol_bar_cont);
    lv_label_set_text(vol_icon_label, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_font(vol_icon_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(vol_icon_label, COLOR_ACCENT, 0);
    lv_obj_align(vol_icon_label, LV_ALIGN_TOP_MID, 0, 4);

    vol_bar = lv_bar_create(vol_bar_cont);
    lv_obj_set_size(vol_bar, 6, 74);
    lv_obj_align(vol_bar, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_bar_set_range(vol_bar, 0, 100);
    lv_bar_set_value(vol_bar, 50, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(vol_bar, lv_color_hex(0x1A283C), LV_PART_MAIN);
    lv_obj_set_style_bg_color(vol_bar, COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_radius(vol_bar, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(vol_bar, 3, LV_PART_INDICATOR);

    /* --- Footer Controls Container (Clean Dedicated Rows, Translucent BG) --- */
    lv_obj_t *Home_footer = lv_obj_create(Home);
    lv_obj_set_size(Home_footer, 240, 96);
    lv_obj_set_pos(Home_footer, 0, 224);
    lv_obj_set_style_radius(Home_footer, 0, 0);
    lv_obj_set_style_border_width(Home_footer, 0, 0);
    lv_obj_set_style_pad_all(Home_footer, 0, 0);
    lv_obj_set_style_bg_opa(Home_footer, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(Home_footer, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(Home_footer, LV_OBJ_FLAG_SCROLLABLE);

    /* Row 1: Time Displays (Elapsed on left, Remaining on right) */
    time_cur_label = lv_label_create(Home_footer);
    lv_label_set_text(time_cur_label, "00:00");
    lv_obj_set_style_text_font(time_cur_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(time_cur_label, lv_color_hex(0xA8BDCE), 0);
    lv_obj_align(time_cur_label, LV_ALIGN_TOP_LEFT, 14, 4);

    time_rem_label = lv_label_create(Home_footer);
    lv_label_set_text(time_rem_label, "00:00");
    lv_obj_set_style_text_font(time_rem_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(time_rem_label, lv_color_hex(0xA8BDCE), 0);
    lv_obj_align(time_rem_label, LV_ALIGN_TOP_RIGHT, -14, 4);

    /* Row 2: Progress Slider (Solid dark rail, glowing cyan indicator, white knob) */
    progress_slider = lv_slider_create(Home_footer);
    lv_slider_set_orientation(progress_slider, LV_SLIDER_ORIENTATION_HORIZONTAL);
    lv_slider_set_range(progress_slider, 0, 100);
    lv_slider_set_value(progress_slider, 0, LV_ANIM_OFF);
    lv_obj_set_size(progress_slider, 212, 6);
    lv_obj_align(progress_slider, LV_ALIGN_TOP_MID, 0, 22);
    lv_obj_set_style_bg_color(progress_slider, lv_color_hex(0x121E2E), LV_PART_MAIN);
    lv_obj_set_style_bg_color(progress_slider, COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(progress_slider, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(progress_slider, 2, LV_PART_KNOB);

    /* Row 3: Bright, Interactive Hardware-Feel Transport Buttons */
    /* Previous Track Button */
    lv_obj_t *prev_btn = lv_obj_create(Home_footer);
    lv_obj_set_size(prev_btn, 36, 36);
    lv_obj_align(prev_btn, LV_ALIGN_BOTTOM_LEFT, 30, -10);
    lv_obj_set_style_radius(prev_btn, 18, 0);
    lv_obj_set_style_border_width(prev_btn, 2, 0);
    lv_obj_set_style_border_color(prev_btn, COLOR_ACCENT, 0);
    lv_obj_set_style_bg_color(prev_btn, COLOR_PANEL_ALT, 0);
    lv_obj_set_style_pad_all(prev_btn, 0, 0);

    lv_obj_t *prev_icon = lv_label_create(prev_btn);
    lv_label_set_text(prev_icon, LV_SYMBOL_PREV);
    lv_obj_set_style_text_font(prev_icon, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(prev_icon, lv_color_white(), 0);
    lv_obj_center(prev_icon);

    /* Center Play/Pause Hero Button */
    play_btn_obj = lv_obj_create(Home_footer);
    lv_obj_set_size(play_btn_obj, 42, 42);
    lv_obj_align(play_btn_obj, LV_ALIGN_BOTTOM_MID, 0, -7);
    lv_obj_set_style_radius(play_btn_obj, 21, 0);
    lv_obj_set_style_border_width(play_btn_obj, 2, 0);
    lv_obj_set_style_border_color(play_btn_obj, lv_color_white(), 0);
    lv_obj_set_style_bg_color(play_btn_obj, COLOR_ACCENT, 0);
    lv_obj_set_style_pad_all(play_btn_obj, 0, 0);

    play_btn_icon = lv_label_create(play_btn_obj);
    lv_label_set_text(play_btn_icon, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_font(play_btn_icon, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(play_btn_icon, lv_color_black(), 0);
    lv_obj_center(play_btn_icon);

    /* Next Track Button */
    lv_obj_t *next_btn = lv_obj_create(Home_footer);
    lv_obj_set_size(next_btn, 36, 36);
    lv_obj_align(next_btn, LV_ALIGN_BOTTOM_RIGHT, -30, -10);
    lv_obj_set_style_radius(next_btn, 18, 0);
    lv_obj_set_style_border_width(next_btn, 2, 0);
    lv_obj_set_style_border_color(next_btn, COLOR_ACCENT, 0);
    lv_obj_set_style_bg_color(next_btn, COLOR_PANEL_ALT, 0);
    lv_obj_set_style_pad_all(next_btn, 0, 0);

    lv_obj_t *next_icon = lv_label_create(next_btn);
    lv_label_set_text(next_icon, LV_SYMBOL_NEXT);
    lv_obj_set_style_text_font(next_icon, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(next_icon, lv_color_white(), 0);
    lv_obj_center(next_icon);

    /****************************************************************************
     * 2. LIBRARY SCREEN (Interactive Track List)
     ****************************************************************************/
    Library = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(Library, COLOR_BG, 0);

    create_top_bar(Library, "MUSIC LIBRARY", &lib_title_label, NULL, NULL, NULL);

    track_list = lv_list_create(Library);
    lv_obj_set_size(track_list, 240, 288);
    lv_obj_set_pos(track_list, 0, 32);
    lv_obj_set_style_radius(track_list, 0, 0);
    lv_obj_set_style_border_width(track_list, 0, 0);
    lv_obj_set_style_bg_color(track_list, COLOR_BG, 0);
    lv_obj_set_style_pad_all(track_list, 4, 0);

    for (int i = 0; i < VISIBLE_TRACKS; i++) {
        track_buttons[i] = lv_list_add_button(track_list, LV_SYMBOL_AUDIO, "---");
        lv_obj_set_style_radius(track_buttons[i], 6, 0);
        lv_obj_set_style_pad_all(track_buttons[i], 6, 0);
        lv_obj_set_style_margin_bottom(track_buttons[i], 3, 0);
    }

    /****************************************************************************
     * 3. BLUETOOTH SCREEN
     ****************************************************************************/
    Bluetooth = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(Bluetooth, COLOR_BG, 0);

    create_top_bar(Bluetooth, "BLUETOOTH AUDIO", NULL, NULL, NULL, NULL);

    /* Connection Status Card */
    bt_status_card = lv_obj_create(Bluetooth);
    lv_obj_set_size(bt_status_card, 230, 32);
    lv_obj_set_pos(bt_status_card, 5, 35);
    lv_obj_set_style_radius(bt_status_card, 6, 0);
    lv_obj_set_style_border_width(bt_status_card, 1, 0);
    lv_obj_set_style_border_color(bt_status_card, lv_color_hex(0x283850), 0);
    lv_obj_set_style_bg_color(bt_status_card, COLOR_PANEL, 0);
    lv_obj_set_style_pad_all(bt_status_card, 4, 0);

    bt_status_label = lv_label_create(bt_status_card);
    lv_label_set_text(bt_status_label, "Status: Disconnected");
    lv_obj_set_style_text_font(bt_status_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(bt_status_label, COLOR_TEXT_MUTED, 0);
    lv_obj_center(bt_status_label);

    /* Discovered Devices List */
    bt_list = lv_list_create(Bluetooth);
    lv_obj_set_size(bt_list, 240, 250);
    lv_obj_set_pos(bt_list, 0, 70);
    lv_obj_set_style_radius(bt_list, 0, 0);
    lv_obj_set_style_border_width(bt_list, 0, 0);
    lv_obj_set_style_bg_color(bt_list, COLOR_BG, 0);
    lv_obj_set_style_pad_all(bt_list, 4, 0);

    bt_scan_btn = lv_list_add_button(bt_list, LV_SYMBOL_REFRESH, "Scan for Devices");
    refresh_bluetooth_list();

    /****************************************************************************
     * 4. SETTINGS SCREEN
     ****************************************************************************/
    Setting = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(Setting, COLOR_BG, 0);

    create_top_bar(Setting, "SETTINGS & STATS", NULL, NULL, NULL, NULL);

    set_list = lv_list_create(Setting);
    lv_obj_set_size(set_list, 240, 288);
    lv_obj_set_pos(set_list, 0, 32);
    lv_obj_set_style_radius(set_list, 0, 0);
    lv_obj_set_style_border_width(set_list, 0, 0);
    lv_obj_set_style_bg_color(set_list, COLOR_BG, 0);
    lv_obj_set_style_pad_all(set_list, 4, 0);

    /* Section 1: Headphone Gestures Card */
    lv_list_add_text(set_list, "HEADPHONE CONTROLS");

    set_items[0] = lv_list_add_button(set_list, LV_SYMBOL_PLAY, "R 1x: Play / Pause");
    set_items[1] = lv_list_add_button(set_list, LV_SYMBOL_NEXT, "R 2x: Next Song");
    set_items[2] = lv_list_add_button(set_list, LV_SYMBOL_VOLUME_MAX, "R Hold: Volume Up (+)");
    set_items[3] = lv_list_add_button(set_list, LV_SYMBOL_PREV, "L 2x: Previous Song");
    set_items[4] = lv_list_add_button(set_list, LV_SYMBOL_VOLUME_MID, "L Hold: Volume Down (-)");

    /* Section 2: 3-Button Device Controls */
    lv_list_add_text(set_list, "DEVICE 3-BUTTON CONTROLS");
    set_items[5] = lv_list_add_button(set_list, LV_SYMBOL_POWER, "Pwr (32): Tap Off | Hold Sleep");
    set_items[6] = lv_list_add_button(set_list, LV_SYMBOL_RIGHT, "Nav (33): Tap Step | Hold Jump");
    set_items[7] = lv_list_add_button(set_list, LV_SYMBOL_PLAY, "Act (25): Tap Enter | Hold Shuf");

    /* Section 3: System Telemetry */
    lv_list_add_text(set_list, "SYSTEM & HARDWARE");

    set_vol_label   = lv_list_add_button(set_list, LV_SYMBOL_VOLUME_MAX, "Volume: 50%");
    set_bt_label    = lv_list_add_button(set_list, LV_SYMBOL_BLUETOOTH,  "Audio Out: None");
    set_sd_label    = lv_list_add_button(set_list, LV_SYMBOL_SD_CARD,    "SD Storage: 0 Songs");
    set_bat_label   = lv_list_add_button(set_list, LV_SYMBOL_BATTERY_FULL, "Battery: 100%");
    set_ram_label   = lv_list_add_button(set_list, LV_SYMBOL_SETTINGS,   "RAM Free: -- KB");
    set_temp_label  = lv_list_add_button(set_list, LV_SYMBOL_SETTINGS,   "Chip Temp: -- °C");
    set_saver_label = lv_list_add_button(set_list, LV_SYMBOL_CHARGE,     "Power Mode: Normal");

    set_items[8]  = set_vol_label;
    set_items[9]  = set_bt_label;
    set_items[10] = set_sd_label;
    set_items[11] = set_bat_label;
    set_items[12] = set_ram_label;
    set_items[13] = set_temp_label;
    set_items[14] = set_saver_label;

    for (int i = 0; i < SET_ITEMS_COUNT; i++) {
        lv_obj_set_style_radius(set_items[i], 6, 0);
        lv_obj_set_style_pad_all(set_items[i], 6, 0);
        lv_obj_set_style_margin_bottom(set_items[i], 3, 0);
    }
    update_setting_selection();

    /* Initialize physical buttons & periodic timers */
    buttons_gpio_init();
    lv_timer_create(button_poll_cb, 20, NULL);
    lv_timer_create(ui_periodic_timer, 120, NULL);

    update_library_selection();
    lv_screen_load(Home);
}

void update_setting_selection(void)
{
    for (int i = 0; i < SET_ITEMS_COUNT; i++) {
        if (!set_items[i]) continue;
        if (i == set_selected_index) {
            lv_obj_set_style_bg_color(set_items[i], COLOR_ACCENT, 0);
            lv_obj_set_style_text_color(set_items[i], lv_color_black(), 0);
            lv_obj_scroll_to_view(set_items[i], LV_ANIM_ON);
        } else {
            lv_obj_set_style_bg_color(set_items[i], COLOR_PANEL, 0);
            lv_obj_set_style_text_color(set_items[i], (i < 8) ? COLOR_ACCENT : COLOR_TEXT, 0);
        }
    }
}

/* ============================================================
 * Bluetooth Screen Helpers
 * ============================================================ */
static void refresh_bluetooth_list(void)
{
    int count = bt_get_device_count();
    lv_obj_clean(bt_list);

    bt_scan_btn = lv_list_add_button(bt_list, LV_SYMBOL_REFRESH,
                                     bt_is_scanning() ? "Searching for Receivers..." : "Scan for Devices");

    if (bt_selected_index == 0) {
        lv_obj_set_style_bg_color(bt_scan_btn, COLOR_ACCENT, 0);
        lv_obj_set_style_text_color(bt_scan_btn, lv_color_black(), 0);
    } else {
        lv_obj_set_style_bg_color(bt_scan_btn, COLOR_PANEL_ALT, 0);
        lv_obj_set_style_text_color(bt_scan_btn, COLOR_TEXT, 0);
    }

    for (int i = 0; i < count && i < BT_MAX_DEVICES; i++) {
        const bt_dev_item_t *dev = bt_get_device(i);
        if (!dev) continue;

        char buf[80];
        snprintf(buf, sizeof(buf), "%s (%d dBm)", dev->name, dev->rssi);
        bt_dev_buttons[i] = lv_list_add_button(bt_list, LV_SYMBOL_BLUETOOTH, buf);
        lv_obj_set_style_radius(bt_dev_buttons[i], 6, 0);
        lv_obj_set_style_margin_bottom(bt_dev_buttons[i], 3, 0);

        if (bt_selected_index == (i + 1)) {
            lv_obj_set_style_bg_color(bt_dev_buttons[i], COLOR_ACCENT, 0);
            lv_obj_set_style_text_color(bt_dev_buttons[i], lv_color_black(), 0);
            lv_obj_scroll_to_view(bt_dev_buttons[i], LV_ANIM_ON);
        } else {
            lv_obj_set_style_bg_color(bt_dev_buttons[i], COLOR_PANEL, 0);
            lv_obj_set_style_text_color(bt_dev_buttons[i], COLOR_TEXT, 0);
        }
    }
}

/* ============================================================
 * Update Library Selection Highlighting
 * ============================================================ */
void update_library_selection(void)
{
    if (song_count == 0) {
        for (int i = 0; i < VISIBLE_TRACKS; i++) {
            if (track_buttons[i]) lv_obj_set_hidden(track_buttons[i], true);
        }
        if (lib_title_label) lv_label_set_text(lib_title_label, "MUSIC LIBRARY (0)");
        return;
    }

    /* Window sliding logic: keep selected_song within visible window */
    if (selected_song < lib_window_start) {
        lib_window_start = selected_song;
    } else if (selected_song >= lib_window_start + VISIBLE_TRACKS) {
        lib_window_start = selected_song - VISIBLE_TRACKS + 1;
    }
    if (lib_window_start + VISIBLE_TRACKS > song_count && song_count >= VISIBLE_TRACKS) {
        lib_window_start = song_count - VISIBLE_TRACKS;
    }
    if (lib_window_start < 0) lib_window_start = 0;

    /* Update title with current track and total count */
    if (lib_title_label) {
        char t_buf[48];
        snprintf(t_buf, sizeof(t_buf), "LIBRARY  %d / %d", selected_song + 1, song_count);
        lv_label_set_text(lib_title_label, t_buf);
    }

    /* Populate the 6 visible slots dynamically */
    for (int i = 0; i < VISIBLE_TRACKS; i++) {
        int song_idx = lib_window_start + i;
        if (!track_buttons[i]) continue;

        if (song_idx < song_count) {
            lv_obj_set_hidden(track_buttons[i], false);
            char clean_title[64];
            get_display_title(song_idx, clean_title, sizeof(clean_title));
            char item_text[96];
            snprintf(item_text, sizeof(item_text), "%02d. %s", song_idx + 1, clean_title);
            lv_list_set_button_text(track_list, track_buttons[i], item_text);

            if (song_idx == selected_song) {
                lv_obj_set_style_bg_color(track_buttons[i], COLOR_ACCENT, LV_PART_MAIN);
                lv_obj_set_style_text_color(track_buttons[i], lv_color_black(), LV_PART_MAIN);
                lv_obj_set_style_border_width(track_buttons[i], 0, 0);
            } else {
                lv_obj_set_style_bg_color(track_buttons[i], COLOR_PANEL, LV_PART_MAIN);
                lv_obj_set_style_text_color(track_buttons[i], COLOR_TEXT, LV_PART_MAIN);
                lv_obj_set_style_border_width(track_buttons[i], 1, 0);
                lv_obj_set_style_border_color(track_buttons[i], lv_color_hex(0x222222), 0);
            }
        } else {
            lv_obj_set_hidden(track_buttons[i], true);
        }
    }
}

/* ============================================================
 * Periodic UI Refresh (Runs every 120 ms)
 * ============================================================ */
static void ui_periodic_timer(lv_timer_t *t)
{
    const player_state_t *st = player_get_state();
    bool is_bt_connected = bt_is_connected();
    lv_obj_t *cur_scr = lv_screen_active();

    /* 1. Update Date & Time Clock on Top Left Header (Replaced 10/30 option) */
    time_t now = time(NULL);
    struct tm ti;
    localtime_r(&now, &ti);
    static const char *const s_month_names[12] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    char dt_buf[24];
    snprintf(dt_buf, sizeof(dt_buf), "%02d %s  %02d:%02d",
             ti.tm_mday, s_month_names[ti.tm_mon % 12], ti.tm_hour, ti.tm_min);
    if (home_track_pill) {
        lv_label_set_text(home_track_pill, dt_buf);
    }

    /* 2. Update Play/Pause Button State & Highlight */
    if (st->playing) {
        lv_label_set_text(play_btn_icon, LV_SYMBOL_PAUSE);
        lv_obj_set_style_bg_color(play_btn_obj, COLOR_ACCENT, 0);
        lv_obj_set_style_text_color(play_btn_icon, lv_color_black(), 0);
        lv_obj_set_style_border_color(play_btn_obj, lv_color_white(), 0);
    } else {
        lv_label_set_text(play_btn_icon, LV_SYMBOL_PLAY);
        lv_obj_set_style_bg_color(play_btn_obj, lv_color_hex(0x14283E), 0);
        lv_obj_set_style_text_color(play_btn_icon, COLOR_ACCENT, 0);
        lv_obj_set_style_border_color(play_btn_obj, COLOR_ACCENT, 0);
    }

    /* 3. Update Shuffle Status Badge */
    if (home_shuf_badge) {
        if (player_is_shuffle()) {
            lv_label_set_text(home_shuf_badge, LV_SYMBOL_SHUFFLE );
        } else {
            lv_label_set_text(home_shuf_badge, "");
        }
    }

    /* 4. Update Progress Slider and Time (Elapsed & Remaining) */
    lv_slider_set_value(progress_slider, st->progress_pct, LV_ANIM_OFF);

    char cur_str[16], rem_str[16];
    format_time_str(cur_str, sizeof(cur_str), st->elapsed_sec);
    lv_label_set_text(time_cur_label, cur_str);

    if (st->duration_sec >= st->elapsed_sec) {
        uint32_t rem = st->duration_sec - st->elapsed_sec;
        format_time_str(rem_str, sizeof(rem_str), rem);
        char buf[20];
        snprintf(buf, sizeof(buf), "-%s", rem_str);
        lv_label_set_text(time_rem_label, buf);
    } else {
        format_time_str(rem_str, sizeof(rem_str), st->duration_sec);
        lv_label_set_text(time_rem_label, rem_str);
    }

    /* 5. Update Song Title & Meta with High Contrast (Only when changed) */
    static int s_last_title_song = -999;
    int active_song = (st->current_song >= 0 && st->current_song < song_count) ? st->current_song : selected_song;
    if (active_song != s_last_title_song) {
        s_last_title_song = active_song;
        if (song_count > 0 && active_song >= 0 && active_song < song_count) {
            char clean_title[64];
            get_display_title(active_song, clean_title, sizeof(clean_title));
            lv_label_set_text(track_title_label, clean_title);
        } else {
            lv_label_set_text(track_title_label, "No Songs Found");
        }
    }

    static int s_last_meta_playing = -1;
    static uint32_t s_last_meta_bitrate = 0xFFFFFFFF;
    if ((int)st->playing != s_last_meta_playing || st->bitrate_kbps != s_last_meta_bitrate) {
        s_last_meta_playing = (int)st->playing;
        s_last_meta_bitrate = st->bitrate_kbps;
        char meta_buf[64];
        snprintf(meta_buf, sizeof(meta_buf), "%s  •  %u kbps",
                 st->playing ? "Playing" : "Paused", (unsigned)st->bitrate_kbps);
        lv_label_set_text(track_meta_label, meta_buf);
    }

    /* 6. Check Volume / Interactive Vertical Volume Bar HUD */
    if (last_displayed_vol == 0xFF) {
        /* First run initialization: silent sync without popping up on boot */
        last_displayed_vol = st->volume;
        lv_bar_set_value(vol_bar, st->volume, LV_ANIM_OFF);
    } else if (st->volume != last_displayed_vol) {
        last_displayed_vol = st->volume;
        vol_bar_expiry = lv_tick_get() + 1500; /* Show for 1.5 seconds */
        lv_label_set_text(vol_icon_label,
            st->volume == 0 ? LV_SYMBOL_MUTE :
            (st->volume > 50 ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_VOLUME_MID));
        lv_bar_set_value(vol_bar, st->volume, LV_ANIM_OFF);
        lv_obj_set_hidden(vol_bar_cont, false);
    } else if (vol_bar_expiry > 0 && lv_tick_elaps(vol_bar_expiry) < 100000) {
        lv_obj_set_hidden(vol_bar_cont, true);
        vol_bar_expiry = 0;
    }

    /* 7. Update Status Bar Icons (Glow Cyan when Connected) */
    if (is_bt_connected) {
        lv_obj_set_style_text_color(home_bt_badge, COLOR_ACCENT, 0);
    } else {
        lv_obj_set_style_text_color(home_bt_badge, lv_color_hex(0x556677), 0);
    }

    char bat_buf[32];
    uint8_t bat_pct = battery_get_percentage();
    snprintf(bat_buf, sizeof(bat_buf), "%s %d%%", battery_get_symbol(), bat_pct);
    lv_label_set_text(home_battery_badge, bat_buf);

    /* 8. Update Bluetooth Screen if Active */
    if (cur_scr == Bluetooth) {
        bt_status_t cur_st = bt_get_status();
        int cur_cnt = bt_get_device_count();

        if (cur_st != last_bt_status || cur_cnt != last_bt_count) {
            last_bt_status = cur_st;
            last_bt_count = cur_cnt;

            if (is_bt_connected) {
                char sbuf[80];
                snprintf(sbuf, sizeof(sbuf), "Connected: %s", bt_get_connected_name());
                lv_label_set_text(bt_status_label, sbuf);
                lv_obj_set_style_text_color(bt_status_label, COLOR_ACCENT, 0);
                lv_obj_set_style_border_color(bt_status_card, COLOR_ACCENT, 0);
            } else if (cur_st == BT_STATUS_SCANNING) {
                lv_label_set_text(bt_status_label, "Scanning for receivers...");
                lv_obj_set_style_text_color(bt_status_label, COLOR_AMBER, 0);
                lv_obj_set_style_border_color(bt_status_card, COLOR_AMBER, 0);
            } else if (cur_st == BT_STATUS_CONNECTING) {
                lv_label_set_text(bt_status_label, "Connecting...");
                lv_obj_set_style_text_color(bt_status_label, COLOR_ROBOT_BLUE, 0);
                lv_obj_set_style_border_color(bt_status_card, COLOR_ROBOT_BLUE, 0);
            } else {
                lv_label_set_text(bt_status_label, "Status: Disconnected");
                lv_obj_set_style_text_color(bt_status_label, COLOR_TEXT_MUTED, 0);
                lv_obj_set_style_border_color(bt_status_card, lv_color_hex(0x333333), 0);
            }

            refresh_bluetooth_list();
        }
    }

    /* 9. Update Settings Screen if Active */
    if (cur_scr == Setting) {
        char s_buf[80];
        snprintf(s_buf, sizeof(s_buf), "Volume: %d%%", st->volume);
        lv_list_set_button_text(set_list, set_vol_label, s_buf);

        snprintf(s_buf, sizeof(s_buf), "Audio Out: %s",
                 is_bt_connected ? bt_get_connected_name() : "None");
        lv_list_set_button_text(set_list, set_bt_label, s_buf);

        snprintf(s_buf, sizeof(s_buf), "SD Storage: %d Songs", song_count);
        lv_list_set_button_text(set_list, set_sd_label, s_buf);

        snprintf(s_buf, sizeof(s_buf), "Battery: %d%% (%u mV)",
                 bat_pct, (unsigned)battery_get_voltage_mv());
        lv_list_set_button_text(set_list, set_bat_label, s_buf);

        snprintf(s_buf, sizeof(s_buf), "RAM Free: %u KB",
                 (unsigned)(esp_get_free_heap_size() / 1024));
        lv_list_set_button_text(set_list, set_ram_label, s_buf);

        float temp_c = temperature_read_celsius();
        snprintf(s_buf, sizeof(s_buf), "Chip Temp: %.1f °C", temp_c);
        lv_list_set_button_text(set_list, set_temp_label, s_buf);

        snprintf(s_buf, sizeof(s_buf), "Power Mode: %s",
                 s_battery_saver ? "Battery Saver (Screen Off)" : "Normal (Active)");
        lv_list_set_button_text(set_list, set_saver_label, s_buf);
    }
}

/* ============================================================
 * GPIO Button Configuration (Strictly 3 Physical Buttons)
 * ============================================================ */
void buttons_gpio_init(void)
{
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << PIN_POWER_SLEEP) |
                        (1ULL << PIN_NAV)         |
                        (1ULL << PIN_ACTION),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&btn_conf);
}

/* ============================================================
 * Button State Machine (Debounce, Short Press, Long Press)
 * ============================================================ */
#define LONG_MS    450
#define REPEAT_MS  120

typedef enum { BTN_NONE, BTN_SHORT, BTN_LONG } btn_evt_t;

typedef struct {
    gpio_num_t pin;
    bool       repeat;
    bool       down;
    bool       long_fired;
    bool       just_pressed;
    uint32_t   t_down;
    uint32_t   t_rep;
    uint32_t   long_ms;
} btn_t;

static btn_evt_t btn_update(btn_t *b)
{
    bool now = (gpio_get_level(b->pin) == 0);   /* Active Low */
    btn_evt_t e = BTN_NONE;

    b->just_pressed = (now && !b->down);

    if (now && !b->down) {
        b->t_down = lv_tick_get();
        b->long_fired = false;
    }
    else if (!now && b->down) {
        if (!b->long_fired) e = BTN_SHORT;
    }
    else if (now && b->down) {
        uint32_t thresh = b->long_ms ? b->long_ms : LONG_MS;
        if (lv_tick_elaps(b->t_down) >= thresh) {
            if (!b->long_fired) {
                b->long_fired = true;
                b->t_rep = lv_tick_get();
                e = BTN_LONG;
            } else if (b->repeat && lv_tick_elaps(b->t_rep) >= REPEAT_MS) {
                b->t_rep = lv_tick_get();
                e = BTN_LONG;
            }
        }
    }
    b->down = now;
    return e;
}

static btn_t b_power  = { .pin = PIN_POWER_SLEEP, .repeat = false, .long_ms = 600 };
static btn_t b_nav    = { .pin = PIN_NAV,         .repeat = false, .long_ms = 450 };
static btn_t b_action = { .pin = PIN_ACTION,      .repeat = false, .long_ms = 450 };

/* ============================================================
 * Unified 3-Button Poll Callback (Runs every 20 ms)
 * ============================================================ */
static void button_poll_cb(lv_timer_t *t)
{
    btn_evt_t power_evt  = btn_update(&b_power);
    btn_evt_t nav_evt    = btn_update(&b_nav);
    btn_evt_t action_evt = btn_update(&b_action);

    /* 1. Multi-click or Any-button Wake from Battery Saver Mode */
    bool any_pressed = (b_power.just_pressed || b_nav.just_pressed || b_action.just_pressed);

    if (any_pressed) {
        uint32_t now = lv_tick_get();
        if (lv_tick_elaps(s_click_window_start) <= 1500) {
            s_multi_clicks++;
        } else {
            s_multi_clicks = 1;
            s_click_window_start = now;
        }

        if (s_battery_saver) {
            /* Wake up display on triple click or any button press */
            if (s_multi_clicks >= 3 || b_power.just_pressed) {
                exit_battery_saver_mode();
                s_multi_clicks = 0;
            }
            return; /* In Battery Saver Mode, ignore action until display is awake */
        }
    }

    if (s_battery_saver) {
        return; /* While screen is asleep in battery saver mode, do not process UI navigation */
    }

    /* 2. Button 1: PIN_POWER_SLEEP (GPIO 32)
     * Single click: Power OFF (Deep Sleep)
     * Hold (>600ms): Sleep Mode / Battery Saver (Screen black, background audio active)
     */
    if (power_evt == BTN_SHORT) {
        ESP_LOGI(TAG, "Power button clicked -> Deep Sleep Power Off");
        power_off_device();
        return;
    }
    if (power_evt == BTN_LONG) {
        ESP_LOGI(TAG, "Power button held -> Entering Sleep Mode / Battery Saver");
        enter_battery_saver_mode();
        return;
    }

    /* 3. Button 2: PIN_NAV (GPIO 33)
     * Single click: Screen Navigation / Advance selection in list
     * Hold (>450ms): Quick Jump / Screen Switcher
     */
    lv_obj_t *scr = lv_screen_active();

    if (nav_evt == BTN_SHORT) {
        if (scr == Home) {
            lv_screen_load(Library);
            update_library_selection();
        } else if (scr == Library) {
            next_song();
            update_library_selection();
        } else if (scr == Bluetooth) {
            int dev_count = bt_get_device_count();
            int total_items = dev_count + 1;
            bt_selected_index = (bt_selected_index + 1) % total_items;
            refresh_bluetooth_list();
        } else if (scr == Setting) {
            set_selected_index = (set_selected_index + 1) % SET_ITEMS_COUNT;
            update_setting_selection();
        }
    }

    if (nav_evt == BTN_LONG) {
        /* Hold Nav:
         * From Home -> Setting
         * From Library -> Bluetooth
         * From Bluetooth -> Setting
         * From Setting -> Home
         */
        if (scr == Home) {
            lv_screen_load(Setting);
            set_selected_index = 0;
            update_setting_selection();
        } else if (scr == Library) {
            lv_screen_load(Bluetooth);
            bt_selected_index = 0;
            refresh_bluetooth_list();
        } else if (scr == Bluetooth) {
            lv_screen_load(Setting);
            set_selected_index = 0;
            update_setting_selection();
        } else {
            lv_screen_load(Home);
        }
    }

    /* 4. Button 3: PIN_ACTION (GPIO 25)
     * Single click: Play/Pause / Select item / Connect / Toggle
     * Hold (>450ms): Toggle Shuffle Mode (or return to Home from Subscreens)
     */
    if (action_evt == BTN_SHORT) {
        if (scr == Home) {
            toggle_play_pause();
        } else if (scr == Library) {
            player_play_selected();
            lv_screen_load(Home);
        } else if (scr == Bluetooth) {
            if (bt_selected_index == 0) {
                bt_start_scan();
                refresh_bluetooth_list();
            } else {
                int dev_idx = bt_selected_index - 1;
                bt_connect_index(dev_idx);
                refresh_bluetooth_list();
            }
        } else if (scr == Setting) {
            if (set_selected_index == 14) {
                /* Battery Saver mode toggle */
                enter_battery_saver_mode();
            } else {
                lv_screen_load(Home);
            }
        }
    }

    if (action_evt == BTN_LONG) {
        if (scr == Home) {
            player_toggle_shuffle();
            bool is_shuf = player_is_shuffle();
            if (home_shuf_badge) {
                lv_label_set_text(home_shuf_badge, is_shuf ? "[SHUF]" : "");
            }
        } else {
            /* From Library, Bluetooth, Setting: Hold Action returns to Home */
            lv_screen_load(Home);
        }
    }
}
