"""Exercise the actual Baidu HTTP helpers with a bounded fake client pool."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class HttpLifetimeTest(unittest.TestCase):
    def test_requests_release_clients_on_success_and_failure(self):
        source = (ROOT / 'main/kuku_baidu.c').read_text()
        http_section = source.index('// ---- HTTPS(')
        helpers = source[source.index('static int http_req(', http_section):source.index('// ---- MD5')]
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#define KUKU_BAIDU_CA_PEM "test-ca"
#define esp_crt_bundle_attach ((void *)1)
#define TAG "test"
#define KUKU_BD_IOBUF 4096
#define KUKU_BD_UPLOAD_IOBUF 1024
#define KUKU_BD_WRITE_TIMEOUT_MS 3000
#define KUKU_BD_BODY_BUDGET_MS 40000
static int64_t clock_us;
static int slow_write, socket_fail;
static int64_t esp_timer_get_time(void) { return clock_us; }
#define pdMS_TO_TICKS(ms) (ms)
static void vTaskDelay(int ms) { assert(ms > 0); }
#define ESP_OK 0
#define ESP_ERR_NO_MEM -2
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
static struct { bool recording; } g_kuku;
static void fake_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
#define ESP_LOGW(...) fake_log(__VA_ARGS__)
typedef int esp_err_t;
static const char *esp_err_to_name(int e) { (void)e; return "test"; }
typedef int esp_http_client_method_t;
#define HTTP_METHOD_GET 0
#define HTTP_METHOD_POST 1
typedef struct {
 const char *url, *cert_pem;
 void *crt_bundle_attach;
 int method, timeout_ms, buffer_size, buffer_size_tx;
} esp_http_client_config_t;
typedef struct { int closed, timeout; } *esp_http_client_handle_t;
typedef struct { const char *valuestring; } cJSON;
static int live, created, cleaned, init_fail, open_fail, short_write, status = 200;
static esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg) {
 assert(cfg->url);
 if (init_fail || live >= 4) return NULL;
 esp_http_client_handle_t c = calloc(1, sizeof(*c));
 assert(c); c->timeout=cfg->timeout_ms; ++live; ++created; return c;
}
static void esp_http_client_set_header(esp_http_client_handle_t c, const char *a, const char *b) {
 (void)c; (void)a; (void)b;
}
static void esp_http_client_set_post_field(esp_http_client_handle_t c, const char *b, int n) {
 (void)c; (void)b; (void)n;
}
static int esp_http_client_open(esp_http_client_handle_t c, int n) {
 (void)c; (void)n; return open_fail ? -1 : ESP_OK;
}
static int esp_http_client_get_socket(esp_http_client_handle_t c) { assert(c); return 7; }
#define setsockopt fake_setsockopt
static int fake_setsockopt(int fd,int level,int option,const void *value,socklen_t size) {
 const struct timeval *tv=value;
 assert(fd==7 && level==SOL_SOCKET && option==SO_SNDTIMEO);
 assert(size==sizeof(*tv) && tv->tv_sec==3 && tv->tv_usec==0);
 return socket_fail ? -1 : 0;
}
static int esp_http_client_set_timeout_ms(esp_http_client_handle_t c, int ms) {
 assert(ms==3000 || ms==15000); c->timeout=ms; return 0;
}
static int esp_http_client_write(esp_http_client_handle_t c, const char *b, int n) {
 (void)b;
 if(slow_write && n<=64) { assert(c->timeout==3000); clock_us+=3000000; return 1; }
 return short_write ? 0 : n;
}
static int esp_http_client_fetch_headers(esp_http_client_handle_t c) { (void)c; return 2; }
static int esp_http_client_read_response(esp_http_client_handle_t c, char *b, int n) {
 (void)c; assert(n >= 2); memcpy(b, "{}", 2); return 2;
}
static int esp_http_client_read(esp_http_client_handle_t c, char *b, int n) {
 (void)c; (void)b; (void)n; return 0;
}
static int esp_http_client_get_status_code(esp_http_client_handle_t c) { (void)c; return status; }
static void esp_http_client_close(esp_http_client_handle_t c) { c->closed = 1; }
static void esp_http_client_cleanup(esp_http_client_handle_t c) {
 assert(c->closed); free(c); --live; ++cleaned;
}
static cJSON *cJSON_Parse(const char *s) { (void)s; return NULL; }
static cJSON *cJSON_GetObjectItem(const cJSON *o, const char *s) { (void)o; (void)s; return NULL; }
static int cJSON_IsString(const cJSON *o) { (void)o; return 0; }
static void cJSON_Delete(cJSON *o) { (void)o; }
'''
        checks = r'''
int main(void) {
 char response[32];
 g_kuku.recording = true;
 // Repeated polling must not exhaust the four-slot client pool.
 for (int i = 0; i < 100; ++i) {
  assert(http_req("test", HTTP_METHOD_GET, NULL, 0, NULL, response, sizeof(response), 1) == 0);
  assert(live == 0);
 }
 status = 400;
 for (int i = 0; i < 100; ++i) {
  assert(http_req("test", HTTP_METHOD_GET, NULL, 0, NULL, response, sizeof(response), 1) == 400);
  assert(live == 0);
 }
 status = 200;
 open_fail = 1; strcpy(response, "stale-response");
 assert(http_req("test", 0, NULL, 0, NULL, response, sizeof(response), 1) == -1);
 assert(live == 0 && response[0] == 0); open_fail = 0;
 short_write = 1;
 assert(http_req("test", 1, "body", 4, "test", response, sizeof(response), 1) == -1);
 assert(live == 0); short_write = 0;
 init_fail = 1;
 assert(http_req("test", 0, NULL, 0, NULL, response, sizeof(response), 1) == -1);
 assert(live == 0); init_fail = 0;
 FILE *f = tmpfile(); assert(f); fputs("data", f);
 for (int i = 0; i < 100; ++i) {
  rewind(f);
  assert(http_upload_part("test", f, 4, response, sizeof(response)) == 0);
  assert(live == 0);
 }
 rewind(f);
 assert(http_upload_part("test", f, 16, response, sizeof(response)) == -1);
 assert(live == 0);
 open_fail = 1;
 assert(http_upload_part("test", f, 4, response, sizeof(response)) == -1);
 assert(live == 0); open_fail = 0;
 short_write = 1; rewind(f);
 assert(http_upload_part("test", f, 4, response, sizeof(response)) == -1);
 assert(live == 0); short_write = 0;
 status = 401; rewind(f);
 assert(http_upload_part("test", f, 4, response, sizeof(response)) == 401);
 assert(live == 0);
 socket_fail=1; status=200; rewind(f);
 assert(http_upload_part("test", f, 4, response, sizeof(response))==-1);
 assert(live==0); socket_fail=0;
 // A stream that still advances very slowly must hit the total body budget;
 // release the failed client, then recover on the next normal connection.
 fseek(f,0,SEEK_END); for(int i=0;i<60;i++) fputc(0,f); fflush(f);
 status=200; slow_write=1; clock_us=0; rewind(f);
 assert(http_upload_part("test", f, 64, response, sizeof(response))==-1);
 assert(live==0 && clock_us>=40000000 && clock_us<45000000);
 slow_write=0; rewind(f);
 assert(http_upload_part("test", f, 4, response, sizeof(response))==0);
 assert(live==0);
 fclose(f); assert(created == cleaned);
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'test.c').write_text(harness + helpers + checks)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            str(path / 'test.c'), '-o', str(path / 'test')], check=True)
            subprocess.run([str(path / 'test')], check=True)


if __name__ == '__main__':
    unittest.main()
