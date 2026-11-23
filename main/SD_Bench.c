/* SD read speed benchmark. Run once at boot, BEFORE lvgl_init() and bt_init(),
 * so nothing else is competing. Reads the first song several different ways. */

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "SD_Card.h"                /* songs[], song_count */

static const char *TAG = "SDBENCH";

#define BENCH_BYTES   (128 * 1024)  /* read this much per test */
#define BUF_SIZE      (16 * 1024)

static void report(const char *name, size_t bytes, int64_t us)
{
    if (us <= 0) us = 1;
    long long kbps = ((long long)bytes * 1000000LL / us) / 1024;
    ESP_LOGI(TAG, "%-30s %4u KB in %5lld ms = %5lld KB/s",
             name, (unsigned)(bytes / 1024), (long long)(us / 1000), kbps);
}

/* fread with a given chunk size and optional stdio buffer size (0 = default) */
static void test_fread(const char *path, uint8_t *buf, size_t chunk, size_t vbuf, const char *name)
{
    FILE *f = fopen(path, "rb");
    if (!f) { ESP_LOGE(TAG, "open failed"); return; }
    if (vbuf) setvbuf(f, NULL, _IOFBF, vbuf);

    size_t total = 0;
    int64_t t0 = esp_timer_get_time();
    while (total < BENCH_BYTES) {
        size_t n = fread(buf, 1, chunk, f);
        if (n == 0) break;
        total += n;
    }
    report(name, total, esp_timer_get_time() - t0);
    fclose(f);
}

/* POSIX read() straight to the FAT layer */
static void test_posix(const char *path, uint8_t *buf, size_t chunk, const char *name)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) { ESP_LOGE(TAG, "open failed"); return; }

    size_t total = 0;
    int64_t t0 = esp_timer_get_time();
    while (total < BENCH_BYTES) {
        ssize_t n = read(fd, buf, chunk);
        if (n <= 0) break;
        total += n;
    }
    report(name, total, esp_timer_get_time() - t0);
    close(fd);
}

void sd_bench(void)
{
    if (song_count == 0) {
        ESP_LOGW(TAG, "No songs, skipping benchmark");
        return;
    }

    uint8_t *buf = malloc(BUF_SIZE);
    if (!buf) { ESP_LOGE(TAG, "malloc failed"); return; }

    const char *path = song_get_path(0);
    ESP_LOGI(TAG, "Benchmarking: %s", path);

    test_fread(path, buf, 512,   0,    "fread 512 B, default buf");
    test_fread(path, buf, 6144,  0,    "fread 6 KB, default buf");
    test_fread(path, buf, 16384, 4096, "fread 16 KB, setvbuf 4 KB");
    test_posix(path, buf, 16384,       "read() 16 KB");
    test_posix(path, buf, 4096,        "read() 4 KB");

    free(buf);
    ESP_LOGI(TAG, "Benchmark done. Need at least 40 KB/s for 256 kbps audio.");
}

