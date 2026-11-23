#include "Player.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MINIMP3_IMPLEMENTATION      /* in exactly ONE .c file */
#include "minimp3.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "SD_Card.h"                /* songs[], song_count, selected_song, next_song() */

static const char *TAG = "PLAYER";

/* ---------------- Sizes ---------------- */
#define READ_BUF_SIZE   (4 * 1024)    /* MP3 bytes held in RAM (4 KB)           */
#define REFILL_BELOW    (1536)        /* refill when fewer bytes than this      */
#define RING_SIZE       (16 * 1024)   /* 16 KB PCM ring buffer (~93 ms audio buffer) */
#define PLAYER_STACK    (24 * 1024)   /* 24 KB stack: required for minimp3 16.5 KB scratch struct */
#define PREBUFFER_BYTES (6 * 1024)    /* Prebuffer 6 KB (~35 ms) before starting playback */
#define SAMPLE_RATE_HZ  44100         /* A2DP stream is fixed at 44.1 kHz */

/* ---------------- Commands to the player task ---------------- */
typedef enum { 
	CMD_PLAY 
} cmd_type_t;

typedef struct {
    cmd_type_t type;
    int index;
} player_cmd_t;

/* ---------------- State ---------------- */
static player_state_t state = {
    .playing      = false,
    .volume       = 50,
    .current_song = -1,
    .elapsed_sec  = 0,
    .duration_sec = 0,
    .progress_pct = 0,
    .bitrate_kbps = 0,
    .sample_rate  = SAMPLE_RATE_HZ,
};

static bool s_shuffle = false;
static bool s_prebuffering = false;
static bool s_file_loaded = false;
static uint32_t s_resume_sec = 0;
static uint32_t s_last_saved_sec = 0;
static StreamBufferHandle_t ring  = NULL;   /*  ring: holds the decoded audio op, PCM: player task -> Bluetooth callback */
static QueueHandle_t        cmd_q = NULL;   /* cmd_q: Holds the command from button/ UI, commands: UI -> player task            */

