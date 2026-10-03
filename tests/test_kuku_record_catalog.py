"""Exercise production catalog locking and interrupted-WAV recovery on real files."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class CatalogTest(unittest.TestCase):
    def test_active_file_hidden_and_closed_snapshot_survives_changes(self):
        source = (ROOT / 'main/kuku_rec.c').read_text()
        catalog = source[source.index('struct kuku_rec_entry {'):source.index('long kuku_rec_free_kb(')]
        recovery = source[source.index('static void recover_recordings('):source.index('// ---- 初始化')]
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include "kuku_wav.h"
#define KUKU_MAX_NAME 32
#define KUKU_MAX_FILES 200
#define KUKU_SAMPLE_RATE_HZ 16000
#define REC_MOUNT_POINT "rec"
#define portMAX_DELAY 0
#define ESP_LOGI(...) ((void)0)
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t *s_catalog_mutex = &mutex;
static void xSemaphoreTake(pthread_mutex_t *m, int timeout) { (void)timeout; assert(!pthread_mutex_lock(m)); }
static void xSemaphoreGive(pthread_mutex_t *m) { assert(!pthread_mutex_unlock(m)); }
#ifdef strlcpy
#undef strlcpy
#endif
#define strlcpy test_strlcpy
static size_t test_strlcpy(char *d, const char *s, size_t cap) {
 size_t len = strlen(s); if (cap) snprintf(d, cap, "%s", s); return len;
}
static void create(const char *path, int bytes) {
 FILE *f = fopen(path, "wb"); assert(f);
 uint8_t hdr[KUKU_WAV_HDR]; kuku_wav_fill(hdr, 16000, 1, 16, 0);
 assert(fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr));
 for (int i = 0; i < bytes; ++i) assert(fputc(i & 255, f) != EOF);
 assert(!fclose(f));
}
'''
        checks = r'''
static void *publisher(void *arg) {
 (void)arg;
 for (int i = 0; i < 200; ++i) {
  xSemaphoreTake(s_catalog_mutex, 0);
  create("rec/REC00002.WAV", 1024);
  cache_invalidate(); xSemaphoreGive(s_catalog_mutex);
  assert(!kuku_rec_mark_uploaded("REC00002.WAV"));
 }
 return NULL;
}
int main(void) {
 assert(!mkdir("rec", 0700));
 create("rec/REC00001.WAV", 2048);
 create("rec/REC00003.REC", 4096); // Still writing: must not enter catalog.
 char snapshot[4][KUKU_MAX_NAME], copy[KUKU_MAX_NAME];
 assert(kuku_rec_count() == 1 && kuku_rec_snapshot(snapshot, 4) == 1);
 assert(!strcmp(snapshot[0], "REC00001.WAV"));
 assert(kuku_rec_name_copy(0, copy, sizeof(copy)) && !strcmp(copy, snapshot[0]));
 assert(!kuku_rec_name_copy(1, copy, sizeof(copy)) && !copy[0]);
 assert(!kuku_rec_mark_uploaded("REC00001.WAV"));
 assert(kuku_rec_count() == 0 && !strcmp(snapshot[0], "REC00001.WAV"));
 // Boot recovers only interrupted .REC, patches length, then publishes.
 create("rec/KIMG.TMP", 1024);
 recover_recordings(); kuku_rec_refresh();
 assert(kuku_rec_count() == 1);
 FILE *f = fopen("rec/REC00003.WAV", "rb"); assert(f);
 uint8_t hdr[KUKU_WAV_HDR]; assert(fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr)); fclose(f);
 uint32_t data = hdr[40] | (uint32_t)hdr[41] << 8 | (uint32_t)hdr[42] << 16 | (uint32_t)hdr[43] << 24;
 assert(data == 4096 && access("rec/REC00003.REC", F_OK) != 0);
 assert(access("rec/KIMG.TMP", F_OK) == 0);
 // An existing WAV must never be overwritten by recovery.
 create("rec/REC00003.REC", 8192); recover_recordings();
 assert(access("rec/REC00003.REC", F_OK) == 0);
 pthread_t thread; assert(!pthread_create(&thread, NULL, publisher, NULL));
 for (int i = 0; i < 200; ++i) {
  int n = kuku_rec_snapshot(snapshot, 4);
  assert(n >= 1 && n <= 2);
  for (int j = 0; j < n; ++j) assert(!strcmp(snapshot[j] + strlen(snapshot[j]) - 4, ".WAV"));
 }
 assert(!pthread_join(thread, NULL));
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'test.c').write_text(harness + catalog + recovery + checks)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                            '-I' + str(ROOT / 'main'), str(path / 'test.c'),
                            str(ROOT / 'main/kuku_wav.c'), '-o', str(path / 'test')], check=True)
            subprocess.run([str(path / 'test')], cwd=path, check=True)


if __name__ == '__main__':
    unittest.main()
