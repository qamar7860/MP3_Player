#ifndef MAIN_SD_CARD_H_
#define MAIN_SD_CARD_H_
#include "esp_err.h"

#include <stdint.h>

#define MAX_SONGS 100

typedef struct
{
    char name[48];
    char path[80];
} mp3_file_t;

extern int song_count;
extern int selected_song;

esp_err_t sdcard_init(void);
void scan_mp3_files(void);
void next_song(void);
void prev_song(void);

/* In-RAM SD index accessors (instantaneous, zero SD bus contention) */
const mp3_file_t *song_get(int index);
const char *song_get_name(int index);
const char *song_get_path(int index);

#endif /* MAIN_SD_CARD_H_ */