/* ---------------- NVS Playback State Persistence ---------------- */
void player_save_state(void)
{
    nvs_handle_t h;
    if (nvs_open("mp3_state", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "song_idx", (int32_t)selected_song);
        nvs_set_u32(h, "elapsed", state.elapsed_sec);
        nvs_set_u8(h, "vol", state.volume);
        nvs_set_u8(h, "shuf", s_shuffle ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
        s_last_saved_sec = state.elapsed_sec;
    }
}

void player_restore_state(void)
{
    nvs_handle_t h;
    if (nvs_open("mp3_state", NVS_READONLY, &h) == ESP_OK) {
        int32_t saved_idx = 0;
        uint32_t saved_elapsed = 0;
        uint8_t saved_vol = 50;
        uint8_t saved_shuf = 0;

        if (nvs_get_i32(h, "song_idx", &saved_idx) == ESP_OK) {
            if (saved_idx >= 0 && saved_idx < song_count) {
                selected_song = (int)saved_idx;
                state.current_song = (int)saved_idx;
            }
        }
        if (nvs_get_u32(h, "elapsed", &saved_elapsed) == ESP_OK) {
            state.elapsed_sec = saved_elapsed;
            s_resume_sec = saved_elapsed;
        }
        if (nvs_get_u8(h, "vol", &saved_vol) == ESP_OK) {
            if (saved_vol <= VOLUME_MAX) state.volume = saved_vol;
        }
        if (nvs_get_u8(h, "shuf", &saved_shuf) == ESP_OK) {
            s_shuffle = (saved_shuf != 0);
        }
        nvs_close(h);
        ESP_LOGI(TAG, "Restored state: song=%d, elapsed=%u s, vol=%d, shuf=%d",
                 selected_song, (unsigned)state.elapsed_sec, state.volume, s_shuffle);
    }
}

/* ---------------- Diagnostics to identify playback problems (written by the BT task, read by the player task) ---------------- */
static volatile uint32_t underruns = 0;          /* callback found too little data     */
static volatile size_t   min_avail = RING_SIZE;  /* lowest ring fill seen              */

const player_state_t *player_get_state(void)
{
    return &state;
}

void player_toggle_shuffle(void)
{
    s_shuffle = !s_shuffle;
    player_save_state();
    ESP_LOGI(TAG, "Shuffle: %s", s_shuffle ? "ON" : "OFF");
}

bool player_is_shuffle(void)
{
    return s_shuffle;
}

void player_next(void)
{
    if (song_count == 0) return;
    s_resume_sec = 0;
    if (s_shuffle && song_count > 1) {
        int r = rand() % (song_count - 1);
        selected_song = (selected_song + 1 + r) % song_count;
    } else {
        next_song();
    }
    player_play_selected();
}

void player_prev(void)
{
    if (song_count == 0) return;
    s_resume_sec = 0;
    prev_song();
    player_play_selected();
}

/* ============================================================
 * Control functions (called from the UI / button task)
 * ============================================================ */
void player_play_song(int index)
{
    if (song_count == 0) return;
    if (index < 0) index = 0;
    if (index >= song_count) index = song_count - 1;
    selected_song = index;
    s_resume_sec = 0;
    player_play_selected();
}

void player_play_selected(void)
{
    if (song_count == 0) {
        ESP_LOGW(TAG, "No songs available");
        return;
    }
    if (!cmd_q) {
        ESP_LOGE(TAG, "player_init() was not called");
        return;
    }

    state.current_song = selected_song;
    if (s_resume_sec == 0) {
        state.elapsed_sec  = 0;
        state.progress_pct = 0;
    }

    player_cmd_t c = { .type = CMD_PLAY, .index = selected_song };
    xQueueOverwrite(cmd_q, &c);
    player_save_state();
}

void toggle_play_pause(void)
{
    if (song_count == 0) {
        ESP_LOGW(TAG, "No songs available");
        return;
    }

    /* Nothing loaded yet or first playback after boot: start the highlighted/restored song */
    if (state.current_song < 0 || !s_file_loaded) {
        player_play_selected();
        return;
    }

    state.playing = !state.playing;
    s_prebuffering = false;
    ESP_LOGI(TAG, "%s", state.playing ? "Playing" : "Paused");
    if (!state.playing) {
        player_save_state();
    }
}

void player_stop(void)
{
    state.playing = false;
    s_prebuffering = false;
    s_file_loaded = false;
    player_save_state();
    ESP_LOGI(TAG, "STOP");
}

void volume_up(void)
{
    int v = state.volume + VOLUME_STEP;
    state.volume = (v > VOLUME_MAX) ? VOLUME_MAX : v;
    player_save_state();
    ESP_LOGI(TAG, "Volume: %d", state.volume);
}

void volume_down(void)
{
    int v = state.volume - VOLUME_STEP;
    state.volume = (v < VOLUME_MIN) ? VOLUME_MIN : v;
    player_save_state();
    ESP_LOGI(TAG, "Volume: %d", state.volume);
}

void volume_set(uint8_t vol)
{
    state.volume = (vol > VOLUME_MAX) ? VOLUME_MAX : vol;
    player_save_state();
    ESP_LOGI(TAG, "Volume set: %d", state.volume);
}

/* ============================================================
 * Called from the Bluetooth task. Must be fast and must never block.
 * No logging in here.
 * ============================================================ */
size_t player_read_pcm(uint8_t *dst, size_t len)
{
    size_t got = 0;

    if (state.playing && ring) {
        size_t avail = xStreamBufferBytesAvailable(ring);

        /* diagnostics */
        if (avail < len) underruns++;
        if (avail < min_avail) min_avail = avail;

        size_t want = (avail < len ? avail : len) & ~(size_t)3;  /* whole stereo frames only */
        if (want) {
            got = xStreamBufferReceive(ring, dst, want, 0);
        }
    }

    if (got < len) {
        memset(dst + got, 0, len - got);     /* paused or under run: silence */
    }
    return len;
}

/* ============================================================
 * Player task internals
 * ============================================================ */
static FILE *open_song(int index)
{
    if (index < 0 || index >= song_count) return NULL;

    const char *path = song_get_path(index);
    if (!path || path[0] == '\0') return NULL;

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open %s", path);
        return NULL;
    }
    ESP_LOGI(TAG, "Now playing: %s", song_get_name(index));
    return f;
}

