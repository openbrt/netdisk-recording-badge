"""Run production upload/probe scheduling with recordings closing mid-batch."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ParallelTest(unittest.TestCase):
    def test_upload_snapshot_and_record_admission(self):
        source = (ROOT / 'main/kuku_baidu.c').read_text()
        probe = source[source.index('static void record_probe_task('):source.index('// ---- 授权任务')]
        upload = source[source.index('static void upload_task('):source.index('// ---- UI 读取接口')]
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define KUKU_MAX_NAME 32
#define KUKU_BD_MIN_RECORD_KB 256
#define KUKU_BD_UPLOAD_ATTEMPTS 3
#define KUKU_BD_UPLOAD_STACK 12288
#define KUKU_BD_PROBE_STACK 8192
#define BD_READY 2
#define BD_NO_AUTH 0
#define BD_UPLOADING 3
#define WIFI_PS_NONE 0
#define WIFI_PS_MIN_MODEM 1
#define pdPASS 1
#define pdMS_TO_TICKS(ms) (ms)
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define TAG "test"
static struct { bool wifi_up, recording; int bd_state; } g_kuku;
static int s_cloud_mux, s_cloud_status, s_cloud_count;
static bool s_cloud_has_more;
static char s_access[160] = "authorized", s_up_name[KUKU_MAX_NAME];
static int s_up_done, s_up_total;
static int64_t s_at_exp_boot;
static bool s_probe_uses_network, s_probe_task_running, s_upload_task_running;
static bool s_auth_task_running, s_upload_requested, s_reset_running;
static int free_kb = 4096, probes, starts, creates, failures, upload_fail, appended;
static bool create_fail;
static void (*next_task)(void *);
static char catalog[8][KUKU_MAX_NAME];
static int catalog_count;
static char sent[8][KUKU_MAX_NAME];
static int sent_count;
static char status[128];
int kuku_baidu_upload_pass(void);
void kuku_baidu_on_recording_saved(void);
static void set_state(int state) { g_kuku.bd_state = state; }
static void esp_wifi_set_ps(int ps) { (void)ps; }
static void vTaskDelay(int ms) { assert(ms > 0); }
static void vTaskDelete(void *p) { assert(!p); }
static void kuku_ui_set_status(const char *s) { snprintf(status, sizeof(status), "%s", s); }
static int token_refresh(void) { return 0; }
static int kuku_rec_count(void) { return catalog_count; }
static int kuku_rec_free_kb(void) { return free_kb; }
static int kuku_rec_clean_synced(void) { return 0; }
static void kuku_rec_refresh(void) {}
static bool upload_host_reachable(void) { ++probes; return true; }
static bool account_reachable(void) { ++probes; return true; }
static int kuku_rec_start(void) { ++starts; g_kuku.recording = true; return 0; }
static int prepare_recording_directory(const char *name) { (void)name; return 0; }
#ifdef strlcpy
#undef strlcpy
#endif
#define strlcpy test_strlcpy
static size_t test_strlcpy(char *d, const char *s, size_t n) {
 size_t len = strlen(s); if (n) snprintf(d, n, "%s", s); return len;
}
static int xTaskCreate(void (*fn)(void *), const char *name, int stack,
                       void *arg, int pri, void *handle) {
 (void)name; (void)stack; (void)arg; (void)pri; (void)handle;
 ++creates; if (create_fail) return 0; next_task = fn; return pdPASS;
}
static int kuku_rec_snapshot(char (*names)[KUKU_MAX_NAME], int cap) {
 int n = catalog_count < cap ? catalog_count : cap;
 for (int i = 0; i < n; ++i) strlcpy(names[i], catalog[i], KUKU_MAX_NAME);
 return n;
}
static int upload_file(const char *name) {
 assert(g_kuku.recording); // Upload must keep going during capture.
 if (upload_fail) { ++failures; return -1; }
 strlcpy(sent[sent_count++], name, KUKU_MAX_NAME);
 // Remove A immediately: a live index loop would skip B after this mutation.
 for (int i = 0; i < catalog_count; ++i) {
  if (!strcmp(catalog[i], name)) {
   memmove(catalog + i, catalog + i + 1, (size_t)(--catalog_count - i) * sizeof(*catalog));
   break;
  }
 }
 if (!appended) {
  strlcpy(catalog[catalog_count++], "C.WAV", KUKU_MAX_NAME);
  // Use realistic names (production rejects too-short names).
  strlcpy(catalog[catalog_count - 1], "REC00003.WAV", KUKU_MAX_NAME);
  appended = 1; kuku_baidu_on_recording_saved();
 }
 return 0;
}
'''
        checks = r'''
int main(void) {
 g_kuku.wifi_up = true; g_kuku.bd_state = BD_UPLOADING;
 s_upload_task_running = true;
 // A pending/uploading file must not reject another segment or spawn TLS.
 strcpy(catalog[0], "REC00001.WAV"); catalog_count = 1;
 assert(kuku_baidu_request_recording() == 0);
 assert(!s_probe_uses_network);
 assert(kuku_baidu_request_recording() == -3); // single outstanding probe
 next_task(NULL); assert(starts == 1 && probes == 0);
 s_upload_task_running = false; s_upload_requested = false;
 g_kuku.bd_state = BD_READY;
 strcpy(catalog[1], "REC00002.WAV"); catalog_count = 2;
 assert(kuku_baidu_upload_pass() == 0); // recording=true still accepted
 assert(kuku_baidu_upload_pass() == -3); // no duplicate uploader
 s_upload_requested = false;
 next_task(NULL);
 assert(sent_count == 2);
 assert(!strcmp(sent[0], "REC00001.WAV") && !strcmp(sent[1], "REC00002.WAV"));
 assert(catalog_count == 1 && s_upload_task_running); // C queued in next batch
 next_task(NULL);
 assert(sent_count == 3 && !strcmp(sent[2], "REC00003.WAV"));
 assert(catalog_count == 0 && !s_upload_task_running && !s_upload_requested);
 // A failed network batch leaves the file pending and capture running.
 strcpy(catalog[0], "REC00004.WAV"); catalog_count = 1; upload_fail = 1;
 assert(kuku_baidu_upload_pass() == 0); next_task(NULL);
 assert(failures == 3 && catalog_count == 1 && s_upload_requested);
 assert(g_kuku.recording && !s_upload_task_running && g_kuku.bd_state == BD_READY);
 // Task allocation failure must release reservation and keep retry pending.
 create_fail = true; assert(kuku_baidu_upload_pass() == -4);
 assert(!s_upload_task_running && s_upload_requested);
 // Low space blocks only the new recording; pending upload survives.
 create_fail = false; g_kuku.recording = false; free_kb = 128;
 assert(kuku_baidu_request_recording() == 0); next_task(NULL);
 assert(starts == 1 && !s_probe_task_running);
 // No-network rejection does not dispatch a worker.
 g_kuku.wifi_up = false; int before = creates;
 assert(kuku_baidu_request_recording() == -2 && creates == before);
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'test.c').write_text(harness + probe + upload + checks)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            str(path / 'test.c'), '-o', str(path / 'test')], check=True)
            subprocess.run([str(path / 'test')], check=True)


if __name__ == '__main__':
    unittest.main()
