#include <stdio.h>
#include <stdint.h>
#include <sys/stat.h>
#include <string.h>
#include <strings.h>
 #include "esp_err.h"
#include "esp_vfs_fat.h"
#include "hal/spi_types.h"
 #include "sdmmc_cmd.h"
 #include "driver/sdspi_host.h"
 #include "dirent.h"     // allows us to use opendir() and readdir()
#include "soc/gpio_num.h"
#include "driver/spi_common.h"
#include "driver/spi_master.h"
#include "SD_Card.h"
#include "driver/gpio.h"
#include "esp_log.h"

int song_count = 0;
int selected_song = 0;

static sdmmc_card_t *card;

/* ---- In-RAM High-Performance Playlist Index (Instantaneous O(1), Zero SD Contention) ---- */
static mp3_file_t *s_songs = NULL;

const mp3_file_t *song_get(int index)
{
    if (!s_songs || index < 0 || index >= song_count) return NULL;
    return &s_songs[index];
}

const char *song_get_name(int index)
{
    const mp3_file_t *s = song_get(index);
    return s ? s->name : "Unknown Track";
}

const char *song_get_path(int index)
{
    const mp3_file_t *s = song_get(index);
    return s ? s->path : "";
}
 
 
 
 /**
 cs  15
 mosi  13
 miso  12
 sck  14
 */
 
 spi_bus_config_t bus_config = {        // for the bus
	.mosi_io_num = 13,
	.miso_io_num = 12,
	.sclk_io_num = 14,
	.quadwp_io_num = -1,
	.quadhd_io_num = -1,
	.max_transfer_sz = 16384,
 };
 
 /*===========================================To initialize the sd card====================================*/
 
 esp_err_t sdcard_init(void){
	
	esp_err_t ret;
	
	   /* Pulling up SPI lines to stabilize signals on jumper wires */
	       gpio_set_pull_mode(12, GPIO_PULLUP_ONLY); // MISO
	       gpio_set_pull_mode(13, GPIO_PULLUP_ONLY); // MOSI
	       gpio_set_pull_mode(15, GPIO_PULLUP_ONLY); // CS

	
	sdmmc_host_t host = SDSPI_HOST_DEFAULT();
	host.slot = SPI2_HOST;
	host.max_freq_khz = 10000;
	 
	ret  = spi_bus_initialize(SPI2_HOST, &bus_config, SPI_DMA_CH_AUTO);
	
	if (ret != ESP_OK)
	   {
	       return ret;
	   }
	
	sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
	slot_config.gpio_cs = 15;
	slot_config.host_id  = SPI2_HOST;
	
	esp_vfs_fat_sdmmc_mount_config_t mount_config = {
	.format_if_mount_failed = false,     // if mounting fails dont format the sd card and lose all the files 
	.max_files = 5,                      // Maximum simultaneously open files.
	.allocation_unit_size = 0,           // filesystem driver choose the appropriate allocation unit size.
	};
	
	ret = esp_vfs_fat_sdspi_mount(
		"/sdcard",
		 &host,
		 &slot_config, 
		 &mount_config, 
	   	 &card
	 );
	 if (ret == ESP_OK) {
	     ESP_LOGI("SD", "SPI clock: %d kHz (card max %d kHz, host max %d kHz)",
	              card->real_freq_khz, card->max_freq_khz, host.max_freq_khz);
	     sdmmc_card_print_info(stdout, card);
	 }

	 return ret;
 };
 
 
 static void scan_dir_recursive(const char *base_path, int depth)
{
    if (depth > 3 || song_count >= MAX_SONGS) return;

    DIR *dir = opendir(base_path);
    if (dir == NULL) {
        ESP_LOGE("MP3", "Failed to open %s", base_path);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char full_path[300];
        snprintf(full_path, sizeof(full_path), "%s/%s", base_path, entry->d_name);

        struct stat st;
        if (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            /* Skip art folder from recursive song scan */
            if (strcasecmp(entry->d_name, "art") != 0) {
                scan_dir_recursive(full_path, depth + 1);
            }
        } else {
            const char *dot = strrchr(entry->d_name, '.');
            if (dot != NULL && (strcasecmp(dot, ".mp3") == 0 || strcasecmp(dot, ".wav") == 0)) {
                if (song_count >= MAX_SONGS) {
                    ESP_LOGW("MP3", "Song limit reached (%d)", MAX_SONGS);
                    break;
                }
                mp3_file_t *rec = &s_songs[song_count];
                memset(rec, 0, sizeof(mp3_file_t));
                strncpy(rec->name, entry->d_name, sizeof(rec->name) - 1);
                strncpy(rec->path, full_path, sizeof(rec->path) - 1);

                if (song_count < 10 || (song_count % 50) == 0) {
                    ESP_LOGI("MP3", "[%d] Found %s: %s", song_count, dot, rec->path);
                }
                song_count++;
            }
        }
    }
    closedir(dir);
}

void scan_mp3_files(void)
{
    /* Clean up any legacy index file on SD card */
    remove("/sdcard/.playlist.idx");

    if (s_songs) {
        free(s_songs);
        s_songs = NULL;
    }
    song_count = 0;

    s_songs = malloc(MAX_SONGS * sizeof(mp3_file_t));
    if (!s_songs) {
        ESP_LOGE("MP3", "Failed to allocate memory for song playlist");
        return;
    }

    scan_dir_recursive("/sdcard", 0);

    ESP_LOGI("MP3", "Total audio files indexed on SD: %d", song_count);

    if (song_count == 0) {
        ESP_LOGW("MP3", "No MP3 or WAV files found");
        free(s_songs);
        s_songs = NULL;
        return;
    }

    /* Shrink allocated memory to exact song count to free unused DRAM */
    mp3_file_t *shrink = realloc(s_songs, song_count * sizeof(mp3_file_t));
    if (shrink) {
        s_songs = shrink;
    }
    ESP_LOGI("MP3", "Playlist cached in RAM (%u bytes, %d tracks)",
             (unsigned)(song_count * sizeof(mp3_file_t)), song_count);
}

void next_song(void)
{
    if (song_count == 0) {
        ESP_LOGW("MP3", "No songs available");
        return;
    }

    selected_song++;
    if (selected_song >= song_count) {
        selected_song = 0;
    }

    ESP_LOGI("MP3", "Selected song: %d - %s", selected_song, song_get_name(selected_song));
}

void prev_song(void)
{
    if (song_count == 0) {
        ESP_LOGW("MP3", "No songs available");
        return;
    }

    if (selected_song == 0) {
        selected_song = song_count - 1;
    } else {
        selected_song--;
    }

    ESP_LOGI("MP3", "Selected song: %d - %s", selected_song, song_get_name(selected_song));
}
 
 