/* Write all bytes into the ring buffer. Blocks while it is full,
 * but gives up instantly if a new command is waiting, so track changes are instantaneous. */
static void ring_write(const uint8_t *data, size_t len)
{
    while (len > 0) {
        if (uxQueueMessagesWaiting(cmd_q) > 0) {
            return;
        }
        size_t n = xStreamBufferSend(ring, data, len, pdMS_TO_TICKS(15));
        data += n;
        len  -= n;
    }
}

static void player_task(void *arg)
{
    /* Permanent buffers: allocated once, on the heap (not the stack) */
    uint8_t  *buf = malloc(READ_BUF_SIZE);
    int16_t  *pcm = malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t));
    mp3dec_t *dec = malloc(sizeof(mp3dec_t));
    if (!buf || !pcm || !dec) {
        ESP_LOGE(TAG, "malloc failed, player disabled");
        vTaskDelete(NULL);
        return;
    }

    FILE   *f = NULL;
    size_t  fill = 0, pos = 0;
    bool    eof = false, checked = false;
    bool    is_wav = false;
    int     wav_channels = 2;
    uint32_t wav_byte_rate = 176400;
    int     bad_streak = 0;
    mp3dec_frame_info_t info;
    player_cmd_t cmd;

    /* diagnostics */
    int64_t max_read_us = 0, max_dec_us = 0;
    int64_t last_log = esp_timer_get_time();

    int64_t total_samples = 0;
    long    file_size = 0;

    for (;;) {

        /* Idle (no file): sleep until a command arrives.
         * Playing: just peek at the queue without waiting. */
        TickType_t wait = f ? 0 : portMAX_DELAY;
        if (xQueueReceive(cmd_q, &cmd, wait) == pdTRUE) {
            if (f) { fclose(f); f = NULL; }
            state.playing = false; /* Pause while opening file and prebuffering */

            if (cmd.type == CMD_PLAY) {
                f = open_song(cmd.index);
                if (f) {
                    s_file_loaded = true;
                    if (ring) xStreamBufferReset(ring);
                    mp3dec_init(dec);
                    fill = pos = 0;
                    eof = checked = false;
                    is_wav = false;
                    total_samples = (int64_t)s_resume_sec * SAMPLE_RATE_HZ;
                    s_prebuffering = true;
                    state.playing = false;

                    /* Measure file size for duration estimation */
                    fseek(f, 0, SEEK_END);
                    file_size = ftell(f);
                    fseek(f, 0, SEEK_SET);

                    state.elapsed_sec  = s_resume_sec;
                    state.duration_sec = 0;
                    state.progress_pct = 0;

                    /* Check if WAV file */
                    const char *fpath = song_get_path(cmd.index);
                    const char *ext = fpath ? strrchr(fpath, '.') : NULL;
                    if (ext && strcasecmp(ext, ".wav") == 0) {
                        uint8_t hdr[44];
                        if (fread(hdr, 1, 44, f) == 44 && memcmp(hdr, "RIFF", 4) == 0 && memcmp(hdr + 8, "WAVE", 4) == 0) {
                            uint16_t audio_fmt = hdr[20] | (hdr[21] << 8);
                            uint16_t channels  = hdr[22] | (hdr[23] << 8);
                            uint32_t srate     = hdr[24] | (hdr[25] << 8) | (hdr[26] << 16) | (hdr[27] << 24);
                            uint16_t bits      = hdr[34] | (hdr[35] << 8);
                            uint32_t data_sz   = hdr[40] | (hdr[41] << 8) | (hdr[42] << 16) | (hdr[43] << 24);

                            if (audio_fmt == 1 && bits == 16 && (channels == 1 || channels == 2)) {
                                is_wav = true;
                                wav_channels = channels;
                                state.bitrate_kbps = (uint32_t)((srate * channels * 16) / 1000);
                                state.sample_rate  = srate;
                                wav_byte_rate      = srate * channels * 2;
                                if (wav_byte_rate > 0 && data_sz > 0) {
                                    state.duration_sec = data_sz / wav_byte_rate;
                                } else if (file_size > 44 && wav_byte_rate > 0) {
                                    state.duration_sec = (uint32_t)((file_size - 44) / wav_byte_rate);
                                }
                                ESP_LOGI(TAG, "WAV Audio: %d Hz, %d ch, 16-bit, %d kbps (duration: %u s)",
                                         (int)srate, (int)channels, (int)state.bitrate_kbps, (unsigned)state.duration_sec);
                                if (s_resume_sec > 0 && wav_byte_rate > 0) {
                                    long seek_byte = 44 + (long)s_resume_sec * wav_byte_rate;
                                    if (seek_byte < file_size) {
                                        fseek(f, seek_byte, SEEK_SET);
                                        total_samples = (int64_t)s_resume_sec * SAMPLE_RATE_HZ;
                                        state.elapsed_sec = s_resume_sec;
                                        if (state.duration_sec > 0) {
                                            state.progress_pct = (uint8_t)((state.elapsed_sec * 100) / state.duration_sec);
                                        }
                                    }
                                }
                            }
                        }
                    }
                } else {
                    s_file_loaded = false;
                    state.playing = false;
                    s_prebuffering = false;
                }
            }
            continue;
        }
        if (!f) continue;

        /* If paused by user, yield CPU */
        if (!state.playing && !s_prebuffering) {
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }

        /* ---- diagnostics: print every 2 seconds ---- */
        if (esp_timer_get_time() - last_log > 2000000) {
            last_log = esp_timer_get_time();
            ESP_LOGI(TAG, "underruns=%u min_ring=%u max_sd_read=%lld us max_decode=%lld us",
                     (unsigned)underruns, (unsigned)min_avail,
                     (long long)max_read_us, (long long)max_dec_us);
            max_read_us = max_dec_us = 0;
            min_avail = RING_SIZE;
        }

        bool ended = false;
        bool bad   = false;

        if (is_wav) {
            /* Raw uncompressed studio WAV streaming directly into ring buffer */
            size_t samples_to_read = 576;
            size_t bytes_to_read = samples_to_read * wav_channels * sizeof(int16_t);
            size_t n = fread(pcm, 1, bytes_to_read, f);
            if (n == 0) {
                if (feof(f)) {
                    ended = true;
                } else if (ferror(f)) {
                    ESP_LOGW(TAG, "SD WAV read glitch, retrying...");
                    clearerr(f);
                }
            } else {
                int samples_read = (int)(n / (wav_channels * sizeof(int16_t)));
                if (wav_channels == 1) {
                    for (int i = samples_read - 1; i >= 0; i--) {
                        pcm[2 * i]     = pcm[i];
                        pcm[2 * i + 1] = pcm[i];
                    }
                }
                int count = samples_read * 2;
                total_samples += samples_read;
                state.elapsed_sec = (uint32_t)(total_samples / SAMPLE_RATE_HZ);
                if (state.duration_sec > 0) {
                    uint32_t pct = (state.elapsed_sec * 100) / state.duration_sec;
                    state.progress_pct = (pct > 100) ? 100 : (uint8_t)pct;
                }

                uint8_t vol = state.volume;
                if (vol < VOLUME_MAX) {
                    for (int i = 0; i < count; i++) {
                        pcm[i] = (int16_t)(((int32_t)pcm[i] * vol) / 100);
                    }
                }

                ring_write((const uint8_t *)pcm, count * sizeof(int16_t));

                static uint32_t s_last_periodic_save_wav = 0;
                if (state.playing && state.elapsed_sec != s_last_periodic_save_wav && (state.elapsed_sec % 4 == 0)) {
                    s_last_periodic_save_wav = state.elapsed_sec;
                    player_save_state();
                }

                if (s_prebuffering && xStreamBufferBytesAvailable(ring) >= PREBUFFER_BYTES) {
                    s_prebuffering = false;
                    state.playing = true;
                }
            }
        } else {
            /* ---- keep the MP3 read buffer topped up ---- */
            if (!eof && (fill - pos) < REFILL_BELOW) {
                memmove(buf, buf + pos, fill - pos);
                fill -= pos;
                pos = 0;

                int64_t rd0 = esp_timer_get_time();
                size_t n = fread(buf + fill, 1, READ_BUF_SIZE - fill, f);
                int64_t rdt = esp_timer_get_time() - rd0;
                if (rdt > max_read_us) max_read_us = rdt;

                if (n == 0) {
                    if (feof(f)) {
                        eof = true;
                    } else if (ferror(f)) {
                        ESP_LOGW(TAG, "SD MP3 read glitch, retrying...");
                        clearerr(f);
                    }
                }
                fill += n;
            }

            if (fill == pos) {
                if (eof) {
                    ended = true;                          /* truly ran out of data */
                }
            } else {
                int64_t dc0 = esp_timer_get_time();
                int samples = mp3dec_decode_frame(dec, buf + pos, (int)(fill - pos), pcm, &info);
                int64_t dct = esp_timer_get_time() - dc0;
                if (dct > max_dec_us) max_dec_us = dct;

                if (info.frame_bytes == 0) {
                    if (eof) {
                        ended = true;                      /* Reached EOF with no more valid frames */
                    } else {
                        /* Incomplete frame in buffer or sync mismatch: refill or resync */
                        if ((fill - pos) < (READ_BUF_SIZE / 2)) {
                            memmove(buf, buf + pos, fill - pos);
                            fill -= pos;
                            pos = 0;
                            size_t n = fread(buf + fill, 1, READ_BUF_SIZE - fill, f);
                            if (n == 0 && feof(f)) eof = true;
                            fill += n;
                        } else {
                            pos++;                         /* Advance 1 byte to resync on next frame header */
                        }
                    }
                } else {
                    pos += info.frame_bytes;

                    if (samples > 0) {                 /* a real audio frame */

                        if (!checked) {
                            checked = true;
                            state.bitrate_kbps = info.bitrate_kbps;
                            state.sample_rate  = info.hz;
                            if (info.bitrate_kbps > 0 && file_size > 0) {
                                state.duration_sec = (uint32_t)(((uint64_t)file_size * 8) / (info.bitrate_kbps * 1000));
                            }
                            ESP_LOGI(TAG, "Format: %d Hz, %d ch, %d kbps (est duration: %u s)",
                                     info.hz, info.channels, info.bitrate_kbps, (unsigned)state.duration_sec);
                            if (info.hz != SAMPLE_RATE_HZ ||
                                (info.channels != 1 && info.channels != 2)) {
                                ESP_LOGW(TAG, "Unsupported format (%d Hz, %d ch) - skipping",
                                         info.hz, info.channels);
                                ended = true;
                                bad   = true;
                            }

                            /* If resuming saved playback position, seek directly */
                            if (!ended && s_resume_sec > 0 && info.bitrate_kbps > 0) {
                                uint32_t seek_sec = s_resume_sec;
                                s_resume_sec = 0; /* Clear so subsequent plays start from 0 */
                                long seek_byte = (long)(((uint64_t)seek_sec * info.bitrate_kbps * 1000) / 8);
                                if (file_size > 16384 && seek_byte > (file_size - 16384)) {
                                    seek_byte = file_size - 16384;
                                }
                                if (fseek(f, seek_byte, SEEK_SET) == 0) {
                                    fill = 0;
                                    pos = 0;
                                    total_samples = (int64_t)seek_sec * SAMPLE_RATE_HZ;
                                    state.elapsed_sec = seek_sec;
                                    if (state.duration_sec > 0) {
                                        state.progress_pct = (uint8_t)((state.elapsed_sec * 100) / state.duration_sec);
                                    }
                                    ESP_LOGI(TAG, "Resumed playback at %u sec (byte offset %ld)", (unsigned)seek_sec, seek_byte);
                                    continue; /* Read next buffer at new seek point */
                                }
                            }
                        }

                        if (!ended) {
                            /* mono -> stereo, in place, from the end backwards */
                            if (info.channels == 1) {
                                for (int i = samples - 1; i >= 0; i--) {
                                    int16_t s = pcm[i];
                                    pcm[2 * i]     = s;
                                    pcm[2 * i + 1] = s;
                                }
                            }
                            int count = samples * 2;   /* int16 values, interleaved L/R */

                            /* track playback time */
                            total_samples += samples;
                            state.elapsed_sec = (uint32_t)(total_samples / SAMPLE_RATE_HZ);
                            if (state.duration_sec > 0) {
                                uint32_t pct = (state.elapsed_sec * 100) / state.duration_sec;
                                state.progress_pct = (pct > 100) ? 100 : (uint8_t)pct;
                            }

                            /* software volume */
                            uint8_t vol = state.volume;
                            if (vol < VOLUME_MAX) {
                                for (int i = 0; i < count; i++) {
                                    pcm[i] = (int16_t)(((int32_t)pcm[i] * vol) / 100);
                                }
                            }

                            ring_write((const uint8_t *)pcm, count * sizeof(int16_t));

                            /* Periodic state save to NVS every ~4 seconds */
                            static uint32_t s_last_periodic_save = 0;
                            if (state.playing && state.elapsed_sec != s_last_periodic_save && (state.elapsed_sec % 4 == 0)) {
                                s_last_periodic_save = state.elapsed_sec;
                                player_save_state();
                            }

                            /* Prebuffering: once ring has enough audio, enable playback */
                            if (s_prebuffering && xStreamBufferBytesAvailable(ring) >= PREBUFFER_BYTES) {
                                s_prebuffering = false;
                                state.playing = true;
                            }
                        }
                    }
                    /* samples == 0 with frame_bytes > 0 = skipped ID3 tag / junk, not an error */
                }
            }
        }

        /* ---- song finished (or unplayable): go to the next one ---- */
        if (ended) {
            fclose(f);
            f = NULL;
            s_file_loaded = false;
            s_resume_sec = 0;
            state.elapsed_sec  = 0;
            state.progress_pct = 0;
            ESP_LOGI(TAG, "%s", bad ? "Skipped unplayable song" : "Song finished");

            bad_streak = bad ? bad_streak + 1 : 0;
            if (bad_streak >= song_count) {
                ESP_LOGE(TAG, "No playable songs");
                state.playing      = false;
                state.current_song = -1;
                continue;
            }

            if (s_shuffle && song_count > 1) {
                int r = rand() % (song_count - 1);
                selected_song = (selected_song + 1 + r) % song_count;
            } else {
                next_song();
            }
            state.current_song = selected_song;

            player_cmd_t c = { .type = CMD_PLAY, .index = selected_song };
            xQueueOverwrite(cmd_q, &c);
        }
    }
}

/* ============================================================
 * Init
 * ============================================================ */
void player_init(void)
{
    state.playing      = false;
    state.volume       = 50;
    state.current_song = -1;

    ring  = xStreamBufferCreate(RING_SIZE, 1);
    cmd_q = xQueueCreate(1, sizeof(player_cmd_t));
    if (!ring || !cmd_q) {
        ESP_LOGE(TAG, "Cannot create ring buffer or queue");
        return;
    }

    player_restore_state();

    /* Pinned to Core 1 at Priority 6 (strictly higher than UI) so audio decoding is never starved */
    xTaskCreatePinnedToCore(player_task, "player", PLAYER_STACK, NULL, 6, NULL, 1);

    ESP_LOGI(TAG, "Player ready. Free heap: %u, min ever: %u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size());
}
