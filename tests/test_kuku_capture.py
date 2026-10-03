"""Exercise the actual capture worker's stop, overflow, and I2S failure paths."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class CaptureTest(unittest.TestCase):
    def test_capture_stops_without_silently_ignoring_overflow(self):
        source = (ROOT / 'main/kuku_rec.c').read_text()
        worker = source[source.index('static void capture_task('):source.index('// Close each completed segment')]
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define KUKU_REC_CHUNK_BYTES 4096
#define KUKU_REC_MAX_SECONDS 120
#define ESP_OK 0
#define TAG "test"
#define ESP_LOGE(...) ((void)0)
#define pdMS_TO_TICKS(ms) (ms)
static struct { bool wifi_up; uint32_t rec_ms; } g_kuku;
static bool s_rec_stop_req, s_capture_failed, s_capture_done;
static uint32_t s_capture_bytes;
static void *s_pcm_stream;
static int reads, deletes, mode;
static int64_t now_us;
static int64_t esp_timer_get_time(void) { return now_us; }
static void vTaskDelete(void *task) { assert(!task); ++deletes; }
static int bsp_audio_read(void *pcm, size_t bytes) {
 ++reads; assert(bytes == 4096); memset(pcm, reads, bytes);
 now_us += mode == 4 ? 120000000 : 128000;
 if (mode == 1) return -1;
 if (mode == 3) g_kuku.wifi_up = false;
 if (reads == 3) s_rec_stop_req = true;
 return ESP_OK;
}
static size_t xStreamBufferSend(void *stream, const void *pcm, size_t bytes, int wait) {
 assert(stream == s_pcm_stream && wait == 50);
 assert(((const unsigned char *)pcm)[0] == reads);
 return mode == 2 ? bytes / 2 : bytes;
}
static void reset(int selected) {
 mode = selected; reads = deletes = 0; now_us = 0;
 s_rec_stop_req = s_capture_failed = s_capture_done = false;
 s_capture_bytes = 0; g_kuku.wifi_up = true; g_kuku.rec_ms = 0;
}
'''
        checks = r'''
int main(void) {
 reset(0); capture_task(NULL);
 assert(reads == 3 && s_capture_bytes == 12288 && !s_capture_failed && s_capture_done && deletes == 1);
 reset(1); capture_task(NULL);
 assert(reads == 1 && s_capture_bytes == 0 && s_capture_failed && s_capture_done);
 reset(2); capture_task(NULL);
 assert(reads == 1 && s_capture_bytes == 2048 && s_capture_failed && s_capture_done);
 reset(3); capture_task(NULL);
 assert(reads == 1 && s_capture_bytes == 4096 && !s_capture_failed && s_capture_done); // Network loss saves and stops.
 reset(4); capture_task(NULL);
 assert(reads == 3 && g_kuku.rec_ms == 360000 && s_capture_done); // No old two-minute session cutoff.
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'test.c').write_text(harness + worker + checks)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            str(path / 'test.c'), '-o', str(path / 'test')], check=True)
            subprocess.run([str(path / 'test')], check=True)


if __name__ == '__main__':
    unittest.main()
