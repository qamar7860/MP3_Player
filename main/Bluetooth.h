#ifndef MAIN_BLUETOOTH_BLUETOOTH_H_
#define MAIN_BLUETOOTH_BLUETOOTH_H_

#include <stdbool.h>
#include <stdint.h>
#include "esp_bt_defs.h"
#include "esp_gap_bt_api.h"

#define TARGET_NAME     "OnePlus Nord Buds 3 Pro(~49dBm)"
#define BT_MAX_DEVICES  8

typedef struct {
    esp_bd_addr_t bda;
    char          name[64];
    int           rssi;
} bt_dev_item_t;

typedef enum {
    BT_STATUS_DISCONNECTED,
    BT_STATUS_SCANNING,
    BT_STATUS_CONNECTING,
    BT_STATUS_CONNECTED
} bt_status_t;

/* Headphone button action triggered by gestures / AVRCP commands */
typedef enum {
    HP_ACTION_PLAY_PAUSE,  /* Single click (Right or Left earbud) -> Play/Pause */
    HP_ACTION_NEXT_SONG,   /* Double click Right earbud           -> Next Song */
    HP_ACTION_PREV_SONG,   /* Double click Left earbud            -> Previous Song */
    HP_ACTION_VOL_UP,      /* Hold Right earbud                   -> Volume Up */
    HP_ACTION_VOL_DOWN     /* Hold Left earbud                    -> Volume Down */
} hp_action_t;

typedef void (*hp_button_cb_t)(hp_action_t action);

void        bt_init(void);
bool        bt_is_connected(void);
bool        bt_is_scanning(void);
bt_status_t bt_get_status(void);
const char *bt_get_connected_name(void);

void        bt_start_scan(void);
void        bt_stop_scan(void);
int         bt_get_device_count(void);
const bt_dev_item_t *bt_get_device(int idx);
void        bt_connect_index(int idx);
void        bt_disconnect(void);
void        bt_default_headphone_handler(hp_action_t action);
void        bt_set_headphone_callback(hp_button_cb_t cb);

#endif /* MAIN_BLUETOOTH_BLUETOOTH_H_ */
