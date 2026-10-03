"""Production function C-stub unit tests for KuKu Netdisk Reset and Auth-Cancel functionality."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class BaiduResetTest(unittest.TestCase):
    def setUp(self):
        self.source_bd = (ROOT / 'main/kuku_baidu.c').read_text()
        self.source_ui = (ROOT / 'main/kuku_ui.c').read_text()

    def test_production_reset_and_auth_cancel_c_harness(self):
        # Extract reset_task, kuku_baidu_reset_start, kuku_baidu_reset_status from main/kuku_baidu.c
        bd_reset_code = self.source_bd[
            self.source_bd.index('static void reset_task(void *arg)'):
            self.source_bd.index('// ---- 分片响应')
        ]

        # Extract auth_task from main/kuku_baidu.c
        bd_auth_code = self.source_bd[
            self.source_bd.index('static void auth_task(void *arg)'):
            self.source_bd.index('int kuku_baidu_auth_start(void)')
        ]

        # Extract reset UI functions from main/kuku_ui.c
        ui_reset_code = self.source_ui[
            self.source_ui.index('static bool s_reset_confirm;'):
            self.source_ui.index('// ---- 操作指南')
        ]

        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KUKU_MAX_NAME 32
#define KUKU_CLOUD_PAGE_SIZE 10
#define KUKU_CLOUD_NAME_MAX 96
#define KUKU_CLOUD_PATH_MAX 128
#define KUKU_CLOUD_ROOT "/apps/网盘录音工牌"
#define KUKU_BD_AUTH_STACK 12288

#define BD_NO_AUTH 0
#define BD_WAIT_CODE 1
#define BD_READY 2
#define BD_UPLOADING 3

#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NVS_NOT_FOUND 0x1102
typedef int esp_err_t;
typedef uint32_t nvs_handle_t;
#define NVS_READWRITE 1

#define pdPASS 1
#define pdMS_TO_TICKS(ms) (ms)
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))

#define TAG "test"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)

#define COLOR_ACCENT 0x1
#define COLOR_PANEL2 0x2
#define COLOR_BG     0x3
#define COLOR_TEXT   0x4
#define LV_OBJ_FLAG_HIDDEN 0x1

#define KUKU_BAIDU_APPKEY "test-key"
#define KUKU_BAIDU_SECRET "test-secret"
#define HTTP_METHOD_GET 0

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

typedef struct {
    uint64_t fs_id;
    uint64_t size;
    uint32_t mtime;
    bool is_dir;
    char name[KUKU_CLOUD_NAME_MAX];
} kuku_cloud_file_t;

typedef struct {
    volatile bool recording;
    volatile bool playing;
    volatile bool screen_off;
    volatile bool wifi_up;
    volatile uint8_t bd_state;
    volatile uint32_t rec_ms;
    volatile uint32_t rec_bytes;
    volatile uint8_t volume;
} kuku_state_t;

static kuku_state_t g_kuku;

// Mock FreeRTOS state
static bool s_auth_task_running;
static bool s_auth_cancel;
static bool s_reset_running;
static int s_reset_status;
static bool s_upload_task_running;
static bool s_upload_requested;
static bool s_probe_task_running;
static bool s_probe_uses_network;
static portMUX_TYPE s_cloud_mux;
static kuku_cloud_file_t s_cloud_files[KUKU_CLOUD_PAGE_SIZE];
static int s_cloud_page, s_cloud_count, s_cloud_status;
static bool s_cloud_has_more, s_cloud_task_running;
static char s_cloud_dir[KUKU_CLOUD_PATH_MAX];
static int s_image_status;

static char s_access[160];
static char s_refresh[160];
static int64_t s_at_exp_boot;
static char s_user_code[16];
static char s_verify_url[64];
static char s_up_name[KUKU_MAX_NAME];
static int s_up_done, s_up_total;

static void set_state(uint8_t st) { g_kuku.bd_state = st; }

static void (*g_task_fn)(void *) = NULL;
static void *g_task_arg = NULL;
static bool g_xtaskcreate_fail = false;

static int xTaskCreate(void (*fn)(void *), const char *name, int stack,
                       void *arg, int pri, void *handle) {
    (void)name; (void)stack; (void)pri; (void)handle;
    if (g_xtaskcreate_fail) return 0;
    g_task_fn = fn;
    g_task_arg = arg;
    return pdPASS;
}

static int g_delay_count = 0;
static int g_simulated_time = 0;
static void (*g_delay_hook)(void) = NULL;

static void vTaskDelay(int ms) {
    g_delay_count++;
    g_simulated_time += ms / 1000 > 0 ? ms / 1000 : 1;
    if (g_delay_hook) g_delay_hook();
}

static void vTaskDelete(void *h) {
    (void)h;
}

// Mock NVS
static bool g_nvs_open_fail = false;
static bool g_nvs_erase_fail = false;
static bool g_nvs_commit_fail = false;
static char g_nvs_opened_ns[32] = {0};
static bool g_nvs_erased = false;
static bool g_nvs_committed = false;
static bool g_nvs_closed = false;

static esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *h) {
    (void)mode;
    strncpy(g_nvs_opened_ns, name, sizeof(g_nvs_opened_ns) - 1);
    if (g_nvs_open_fail) return ESP_FAIL;
    *h = 1001;
    return ESP_OK;
}

static esp_err_t nvs_erase_all(nvs_handle_t h) {
    if (h != 1001) return ESP_FAIL;
    if (g_nvs_erase_fail) return ESP_FAIL;
    g_nvs_erased = true;
    return ESP_OK;
}

static esp_err_t nvs_commit(nvs_handle_t h) {
    if (h != 1001) return ESP_FAIL;
    if (g_nvs_commit_fail) return ESP_FAIL;
    g_nvs_committed = true;
    return ESP_OK;
}

static void nvs_close(nvs_handle_t h) {
    if (h == 1001) g_nvs_closed = true;
}

// Stubs for auth_task dependencies
static int g_uploads_count = 0;
static void sntp_start_once(void) {}
static int kuku_baidu_upload_pass(void) { g_uploads_count++; return 0; }
static int64_t boot_sec(void) { return g_simulated_time; }

typedef struct {
    const char *valuestring;
    double valuedouble;
    int kind; // 1 = string, 2 = number
} cJSON;

static int g_http_mode = 0; // 0: normal code, 1: fail code fetch
static int g_token_return_code = -2; // -2: pending, 0: success
static int g_token_exchange_calls = 0;
static void (*g_token_exchange_hook)(void) = NULL;

static int http_req(const char *url, int method, const char *body, int len,
                    const char *ctype, char *resp, size_t cap, int timeout) {
    (void)url; (void)method; (void)body; (void)len; (void)ctype; (void)timeout;
    if (g_http_mode == 1) return -1;
    snprintf(resp, cap, "{\"device_code\":\"dev123\",\"user_code\":\"usr456\","
                        "\"verification_url\":\"https://openapi.baidu.com/device\","
                        "\"expires_in\":300,\"interval\":5}");
    return 0;
}

static cJSON *cJSON_Parse(const char *resp) {
    static cJSON root;
    if (!resp || !resp[0]) return NULL;
    return &root;
}

static const cJSON *cJSON_GetObjectItem(const cJSON *root, const char *key) {
    static cJSON dc = { .valuestring = "dev123", .kind = 1 };
    static cJSON uc = { .valuestring = "usr456", .kind = 1 };
    static cJSON vu = { .valuestring = "https://openapi.baidu.com/device", .kind = 1 };
    static cJSON ex = { .valuedouble = 300, .kind = 2 };
    static cJSON iv = { .valuedouble = 5, .kind = 2 };

    if (!root) return NULL;
    if (!strcmp(key, "device_code")) return &dc;
    if (!strcmp(key, "user_code")) return &uc;
    if (!strcmp(key, "verification_url")) return &vu;
    if (!strcmp(key, "expires_in")) return &ex;
    if (!strcmp(key, "interval")) return &iv;
    return NULL;
}

static bool cJSON_IsString(const cJSON *v) { return v && v->kind == 1; }
static bool cJSON_IsNumber(const cJSON *v) { return v && v->kind == 2; }
static void cJSON_Delete(cJSON *v) { (void)v; }

static int token_exchange(const char *url) {
    (void)url;
    g_token_exchange_calls++;
    if (g_token_exchange_hook) g_token_exchange_hook();
    return g_token_return_code;
}

// Forward declaration of production auth_task for harness hooks
static void auth_task(void *arg);

// Stubs for UI & LVGL
typedef struct lv_obj_t {
    int flags;
    char text[128];
    uint32_t bg_color;
    uint32_t text_color;
    struct lv_obj_t *child;
} lv_obj_t;

static lv_obj_t g_mock_msg;
static lv_obj_t g_mock_other_labels[8];
static int g_label_idx = 0;
static lv_obj_t g_mock_child0, g_mock_child1;
static lv_obj_t g_mock_choice0 = { .child = &g_mock_child0 };
static lv_obj_t g_mock_choice1 = { .child = &g_mock_child1 };

static int kuku_font_14 = 14;
static int kuku_font_20 = 20;

static lv_obj_t *make_title(const char *t) { (void)t; return NULL; }
static lv_obj_t *make_panel(int x, int y, int w, int h) { (void)x;(void)y;(void)w;(void)h; return NULL; }
static lv_obj_t *make_label(lv_obj_t *p, const char *txt, int x, int y, int w, int c, void *f) {
    (void)p;(void)x;(void)y;(void)w;(void)c;(void)f;
    lv_obj_t *obj;
    if (txt && strstr(txt, "清除工牌网盘授权")) {
        obj = &g_mock_msg;
    } else {
        obj = &g_mock_other_labels[g_label_idx++ % 8];
    }
    if (txt) strncpy(obj->text, txt, sizeof(obj->text) - 1);
    return obj;
}
static lv_obj_t *make_box(lv_obj_t *p, int x, int y, int w, int h, int c, int r) {
    (void)p;(void)x;(void)y;(void)w;(void)h;(void)c;(void)r;
    static int idx = 0;
    return (idx++ % 2 == 0) ? &g_mock_choice0 : &g_mock_choice1;
}
static void make_hint(const char *h) { (void)h; }
static void center(lv_obj_t *o) { (void)o; }
static void lv_obj_set_style_text_line_space(lv_obj_t *o, int sp, int sel) { (void)o;(void)sp;(void)sel; }
static void lv_label_set_text(lv_obj_t *o, const char *t) {
    if (o && t) strncpy(o->text, t, sizeof(o->text) - 1);
}
static void lv_obj_add_flag(lv_obj_t *o, int f) { if (o) o->flags |= f; }
static void lv_obj_remove_flag(lv_obj_t *o, int f) { if (o) o->flags &= ~f; }
static uint32_t lv_color_hex(uint32_t c) { return c; }
static void lv_obj_set_style_bg_color(lv_obj_t *o, uint32_t c, int sel) { (void)sel; if (o) o->bg_color = c; }
static void lv_obj_set_style_text_color(lv_obj_t *o, uint32_t c, int sel) { (void)sel; if (o) o->text_color = c; }
static lv_obj_t *lv_obj_get_child(lv_obj_t *o, int idx) { (void)idx; return o ? o->child : NULL; }
'''

        tests = r'''
static void reset_all_mocks(void) {
    memset(&g_kuku, 0, sizeof(g_kuku));
    g_kuku.wifi_up = true;
    g_kuku.bd_state = BD_READY;
    s_auth_task_running = false;
    s_auth_cancel = false;
    s_reset_running = false;
    s_reset_status = 0;
    s_upload_task_running = false;
    s_upload_requested = false;
    s_probe_task_running = false;
    s_probe_uses_network = false;
    s_cloud_task_running = false;
    s_image_status = 0;

    strcpy(s_access, "mock_access_token_123");
    strcpy(s_refresh, "mock_refresh_token_456");
    s_at_exp_boot = 999999;
    strcpy(s_user_code, "USERCODE");
    strcpy(s_verify_url, "https://example.com/qr");
    strcpy(s_up_name, "REC00001.WAV");
    s_up_done = 1;
    s_up_total = 2;

    g_task_fn = NULL;
    g_task_arg = NULL;
    g_xtaskcreate_fail = false;
    g_delay_count = 0;
    g_simulated_time = 100;
    g_delay_hook = NULL;
    g_uploads_count = 0;
    g_http_mode = 0;
    g_token_return_code = -2;
    g_token_exchange_calls = 0;
    g_token_exchange_hook = NULL;

    g_nvs_open_fail = false;
    g_nvs_erase_fail = false;
    g_nvs_commit_fail = false;
    memset(g_nvs_opened_ns, 0, sizeof(g_nvs_opened_ns));
    g_nvs_erased = false;
    g_nvs_committed = false;
    g_nvs_closed = false;

    memset(&g_mock_msg, 0, sizeof(g_mock_msg));
    memset(&g_mock_choice0, 0, sizeof(g_mock_choice0));
    memset(&g_mock_choice1, 0, sizeof(g_mock_choice1));
    g_mock_choice0.child = &g_mock_child0;
    g_mock_choice1.child = &g_mock_child1;
}

// 1. Verify UI default cancel and navigation
static void test_ui_default_cancel(void) {
    reset_all_mocks();
    reset_build();
    assert(kuku_ui_reset_confirmed() == false);
    assert(kuku_ui_reset_status() == 0);

    kuku_ui_reset_move();
    assert(kuku_ui_reset_confirmed() == true);
    kuku_ui_reset_move();
    assert(kuku_ui_reset_confirmed() == false);
}

// 2. Verify NVS namespace isolation: only kuku_bd namespace is opened and erased
static void test_nvs_isolation(void) {
    reset_all_mocks();
    int rc = kuku_baidu_reset_start();
    assert(rc == 0);
    assert(s_reset_running == true);
    assert(s_reset_status == 1);
    assert(g_task_fn != NULL);

    g_task_fn(g_task_arg);

    assert(strcmp(g_nvs_opened_ns, "kuku_bd") == 0);
    assert(g_nvs_erased == true);
    assert(g_nvs_committed == true);
    assert(g_nvs_closed == true);

    assert(s_access[0] == 0);
    assert(s_refresh[0] == 0);
    assert(s_at_exp_boot == 0);
    assert(s_user_code[0] == 0);
    assert(s_verify_url[0] == 0);
    assert(s_up_name[0] == 0);
    assert(s_up_done == 0 && s_up_total == 0);
    assert(g_kuku.bd_state == BD_NO_AUTH);
    assert(s_reset_status == 2);
    assert(s_reset_running == false);
}

// 3. Verify busy rejections for all active operations
static void test_busy_rejection(void) {
    reset_all_mocks();
    g_kuku.recording = true;
    assert(kuku_baidu_reset_start() == -3);

    reset_all_mocks();
    g_kuku.playing = true;
    assert(kuku_baidu_reset_start() == -3);

    reset_all_mocks();
    s_upload_task_running = true;
    assert(kuku_baidu_reset_start() == -3);

    reset_all_mocks();
    s_probe_task_running = true;
    assert(kuku_baidu_reset_start() == -3);

    reset_all_mocks();
    s_cloud_task_running = true;
    assert(kuku_baidu_reset_start() == -3);

    reset_all_mocks();
    s_image_status = 1;
    assert(kuku_baidu_reset_start() == -3);

    reset_all_mocks();
    s_image_status = 3;
    assert(kuku_baidu_reset_start() == -3);

    reset_all_mocks();
    s_reset_running = true;
    assert(kuku_baidu_reset_start() == -3);
}

// 4. Verify auth_task real cancellation during device-code fetch backoff
static void cancel_during_code_fetch_hook(void) {
    if (g_delay_count >= 1) {
        s_auth_cancel = true; // User triggers reset while auth_task is in backoff delay
    }
}

static void test_real_auth_task_cancel_during_code_fetch(void) {
    reset_all_mocks();
    g_kuku.bd_state = BD_WAIT_CODE;
    s_auth_task_running = true;
    s_auth_cancel = false;
    g_http_mode = 1; // cause device/code fetch failure
    g_delay_hook = cancel_during_code_fetch_hook;

    auth_task(NULL);

    assert(s_auth_cancel == true);
    assert(!s_auth_task_running);
    assert(g_kuku.bd_state == BD_WAIT_CODE); // did NOT advance to BD_READY
    assert(g_uploads_count == 0); // did NOT start upload
}

// 5. Verify auth_task real cancellation during polling loop
static void cancel_during_polling_hook(void) {
    if (g_delay_count >= 1) {
        s_auth_cancel = true; // User triggers reset while polling token
    }
}

static void test_real_auth_task_cancel_during_polling(void) {
    reset_all_mocks();
    g_kuku.bd_state = BD_WAIT_CODE;
    s_auth_task_running = true;
    s_auth_cancel = false;
    g_http_mode = 0; // device/code succeeds
    g_token_return_code = -2; // token pending
    g_delay_hook = cancel_during_polling_hook;

    auth_task(NULL);

    assert(s_auth_cancel == true);
    assert(!s_auth_task_running);
    assert(g_kuku.bd_state == BD_WAIT_CODE);
    assert(g_uploads_count == 0);
}

// 6. Verify auth_task real cancellation race with token_exchange return:
// Test starts with s_auth_cancel=false, real auth_task issues device-code and enters token_exchange.
// Right when token_exchange returns 0, the cancellation hook flips s_auth_cancel=true.
static void cancel_at_token_exchange_hook(void) {
    // Flipped to true inside the token_exchange stub right when response arrives
    s_auth_cancel = true;
}

static void test_real_auth_task_cancel_race_with_token_exchange(void) {
    reset_all_mocks();
    g_kuku.bd_state = BD_WAIT_CODE;
    s_auth_task_running = true;
    s_auth_cancel = false; // Start FALSE so auth_task genuinely enters while loop!
    g_http_mode = 0;
    g_token_return_code = 0; // token exchange succeeds
    g_token_exchange_hook = cancel_at_token_exchange_hook;

    // Run real production auth_task
    auth_task(NULL);

    // Assert token_exchange was genuinely invoked by auth_task
    assert(g_token_exchange_calls == 1);
    // In production auth_task: if (rc == 0 && !s_auth_cancel)
    // Since s_auth_cancel was set to true at exchange return, it MUST NOT set BD_READY or call upload!
    assert(!s_auth_task_running);
    assert(g_kuku.bd_state != BD_READY);
    assert(g_kuku.bd_state == BD_WAIT_CODE);
    assert(g_uploads_count == 0);
}

// 7. Verify reset_task waits for real auth_task to exit on cancel:
// In delay hook, invoke the REAL auth_task(NULL). Since s_auth_cancel was set by reset_start,
// the real auth_task notices the cancellation, completes its cleanup, and clears s_auth_task_running.
static void delay_hook_drive_real_auth_task(void) {
    assert(g_nvs_erased == false); // NVS must not be erased while auth_task is still running!
    assert(s_auth_cancel == true);
    // Real auth_task executes and notices s_auth_cancel == true, then exits naturally
    auth_task(NULL);
    assert(!s_auth_task_running); // Verifies real auth_task cleared its own running flag
}

static void test_reset_task_waits_for_auth_task_exit(void) {
    reset_all_mocks();
    s_auth_task_running = true;
    g_delay_hook = delay_hook_drive_real_auth_task;

    int rc = kuku_baidu_reset_start();
    assert(rc == 0);
    assert(s_auth_cancel == true);
    assert(g_task_fn != NULL);

    // Run real production reset_task
    g_task_fn(g_task_arg);

    assert(g_nvs_erased == true);
    assert(s_reset_status == 2);
    assert(g_kuku.bd_state == BD_NO_AUTH);
    assert(s_reset_running == false);
}

// 8. Verify NVS failure does not falsely report success
static void test_nvs_failures(void) {
    reset_all_mocks();
    g_nvs_open_fail = true;
    kuku_baidu_reset_start();
    g_task_fn(g_task_arg);
    assert(s_reset_status == -1);
    assert(g_kuku.bd_state == BD_READY);
    assert(s_reset_running == false);

    reset_all_mocks();
    g_nvs_erase_fail = true;
    kuku_baidu_reset_start();
    g_task_fn(g_task_arg);
    assert(s_reset_status == -1);
    assert(s_reset_running == false);

    reset_all_mocks();
    g_nvs_commit_fail = true;
    kuku_baidu_reset_start();
    g_task_fn(g_task_arg);
    assert(s_reset_status == -1);
    assert(s_reset_running == false);
}

// 9. Verify task creation failure restores state
static void test_task_creation_failure(void) {
    reset_all_mocks();
    g_xtaskcreate_fail = true;
    int rc = kuku_baidu_reset_start();
    assert(rc == -4);
    assert(s_reset_running == false);
    assert(s_auth_cancel == false);
    assert(s_reset_status == -1);
}

// 10. Verify UI navigation and status guard:
// - While reset is ongoing (status=1), exiting and re-entering reset_build preserves ongoing display.
// - Once reset is done (status=2), re-entering reset_build ignores stale status 2 and shows default cancel.
static void test_stale_status_and_ongoing_ui_guard(void) {
    reset_all_mocks();

    // 10a: Ongoing reset (status=1)
    s_reset_status = 1;
    reset_build();
    assert(kuku_ui_reset_status() == 1);
    assert(strstr(g_mock_msg.text, "正在重置") != NULL);
    assert((g_mock_choice0.flags & LV_OBJ_FLAG_HIDDEN) != 0); // choices hidden while resetting

    // 10b: Finished reset (status=2)
    s_reset_status = 2;
    reset_build();
    // Must report 0, NOT 2!
    assert(kuku_ui_reset_status() == 0);
    assert(kuku_ui_reset_confirmed() == false);
    assert((g_mock_choice0.flags & LV_OBJ_FLAG_HIDDEN) == 0); // choices shown for new action
    assert(strstr(g_mock_msg.text, "清除工牌网盘授权") != NULL);
}

// 11. Verify reauthorization and subsequent second reset
static void test_reauthorization_and_second_reset(void) {
    reset_all_mocks();

    assert(kuku_baidu_reset_start() == 0);
    g_task_fn(g_task_arg);
    assert(s_reset_status == 2);
    assert(g_kuku.bd_state == BD_NO_AUTH);

    g_kuku.wifi_up = true;
    g_kuku.bd_state = BD_READY;
    strcpy(s_access, "new_token_789");

    assert(kuku_baidu_reset_start() == 0);
    assert(s_reset_status == 1);
    g_task_fn(g_task_arg);
    assert(s_reset_status == 2);
    assert(g_kuku.bd_state == BD_NO_AUTH);
    assert(s_access[0] == 0);
}

int main(void) {
    test_ui_default_cancel();
    test_nvs_isolation();
    test_busy_rejection();
    test_real_auth_task_cancel_during_code_fetch();
    test_real_auth_task_cancel_during_polling();
    test_real_auth_task_cancel_race_with_token_exchange();
    test_reset_task_waits_for_auth_task_exit();
    test_nvs_failures();
    test_task_creation_failure();
    test_stale_status_and_ongoing_ui_guard();
    test_reauthorization_and_second_reset();
    printf("Baidu reset and real auth_task cancel host tests: ALL PASS\n");
    return 0;
}
'''

        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            c_file = path / 'test_reset.c'
            c_file.write_text(harness + bd_reset_code + bd_auth_code + ui_reset_code + tests)
            bin_file = path / 'test_reset'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            str(c_file), '-o', str(bin_file)], check=True)
            proc = subprocess.run([str(bin_file)], capture_output=True, text=True)
            if proc.returncode != 0:
                print("STDOUT:\n", proc.stdout)
                print("STDERR:\n", proc.stderr)
            self.assertEqual(proc.returncode, 0)
            self.assertIn("Baidu reset and real auth_task cancel host tests: ALL PASS", proc.stdout)


if __name__ == '__main__':
    unittest.main()
