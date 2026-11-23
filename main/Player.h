#ifndef PLAYER_H
#define PLAYER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VOLUME_MIN   0
#define VOLUME_MAX   100
#define VOLUME_STEP  5

typedef struct {
    volatile bool     playing;       /* true = playing, false = paused/stopped        */
    volatile uint8_t  volume;        /* 0..100, applied to PCM before the ring buffer */
    volatile int      current_song;  /* index of the song actually loaded (-1 = none) */
    volatile uint32_t elapsed_sec;   /* elapsed time of currently playing song        */
    volatile uint32_t duration_sec;  /* estimated total track duration                */
    volatile uint8_t  progress_pct;  /* 0..100 percentage                            */
    volatile uint32_t bitrate_kbps;  /* bitrate in kbps                               */
    volatile uint32_t sample_rate;   /* sample rate in Hz                             */
} player_state_t;

void                  player_init(void);   /* creates ring buffer, queue and player task */
const player_state_t *player_get_state(void);

void toggle_play_pause(void);
void player_stop(void);
void player_next(void);
void player_prev(void);
void player_toggle_shuffle(void);
bool player_is_shuffle(void);
void volume_up(void);
void volume_down(void);
void volume_set(uint8_t vol);
void player_play_selected(void);           /* start the song highlighted in Library */
void player_play_song(int index);          /* start specific song by index */
void player_save_state(void);               /* persist current song and elapsed seconds to NVS */
void player_restore_state(void);            /* restore last played song and progress from NVS */

/* Called from the Bluetooth A2DP data callback.
 * Fills dst with exactly len bytes of 16-bit stereo PCM (silence if paused or empty).
 * Never blocks. Returns len. */
size_t player_read_pcm(uint8_t *dst, size_t len);

#endif /* PLAYER_H */
