"""Run the real authorization task against a short-lived fake device-code API."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class AuthRefreshTest(unittest.TestCase):
    def test_expiry_clears_old_qr_and_fetches_a_new_code(self):
        source = (ROOT / 'main/kuku_baidu.c').read_text()
        task = source[source.index('static void auth_task('):
                      source.index('int kuku_baidu_auth_start(')]
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static size_t test_strlcpy(char *dst, const char *src, size_t cap) {
 size_t n = strlen(src);
 if (cap) {
  size_t copied = n < cap - 1 ? n : cap - 1;
  memcpy(dst, src, copied);
  dst[copied] = 0;
 }
 return n;
}
#ifdef strlcpy
#undef strlcpy
#endif
#define strlcpy test_strlcpy
#define KUKU_BAIDU_APPKEY "test-key"
#define KUKU_BAIDU_SECRET "test-secret"
#define HTTP_METHOD_GET 0
#define TAG "test"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define pdMS_TO_TICKS(ms) (ms)
#define BD_WAIT_CODE 1
#define BD_READY 2
typedef struct { bool wifi_up; int bd_state; } app_state_t;
typedef struct { const char *valuestring; double valuedouble; int kind; } cJSON;
static app_state_t g_kuku = { .wifi_up = true, .bd_state = BD_WAIT_CODE };
static bool s_auth_task_running = true;
static bool s_auth_cancel;
static char s_user_code[16], s_verify_url[64];
static int now, fetches, polls, uploads, delays, deleted;
static int64_t boot_sec(void) { return now; }
static void vTaskDelay(int ms) { assert(ms > 0); now += ms / 1000; ++delays; }
static void vTaskDelete(void *task) { assert(task == NULL); ++deleted; }
static void set_state(int state) { g_kuku.bd_state = state; }
static void sntp_start_once(void) {}
static int kuku_baidu_upload_pass(void) { ++uploads; return 0; }
static int http_req(const char *url, int method, const char *body, int len,
                    const char *ctype, char *resp, size_t cap, int timeout) {
 (void)method; (void)body; (void)len; (void)ctype; (void)timeout;
 assert(strstr(url, "/device/code"));
 assert(s_user_code[0] == 0 && s_verify_url[0] == 0);
 ++fetches;
 if (fetches == 1) return -1;  // transient fetch failure backs off
 snprintf(resp, cap, "%d", fetches);
 return 0;
}
static cJSON *cJSON_Parse(const char *resp) {
 static cJSON root;
 root.valuedouble = resp[0] - '0';
 return &root;
}
static const cJSON *cJSON_GetObjectItem(const cJSON *root, const char *key) {
 static cJSON device, user, verify, expires, interval;
 if (!strcmp(key, "device_code")) {
  device = (cJSON){ .valuestring = root->valuedouble == 2 ? "device-a" : "device-b", .kind = 1 };
  return &device;
 }
 if (!strcmp(key, "user_code")) {
  user = (cJSON){ .valuestring = root->valuedouble == 2 ? "code-a" : "code-b", .kind = 1 };
  return &user;
 }
 if (!strcmp(key, "verification_url")) {
  verify = (cJSON){ .valuestring = "https://openapi.baidu.com/device", .kind = 1 };
  return &verify;
 }
 if (!strcmp(key, "expires_in")) { expires = (cJSON){ .valuedouble = 3, .kind = 2 }; return &expires; }
 if (!strcmp(key, "interval")) { interval = (cJSON){ .valuedouble = 2, .kind = 2 }; return &interval; }
 return NULL;
}
static bool cJSON_IsString(const cJSON *v) { return v && v->kind == 1; }
static bool cJSON_IsNumber(const cJSON *v) { return v && v->kind == 2; }
static void cJSON_Delete(cJSON *v) { (void)v; }
static int token_exchange(const char *url) {
 ++polls;
 if (strstr(url, "code=device-a")) {
  assert(!strcmp(s_user_code, "code-a"));
  return -2;
 }
 assert(strstr(url, "code=device-b"));
 assert(!strcmp(s_user_code, "code-b"));
 return 0;
}
'''
        checks = r'''
int main(void) {
 strcpy(s_user_code, "expired-code");
 strcpy(s_verify_url, "expired-url");
 auth_task(NULL);
 assert(fetches == 3 && polls == 2);
 assert(now == 15 && delays == 15);  // 10s retry, 2s+1s expiry, 2s success
 assert(g_kuku.bd_state == BD_READY && uploads == 1 && deleted == 1);
 assert(!s_auth_task_running);
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'test.c').write_text(harness + task + checks)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            str(path / 'test.c'), '-o', str(path / 'test')], check=True)
            subprocess.run([str(path / 'test')], check=True)


if __name__ == '__main__':
    unittest.main()
