// main/kuku_baidu.c —— 百度网盘接入:设备码授权 + xpan 分片上传。
//
// 授权: GET device/code → 屏显 verification_url + user_code →
//       轮询 token(grant_type=device_token) → NVS 存 access/refresh。
// 上传: precreate(MD5 block_list) → superfile2 逐片(4MB,流式) → create;
//       成功后 /rec/RECxxxx.WAV 改名 .UPD 标记已同步。
// 上传已关闭的 WAV；录音采集与旧文件上传可并行，网络任务不占音频资源。
#include "kuku_app.h"
#ifdef KUKU_BAIDU_KEYS_HEADER
#include KUKU_BAIDU_KEYS_HEADER
#else
#include "kuku_baidu_keys.h"
#endif

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_sntp.h"
#include "nvs.h"
#include "cJSON.h"
#include "mbedtls/md5.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <inttypes.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>

#define TAG "kuku_bd"

// 网盘侧目录:第三方应用仅可写 /apps/<应用名>/ 下。
#define KUKU_BD_DIR KUKU_CLOUD_ROOT
#define KUKU_BD_BLOCK (4 * 1024 * 1024)   // 单片 4MB
#define KUKU_BD_IOBUF 4096
#define KUKU_BD_UPLOAD_IOBUF 1024
#define KUKU_BD_AUTH_STACK 8192
#define KUKU_BD_UPLOAD_ATTEMPTS 3
#define KUKU_BD_WRITE_TIMEOUT_MS 3000
#define KUKU_BD_BODY_BUDGET_MS 40000
// HTTP phases share scratch storage. Device stack watermarks are checked
// during concurrent PCM/TLS transfers; keep extra room for newlib and TLS.
#define KUKU_BD_UPLOAD_STACK 9216
#define KUKU_BD_PROBE_STACK 8192
#define KUKU_BD_PROBE_TIMEOUT_MS 8000
#define KUKU_BD_MIN_RECORD_KB 256 // Allow a shorter segment while older files upload.
#define KUKU_BD_IMAGE_MAX_BYTES (256 * 1024)
#define KUKU_BD_IMAGE_TMP "/rec/KIMG.TMP"
#define KUKU_BD_AVATAR_TMP "/rec/KAVA.TMP"
#define KUKU_BD_RENDER_TMP "/rec/KREN.TMP"

// bd_state 取值(与 g_kuku.bd_state 同步)。
#define BD_NO_AUTH 0
#define BD_WAIT_CODE 1
#define BD_READY 2
#define BD_UPLOADING 3

static char s_access[160];
static char s_refresh[96];
static int64_t s_at_exp_boot;          // access 过期时刻(开机秒),0=未知
static volatile bool s_auth_task_running;
static volatile bool s_auth_cancel;
static volatile bool s_reset_running;
static volatile int s_reset_status;
static volatile bool s_upload_task_running;
static volatile bool s_upload_requested;
static volatile bool s_probe_task_running;
static bool s_probe_uses_network; // Reserved together with task flags below.
static portMUX_TYPE s_cloud_mux = portMUX_INITIALIZER_UNLOCKED;
static kuku_cloud_file_t s_cloud_files[KUKU_CLOUD_PAGE_SIZE];
static int s_cloud_page, s_cloud_count, s_cloud_status;
static bool s_cloud_has_more, s_cloud_task_running;
static char s_cloud_dir[KUKU_CLOUD_PATH_MAX] = KUKU_CLOUD_ROOT;
static volatile int s_image_status;
static kuku_cloud_file_t s_image_file;

// UI 展示数据(WAIT_CODE 时有效)。
static char s_user_code[16];
static char s_verify_url[64];
static char s_up_name[KUKU_MAX_NAME];
static volatile int s_up_done, s_up_total;

static void set_state(uint8_t st) { g_kuku.bd_state = st; }

// Retained upload diagnostics: 0 idle, 1 directory, 2 hash, 3 precreate,
// 4 multipart, 5 create, 6 local mark. No URL, token or response is retained.
static kuku_upload_diag_t s_upload_diag;
static uint32_t s_upload_started_ms, s_upload_phase_ms;
static uint32_t upload_now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static void upload_diag_phase(uint8_t phase) {
    portENTER_CRITICAL(&s_cloud_mux);
    s_upload_diag.phase = phase;
    s_upload_phase_ms = upload_now_ms();
    portEXIT_CRITICAL(&s_cloud_mux);
}
static void upload_diag_begin(void) {
    portENTER_CRITICAL(&s_cloud_mux);
    s_upload_diag.attempts++;
    s_upload_started_ms = upload_now_ms();
    portEXIT_CRITICAL(&s_cloud_mux);
    upload_diag_phase(1);
}
static void upload_diag_end(int rc) {
    portENTER_CRITICAL(&s_cloud_mux);
    uint32_t elapsed = upload_now_ms() - s_upload_started_ms;
    s_upload_diag.last_ms = elapsed;
    if (elapsed > s_upload_diag.max_ms) s_upload_diag.max_ms = elapsed;
    s_upload_diag.last_rc = rc;
    if (rc == 0) s_upload_diag.completed++;
    else {
        s_upload_diag.failures++;
        s_upload_diag.failure_rc = rc;
        s_upload_diag.failure_phase = s_upload_diag.phase;
        s_upload_diag.failure_ms = elapsed;
    }
    s_upload_diag.phase = 0;
    portEXIT_CRITICAL(&s_cloud_mux);
}
void kuku_baidu_get_upload_diag(kuku_upload_diag_t *out) {
    portENTER_CRITICAL(&s_cloud_mux);
    *out = s_upload_diag;
    out->phase_ms = out->phase ? upload_now_ms() - s_upload_phase_ms : 0;
    portEXIT_CRITICAL(&s_cloud_mux);
}

// ---- 小工具 -----------------------------------------------------------------
// Bind cached authorization to this exact client configuration. A changed
// key/secret (or a legacy cache without a fingerprint) requires a fresh scan;
// Wi-Fi settings and recording files are independent and remain intact.
static bool credential_fingerprint(unsigned char out[16]) {
    mbedtls_md5_context ctx;
    mbedtls_md5_init(&ctx);
    bool ok = mbedtls_md5_starts(&ctx) == 0 &&
              mbedtls_md5_update(&ctx, (const unsigned char *)KUKU_BAIDU_APPKEY,
                                 sizeof(KUKU_BAIDU_APPKEY)) == 0 &&
              mbedtls_md5_update(&ctx, (const unsigned char *)KUKU_BAIDU_SECRET,
                                 sizeof(KUKU_BAIDU_SECRET)) == 0 &&
              mbedtls_md5_finish(&ctx, out) == 0;
    mbedtls_md5_free(&ctx);
    return ok;
}

static void nvs_save_tokens(void) {
    nvs_handle_t h;
    if (nvs_open("kuku_bd", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "at", s_access);
    nvs_set_str(h, "rt", s_refresh);
    nvs_set_i32(h, "exp", (int32_t)(s_at_exp_boot >> 1));  // 2s 粒度,够用
    unsigned char fingerprint[16];
    if (credential_fingerprint(fingerprint))
        nvs_set_blob(h, "client_fp", fingerprint, sizeof(fingerprint));
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_load_tokens(void) {
    s_access[0] = s_refresh[0] = 0;
    s_at_exp_boot = 0;
    set_state(BD_NO_AUTH);
    nvs_handle_t h;
    if (nvs_open("kuku_bd", NVS_READONLY, &h) != ESP_OK) return;
    unsigned char saved[16], current[16];
    size_t length = sizeof(saved);
    if (nvs_get_blob(h, "client_fp", saved, &length) != ESP_OK ||
        length != sizeof(saved) || !credential_fingerprint(current) ||
        memcmp(saved, current, sizeof(saved))) {
        nvs_close(h);
        ESP_LOGI(TAG, "网盘应用配置已变更，请重新扫码授权");
        return;
    }
    size_t l1 = sizeof(s_access), l2 = sizeof(s_refresh);
    int32_t exp2 = 0;
    nvs_get_str(h, "at", s_access, &l1);
    nvs_get_str(h, "rt", s_refresh, &l2);
    if (nvs_get_i32(h, "exp", &exp2) == ESP_OK) s_at_exp_boot = (int64_t)exp2 * 2;
    nvs_close(h);
    if (s_access[0]) set_state(BD_READY);
}

static int64_t boot_sec(void) { return esp_timer_get_time() / 1000000; }

// 时间同步(拿到 token 才有意义;设过就跳过)。
static void sntp_start_once(void) {
    if (esp_sntp_enabled()) return;
    setenv("TZ", "CST-8", 1);
    tzset();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    esp_sntp_init();
}

// URL 组件百分号编码(UTF-8 安全)。
static size_t url_encode(const char *in, char *out, size_t cap) {
    static const char HEX[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < cap; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
            (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' ||
            *p == '.' || *p == '~') {
            out[o++] = (char)*p;
        } else {
            out[o++] = '%';
            out[o++] = HEX[*p >> 4];
            out[o++] = HEX[*p & 15];
        }
    }
    out[o] = 0;
    return o;
}

static int http_req(const char *url, esp_http_client_method_t method,
                    const char *body, int body_len, const char *ctype,
                    char *resp, size_t cap, int timeout_ms);
static int token_refresh(void);

// ---- 应用目录文件列表(后台分页,不在按键/LVGL 任务做 HTTPS) -------------------
static void cloud_list_task(void *arg) {
    int page = (int)(intptr_t)arg;
    int status = -1, count = 0;
    bool has_more = false;
    kuku_cloud_file_t files[KUKU_CLOUD_PAGE_SIZE] = {0};
    char dir[KUKU_CLOUD_PATH_MAX], dir_enc[3 * KUKU_CLOUD_PATH_MAX];
    char url[1152];
    char *resp = malloc(8 * 1024);
    kuku_baidu_list_dir(dir, sizeof(dir));
    url_encode(dir, dir_enc, sizeof(dir_enc));
    if (resp && g_kuku.wifi_up && s_access[0]) {
        for (int attempt = 0; attempt < 2; attempt++) {
            snprintf(url, sizeof(url),
                "https://pan.baidu.com/rest/2.0/xpan/file?method=list"
                "&access_token=%s&dir=%s&order=time&desc=1&start=%d&limit=%d",
                s_access, dir_enc, page * KUKU_CLOUD_PAGE_SIZE,
                KUKU_CLOUD_PAGE_SIZE);
            resp[0] = 0;
            if (http_req(url, HTTP_METHOD_GET, NULL, 0, NULL, resp,
                         8 * 1024, 15000) != 0) break;
            cJSON *root = cJSON_Parse(resp);
            if (!root) break;
            const cJSON *errno_j = cJSON_GetObjectItem(root, "errno");
            int code = cJSON_IsNumber(errno_j) ? (int)errno_j->valuedouble : -1;
            if (code != 0 && code != -9) ESP_LOGW(TAG, "list errno=%d", code);
            if (code == 111 && attempt == 0) {
                cJSON_Delete(root);
                if (token_refresh() == 0) continue;
                break;
            }
            if (code == -9) { // 应用目录尚未创建。
                status = 2;
            } else if (code == 0) {
                const cJSON *list = cJSON_GetObjectItem(root, "list");
                if (cJSON_IsArray(list)) {
                    int n = cJSON_GetArraySize(list);
                    has_more = n >= KUKU_CLOUD_PAGE_SIZE;
                    for (int i = 0; i < n && count < KUKU_CLOUD_PAGE_SIZE; i++) {
                        const cJSON *entry = cJSON_GetArrayItem(list, i);
                        const cJSON *id = cJSON_GetObjectItem(entry, "fs_id");
                        const cJSON *name = cJSON_GetObjectItem(entry, "server_filename");
                        if (!cJSON_IsNumber(id) || !cJSON_IsString(name) ||
                            strlen(name->valuestring) >= KUKU_CLOUD_NAME_MAX ||
                            id->valuedouble < 1 || id->valuedouble > 9007199254740991.0)
                            continue;
                        kuku_cloud_file_t *file = &files[count++];
                        file->fs_id = (uint64_t)id->valuedouble;
                        strlcpy(file->name, name->valuestring, sizeof(file->name));
                        const cJSON *size = cJSON_GetObjectItem(entry, "size");
                        const cJSON *mtime = cJSON_GetObjectItem(entry, "server_mtime");
                        const cJSON *isdir = cJSON_GetObjectItem(entry, "isdir");
                        file->size = cJSON_IsNumber(size) ? (uint64_t)size->valuedouble : 0;
                        file->mtime = cJSON_IsNumber(mtime) ? (uint32_t)mtime->valuedouble : 0;
                        file->is_dir = cJSON_IsNumber(isdir) && isdir->valueint != 0;
                    }
                    status = 2;
                }
            }
            cJSON_Delete(root);
            break;
        }
    }
    free(resp);
    portENTER_CRITICAL(&s_cloud_mux);
    memcpy(s_cloud_files, files, sizeof(files));
    s_cloud_page = page;
    s_cloud_count = count;
    s_cloud_has_more = has_more;
    s_cloud_status = status;
    s_cloud_task_running = false;
    portEXIT_CRITICAL(&s_cloud_mux);
    vTaskDelete(NULL);
}

static int cloud_list_request_at(const char *dir, int page) {
    if (page < 0 || page > 10000 || !g_kuku.wifi_up || !s_access[0]) return -1;
    if (s_upload_task_running || s_auth_task_running) return -3;
    portENTER_CRITICAL(&s_cloud_mux);
    if (s_cloud_task_running || s_reset_running) {
        portEXIT_CRITICAL(&s_cloud_mux);
        return -3;
    }
    s_cloud_task_running = true;
    s_cloud_status = 1;
    s_cloud_page = page;
    s_cloud_count = 0;
    s_cloud_has_more = false;
    if (dir) strlcpy(s_cloud_dir, dir, sizeof(s_cloud_dir));
    portEXIT_CRITICAL(&s_cloud_mux);
    if (xTaskCreate(cloud_list_task, "kuku_bd_list", 8192,
                    (void *)(intptr_t)page, 4, NULL) != pdPASS) {
        portENTER_CRITICAL(&s_cloud_mux);
        s_cloud_task_running = false;
        s_cloud_status = -1;
        portEXIT_CRITICAL(&s_cloud_mux);
        return -4;
    }
    return 0;
}

int kuku_baidu_list_request(int page) {
    return cloud_list_request_at(NULL, page);
}

void kuku_baidu_list_dir(char *out, size_t cap) {
    if (!out || !cap) return;
    portENTER_CRITICAL(&s_cloud_mux);
    strlcpy(out, s_cloud_dir, cap);
    portEXIT_CRITICAL(&s_cloud_mux);
}

bool kuku_baidu_list_is_root(void) {
    portENTER_CRITICAL(&s_cloud_mux);
    bool root = !strcmp(s_cloud_dir, KUKU_CLOUD_ROOT);
    portEXIT_CRITICAL(&s_cloud_mux);
    return root;
}

int kuku_baidu_list_enter(const kuku_cloud_file_t *folder) {
    if (!folder || !folder->is_dir) return -1;
    char next[KUKU_CLOUD_PATH_MAX];
    bool found = false;
    portENTER_CRITICAL(&s_cloud_mux);
    for (int i = 0; s_cloud_status == 2 && i < s_cloud_count; ++i) {
        if (s_cloud_files[i].is_dir && s_cloud_files[i].fs_id == folder->fs_id &&
            !strcmp(s_cloud_files[i].name, folder->name)) found = true;
    }
    bool valid = found && kuku_cloud_child_path(s_cloud_dir, folder->name, next, sizeof(next));
    portEXIT_CRITICAL(&s_cloud_mux);
    return valid ? cloud_list_request_at(next, 0) : -1;
}

int kuku_baidu_list_up(void) {
    char current[KUKU_CLOUD_PATH_MAX], parent[KUKU_CLOUD_PATH_MAX];
    kuku_baidu_list_dir(current, sizeof(current));
    if (!strcmp(current, KUKU_CLOUD_ROOT)) return 1;
    if (!kuku_cloud_parent_path(current, parent, sizeof(parent))) return -1;
    return cloud_list_request_at(parent, 0);
}

void kuku_baidu_list_status(int *page, int *count, int *status, bool *has_more) {
    portENTER_CRITICAL(&s_cloud_mux);
    if (page) *page = s_cloud_page;
    if (count) *count = s_cloud_count;
    if (status) *status = s_cloud_status;
    if (has_more) *has_more = s_cloud_has_more;
    portEXIT_CRITICAL(&s_cloud_mux);
}

bool kuku_baidu_list_get(int idx, kuku_cloud_file_t *out) {
    if (!out || idx < 0) return false;
    portENTER_CRITICAL(&s_cloud_mux);
    bool valid = s_cloud_status == 2 && idx < s_cloud_count;
    if (valid) *out = s_cloud_files[idx];
    portEXIT_CRITICAL(&s_cloud_mux);
    return valid;
}

bool kuku_baidu_is_jpeg(const char *name) {
    if (!name) return false;
    const char *ext = strrchr(name, '.');
    return ext && (!strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg"));
}

typedef struct {
    FILE *file;
    size_t written;
    bool overflow;
    char *redirect;
} image_sink_t;

static esp_err_t image_http_event(esp_http_client_event_t *evt) {
    image_sink_t *sink = evt->user_data;
    if (sink && evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key &&
        evt->header_value && !strcasecmp(evt->header_key, "Location")) {
        size_t n = strlen(evt->header_value);
        if (n < 4096) {
            free(sink->redirect);
            sink->redirect = strdup(evt->header_value);
        }
        return ESP_OK;
    }
    if (evt->event_id != HTTP_EVENT_ON_DATA ||
        esp_http_client_get_status_code(evt->client) != 200) return ESP_OK;
    if (!sink || !sink->file || evt->data_len < 0 ||
        sink->written + (size_t)evt->data_len > KUKU_BD_IMAGE_MAX_BYTES) {
        if (sink) sink->overflow = true;
        return ESP_FAIL;
    }
    if (fwrite(evt->data, 1, evt->data_len, sink->file) != (size_t)evt->data_len)
        return ESP_FAIL;
    sink->written += (size_t)evt->data_len;
    return ESP_OK;
}

// TinyJPEG on this target handles baseline JPEG only. Read markers before SOS
// without loading the image into RAM; progressive originals use a cloud thumb.
static bool image_is_baseline(FILE *f) {
    if (!f || fseek(f, 2, SEEK_SET) != 0) return false;
    for (int i = 0; i < 128; i++) {
        int lead = fgetc(f);
        if (lead != 0xff) return false;
        int marker;
        do { marker = fgetc(f); } while (marker == 0xff);
        if (marker < 0 || marker == 0xda || marker == 0xd9) return false;
        if (marker == 0xc0) return true;
        if (marker == 0xc2) return false;
        if (marker == 0xd8 || marker == 0x01 ||
            (marker >= 0xd0 && marker <= 0xd7)) continue;
        int high = fgetc(f), low = fgetc(f);
        int length = (high << 8) | low;
        if (high < 0 || low < 0 || length < 2 ||
            fseek(f, length - 2, SEEK_CUR) != 0) return false;
    }
    return false;
}

bool kuku_baidu_jpeg_baseline(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t magic[2] = {0};
    bool valid = fread(magic, 1, 2, f) == 2 &&
                 magic[0] == 0xff && magic[1] == 0xd8 &&
                 image_is_baseline(f);
    fclose(f);
    return valid;
}

static int image_download(const char *url) {
    if (!url || strncmp(url, "https://", 8) != 0 ||
        kuku_rec_free_kb() < KUKU_BD_MIN_RECORD_KB + 320) return -1;
    FILE *f = fopen(KUKU_BD_IMAGE_TMP, "wb");
    if (!f) return -2;
    image_sink_t sink = {.file = f};
    int status = -1;
    char *current_url = strdup(url);
    for (int hop = 0; current_url && hop < 4; hop++) {
        esp_http_client_config_t cfg = {
            .url = current_url,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .method = HTTP_METHOD_GET,
            .timeout_ms = 15000,
            .disable_auto_redirect = true,
            .event_handler = image_http_event,
            .user_data = &sink,
            .buffer_size = 2048,
            .buffer_size_tx = 4096,
        };
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) break;
        esp_http_client_set_header(client, "User-Agent", "pan.baidu.com");
        esp_err_t err = esp_http_client_perform(client);
        status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);
        if (status == 200) {
            if (err != ESP_OK)
                ESP_LOGW(TAG, "image transport=%s http=%d", esp_err_to_name(err), status);
            if (err != ESP_OK) status = -1;
            break;
        }
        if (status >= 300 && status < 400 && sink.redirect &&
            strncmp(sink.redirect, "https://", 8) == 0) {
            free(current_url);
            current_url = sink.redirect;
            sink.redirect = NULL;
            continue;
        }
        if (err != ESP_OK)
            ESP_LOGW(TAG, "image transport=%s http=%d", esp_err_to_name(err), status);
        break;
    }
    free(current_url);
    free(sink.redirect);
    fclose(f);
    if (status != 200 || sink.overflow || sink.written < 4) {
        remove(KUKU_BD_IMAGE_TMP);
        ESP_LOGW(TAG, "image download failed status=%d bytes=%u", status,
                 (unsigned)sink.written);
        return -3;
    }
    f = fopen(KUKU_BD_IMAGE_TMP, "rb");
    uint8_t magic[2] = {0};
    bool baseline = false;
    if (f) {
        fread(magic, 1, 2, f);
        if (magic[0] == 0xff && magic[1] == 0xd8)
            baseline = image_is_baseline(f);
        fclose(f);
    }
    if (magic[0] != 0xff || magic[1] != 0xd8) {
        remove(KUKU_BD_IMAGE_TMP);
        return -4;
    }
    if (!baseline) {
        remove(KUKU_BD_IMAGE_TMP);
        return -6;
    }
    remove(KUKU_IMAGE_PREVIEW_PATH);
    if (rename(KUKU_BD_IMAGE_TMP, KUKU_IMAGE_PREVIEW_PATH) != 0) {
        remove(KUKU_BD_IMAGE_TMP);
        return -5;
    }
    if (kuku_image_render_bmp(KUKU_IMAGE_PREVIEW_PATH, KUKU_BD_RENDER_TMP,
                              KUKU_IMAGE_PREVIEW_EDGE) != 0)
        return -7;
    remove(KUKU_IMAGE_PREVIEW_BMP_PATH);
    if (rename(KUKU_BD_RENDER_TMP, KUKU_IMAGE_PREVIEW_BMP_PATH) != 0) {
        remove(KUKU_BD_RENDER_TMP);
        return -8;
    }
    return 0;
}

static const char *image_thumbnail(const cJSON *file) {
    const cJSON *thumbs = cJSON_GetObjectItem(file, "thumbs");
    if (!cJSON_IsObject(thumbs)) return NULL;
    const char *keys[] = {"url3", "url2", "url1", "url4"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        const cJSON *item = cJSON_GetObjectItem(thumbs, keys[i]);
        if (cJSON_IsString(item) && !strncmp(item->valuestring, "https://", 8))
            return item->valuestring;
    }
    return NULL;
}

static void image_task(void *arg) {
    (void)arg;
    int result = -1;
    char *resp = malloc(4096);
    if (resp) {
        for (int attempt = 0; attempt < 2; attempt++) {
            char url[480];
            snprintf(url, sizeof(url),
                "https://pan.baidu.com/rest/2.0/xpan/multimedia?method=filemetas"
                "&access_token=%s&fsids=%%5B%" PRIu64 "%%5D&dlink=1&thumb=1&extra=1",
                s_access, s_image_file.fs_id);
            resp[0] = 0;
            if (http_req(url, HTTP_METHOD_GET, NULL, 0, NULL,
                         resp, 4096, 15000) != 0) break;
            cJSON *root = cJSON_Parse(resp);
            const cJSON *err = root ? cJSON_GetObjectItem(root, "errno") : NULL;
            int code = cJSON_IsNumber(err) ? err->valueint : -1;
            if (code == 111 && attempt == 0) {
                cJSON_Delete(root);
                if (token_refresh() == 0) continue;
                break;
            }
            const cJSON *list = cJSON_GetObjectItem(root, "list");
            const cJSON *file = cJSON_GetArrayItem(list, 0);
            const cJSON *dlink = cJSON_GetObjectItem(file, "dlink");
            const char *thumb = image_thumbnail(file);
            char *fallback_thumb = thumb ? strdup(thumb) : NULL;
            ESP_LOGI(TAG, "image meta thumbnail=%d", fallback_thumb != NULL);
            const char *selected = NULL;
            bool append_token = false;
            if (s_image_file.size > KUKU_BD_IMAGE_MAX_BYTES)
                selected = image_thumbnail(file);
            if (!selected && s_image_file.size <= KUKU_BD_IMAGE_MAX_BYTES &&
                cJSON_IsString(dlink)) {
                selected = dlink->valuestring;
                append_token = true;
            }
            if (code == 0 && selected && !strncmp(selected, "https://", 8)) {
                size_t len = strlen(selected) + strlen(s_access) + 32;
                char *download_url = malloc(len);
                if (download_url) {
                    snprintf(download_url, len, "%s%saccess_token=%s", selected,
                             strchr(selected, '?') ? "&" : "?", s_access);
                    if (!append_token) strlcpy(download_url, selected, len);
                    cJSON_Delete(root);
                    root = NULL;
                    free(resp);
                    resp = NULL;
                    result = image_download(download_url);
                    if (result == -6 && fallback_thumb) {
                        ESP_LOGI(TAG, "progressive JPEG: using cloud thumbnail");
                        result = image_download(fallback_thumb);
                    }
                    free(download_url);
                }
            }
            if (code != 0) ESP_LOGW(TAG, "image meta errno=%d", code);
            free(fallback_thumb);
            cJSON_Delete(root);
            break;
        }
    }
    free(resp);
    s_image_status = result == 0 ? 2 : -1;
    vTaskDelete(NULL);
}

int kuku_baidu_image_request(const kuku_cloud_file_t *file) {
    if (!file || file->is_dir || !kuku_baidu_is_jpeg(file->name) ||
        !g_kuku.wifi_up || g_kuku.bd_state != BD_READY) return -1;
    if (s_image_status == 1 || s_image_status == 3 ||
        s_upload_task_running || s_cloud_task_running || s_auth_task_running ||
        s_reset_running)
        return -3;
    s_image_file = *file;
    s_image_status = 1;
    if (xTaskCreate(image_task, "kuku_bd_img", 8192, NULL, 4, NULL) != pdPASS) {
        s_image_status = -1;
        return -4;
    }
    return 0;
}

static void avatar_save_task(void *arg) {
    (void)arg;
    int result = kuku_image_render_bmp(KUKU_IMAGE_PREVIEW_PATH, KUKU_BD_AVATAR_TMP, 64);
    if (result == 0) {
        remove(KUKU_IMAGE_AVATAR_PATH);
        if (rename(KUKU_BD_AVATAR_TMP, KUKU_IMAGE_AVATAR_PATH) != 0) result = -1;
    }
    if (result != 0) remove(KUKU_BD_AVATAR_TMP);
    s_image_status = result == 0 ? 4 : -1;
    vTaskDelete(NULL);
}

int kuku_baidu_avatar_save(void) {
    if (s_image_status != 2 ||
        kuku_rec_free_kb() < KUKU_BD_MIN_RECORD_KB + 320) return -1;
    s_image_status = 3;
    if (xTaskCreate(avatar_save_task, "kuku_bd_avatar", 4096,
                    NULL, 3, NULL) != pdPASS) {
        s_image_status = 2;
        return -4;
    }
    return 0;
}

int kuku_baidu_image_status(void) { return s_image_status; }

// ---- HTTPS(统一 open/write/fetch/read 路径) ---------------------------------
// 返回 0=HTTP 2xx,resp 收到响应体;-1 传输失败;>0 HTTP 状态码。
static int http_req(const char *url, esp_http_client_method_t method,
                    const char *body, int body_len, const char *ctype,
                    char *resp, size_t cap, int timeout_ms) {
    if (resp && cap) resp[0] = 0; // Failed handshakes have no response body.
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .method = method,
        .timeout_ms = timeout_ms,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;
    if (body && body_len > 0) {
        esp_http_client_set_header(c, "Content-Type", ctype);
        esp_http_client_set_post_field(c, body, body_len);
    }
    int rc = -1;
    esp_err_t open_err = esp_http_client_open(c, body ? body_len : 0);
    if (open_err == ESP_OK) {
        if (body && body_len > 0) {
            int w = esp_http_client_write(c, body, body_len);
            if (w != body_len) {
                ESP_LOGE(TAG, "write body short %d/%d", w, body_len);
                esp_http_client_close(c);
                esp_http_client_cleanup(c);
                return -1;
            }
        }
        esp_http_client_fetch_headers(c);
        int n = 0;
        if (resp && cap) {
            n = esp_http_client_read_response(c, resp, (int)cap - 1);
            if (n < 0) n = 0;
            resp[n] = 0;
        } else {
            char sink[256];
            while (esp_http_client_read(c, sink, sizeof(sink)) > 0) {}
        }
        int status = esp_http_client_get_status_code(c);
        rc = (status >= 200 && status < 300) ? 0 : status;
    } else {
        ESP_LOGW(TAG, "http open: %s", esp_err_to_name(open_err));
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    // OAuth URLs carry application secrets and access tokens; never log them.
    if (rc != 0) {
        const char *host = strstr(url, "://");
        host = host ? host + 3 : url;
        const char *host_end = strchr(host, '/');
        ESP_LOGW(TAG, "http request failed host=%.*s rc=%d",
                 host_end ? (int)(host_end - host) : (int)strlen(host), host, rc);
        cJSON *failure = resp ? cJSON_Parse(resp) : NULL;
        const cJSON *error = failure ? cJSON_GetObjectItem(failure, "error") : NULL;
        const char *known[] = { "invalid_client", "invalid_request", "invalid_grant",
                                "unauthorized_client", "unsupported_response_type",
                                "invalid_scope", "expired_token", "authorization_pending" };
        if (cJSON_IsString(error)) {
            for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++)
                if (!strcmp(error->valuestring, known[i]))
                    ESP_LOGW(TAG, "OAuth error: %s", known[i]);
        }
        cJSON_Delete(failure);
    }
    return rc;
}

// superfile2 单片:multipart 流式(文件不进 RAM)。
static int http_upload_part(const char *url, FILE *f, long size,
                            char *resp, size_t cap) {
    static const char BOUNDARY[] = "----kukubadge7f3a9";
    char head[256];
    int head_len = snprintf(head, sizeof(head),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"chunk\"\r\n"
        "Content-Type: application/octet-stream\r\n\r\n", BOUNDARY);
    char tail[64];
    int tail_len = snprintf(tail, sizeof(tail), "\r\n--%s--\r\n", BOUNDARY);

    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 15000,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;

    char ctype[64];
    snprintf(ctype, sizeof(ctype), "multipart/form-data; boundary=%s", BOUNDARY);
    esp_http_client_set_header(c, "Content-Type", ctype);
    long total = head_len + size + tail_len;

    int rc = -1;
    uint8_t *buf = malloc(KUKU_BD_UPLOAD_IOBUF);
    long sent = 0;
    esp_err_t open_rc = buf ? esp_http_client_open(c, total) : ESP_ERR_NO_MEM;
    if (open_rc == ESP_OK) {
        // A 60-second blocked socket write consumes most of the recording
        // reserve. Close a stalled transaction and retry on a new connection.
        esp_http_client_set_timeout_ms(c, KUKU_BD_WRITE_TIMEOUT_MS);
        // The HTTP timeout changes polling; the TLS socket still carries the
        // timeout installed during connection setup. Bound its send too.
        int sockfd = esp_http_client_get_socket(c);
        struct timeval send_timeout = {
            .tv_sec = KUKU_BD_WRITE_TIMEOUT_MS / 1000,
            .tv_usec = (KUKU_BD_WRITE_TIMEOUT_MS % 1000) * 1000,
        };
        bool ok = sockfd >= 0 && setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO,
                                           &send_timeout, sizeof(send_timeout)) == 0;
        int64_t body_started = esp_timer_get_time();
        int w = ok ? esp_http_client_write(c, head, head_len) : -1;
        ok = ok && (w == head_len);
        if (ok) {
            long left = size;
            while (left > 0 && ok) {
                size_t want = left > KUKU_BD_UPLOAD_IOBUF ? KUKU_BD_UPLOAD_IOBUF : (size_t)left;
                size_t r = fread(buf, 1, want, f);
                if (r == 0) { ok = false; break; }
                int off = 0;
                while (off < (int)r && ok) {
                    if ((esp_timer_get_time() - body_started) / 1000 >= KUKU_BD_BODY_BUDGET_MS) {
                        ESP_LOGW(TAG, "part body deadline sent=%ld/%ld", sent, size);
                        ok = false;
                        break;
                    }
                    w = esp_http_client_write(c, (const char *)buf + off, (int)r - off);
                    if (w <= 0) ok = false; else { off += w; sent += w; }
                }
                left -= r;
                if ((sent - (long)r) / 65536 != sent / 65536)
                    ESP_LOGI(TAG, "UP_PCM: sent=%ld rec=%d", sent, g_kuku.recording);
                vTaskDelay(pdMS_TO_TICKS(1)); // Bound upload CPU bursts.
            }
        }
        if (ok) ok = (esp_http_client_write(c, tail, tail_len) == tail_len);
        if (ok) {
            esp_http_client_set_timeout_ms(c, 15000);
            int64_t header_rc = esp_http_client_fetch_headers(c);
            if (header_rc < 0) {
                ESP_LOGW(TAG, "part response headers failed rc=%lld sent=%ld/%ld",
                         (long long)header_rc, sent, size);
                ok = false;
            }
        }
        if (ok) {
            int n = esp_http_client_read_response(c, resp, (int)cap - 1);
            if (n < 0) n = 0;
            resp[n] = 0;
            int status = esp_http_client_get_status_code(c);
            rc = (status >= 200 && status < 300) ? 0 : status;
        }
        if (!ok) ESP_LOGW(TAG, "part body write failed sent=%ld/%ld", sent, size);
    } else {
        ESP_LOGW(TAG, "part connection failed rc=%s", esp_err_to_name(open_rc));
    }
    free(buf);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return rc;
}

// ---- MD5 --------------------------------------------------------------------
// 对文件 [off, off+len) 求 MD5,输出 32 位小写十六进制。
static int file_block_md5(FILE *f, long off, long len, char hex[33]) {
    mbedtls_md5_context ctx;
    mbedtls_md5_init(&ctx);
    if (mbedtls_md5_starts(&ctx) != 0) { mbedtls_md5_free(&ctx); return -1; }
    uint8_t *buf = malloc(KUKU_BD_IOBUF);
    if (!buf) { mbedtls_md5_free(&ctx); return -1; }
    if (fseek(f, off, SEEK_SET) != 0) { free(buf); mbedtls_md5_free(&ctx); return -1; }
    long left = len;
    while (left > 0) {
        size_t want = left > KUKU_BD_IOBUF ? KUKU_BD_IOBUF : (size_t)left;
        size_t r = fread(buf, 1, want, f);
        if (r == 0) { free(buf); mbedtls_md5_free(&ctx); return -1; }
        if (mbedtls_md5_update(&ctx, buf, r) != 0) {
            free(buf); mbedtls_md5_free(&ctx); return -1;
        }
        left -= r;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    free(buf);
    uint8_t digest[16];
    if (mbedtls_md5_finish(&ctx, digest) != 0) {
        mbedtls_md5_free(&ctx); return -1;
    }
    mbedtls_md5_free(&ctx);
    static const char HEX[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        hex[i * 2] = HEX[digest[i] >> 4];
        hex[i * 2 + 1] = HEX[digest[i] & 15];
    }
    hex[32] = 0;
    return 0;
}

// 全文件按 4MB 切片求 block_list JSON(含方括号,已 urlencode 前的原文)。
static int md5_block_list(const char *path, long size, char *json, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t o = 0;
    o += (size_t)snprintf(json + o, cap - o, "[");
    int nblk = (int)((size + KUKU_BD_BLOCK - 1) / KUKU_BD_BLOCK);
    for (int i = 0; i < nblk; i++) {
        long off = (long)i * KUKU_BD_BLOCK;
        long len = size - off > KUKU_BD_BLOCK ? KUKU_BD_BLOCK : size - off;
        char hex[33];
        if (file_block_md5(f, off, len, hex) != 0) { fclose(f); return -1; }
        o += (size_t)snprintf(json + o, cap - o, "%s\"%s\"", i ? "," : "", hex);
        if (o >= cap - 40) { fclose(f); return -1; }
    }
    fclose(f);
    snprintf(json + o, cap - o, "]");
    return 0;
}

// ---- 令牌 -------------------------------------------------------------------
// 成功写 s_access/s_refresh 并持久化,返回 0。
static int token_exchange(const char *url) {
    char resp[1024] = {0};
    if (http_req(url, HTTP_METHOD_GET, NULL, 0, NULL, resp, sizeof(resp), 12000) != 0)
        return -1;
    cJSON *root = cJSON_Parse(resp);
    if (!root) { ESP_LOGW(TAG, "token resp not json: %.80s", resp); return -1; }
    const cJSON *at = cJSON_GetObjectItem(root, "access_token");
    const cJSON *rt = cJSON_GetObjectItem(root, "refresh_token");
    const cJSON *ex = cJSON_GetObjectItem(root, "expires_in");
    const cJSON *err = cJSON_GetObjectItem(root, "error");
    if (cJSON_IsString(at) && at->valuestring[0]) {
        strlcpy(s_access, at->valuestring, sizeof(s_access));
        // refresh_token 单次有效:响应里给了新的就立刻替换持久化
        if (cJSON_IsString(rt) && rt->valuestring[0])
            strlcpy(s_refresh, rt->valuestring, sizeof(s_refresh));
        int64_t now_epoch = time(NULL);
        int64_t exp = cJSON_IsNumber(ex) ? (int64_t)ex->valuedouble : 2592000;
        // 墙钟未同步时存 0(视为"未知",上传遇 111 再刷新)
        s_at_exp_boot = (now_epoch > 1600000000) ? (now_epoch + exp - 3600) : 0;
        nvs_save_tokens();
        cJSON_Delete(root);
        return 0;
    }
    const char *e = cJSON_IsString(err) ? err->valuestring : "?";
    ESP_LOGW(TAG, "token error: %s", e);
    cJSON_Delete(root);
    return -2;   // 授权等待/拒绝/过期等业务错误
}

static int token_refresh(void) {
    if (!s_refresh[0]) return -1;
    char url[512];
    snprintf(url, sizeof(url),
        "https://openapi.baidu.com/oauth/2.0/token?grant_type=refresh_token"
        "&refresh_token=%s&client_id=" KUKU_BAIDU_APPKEY
        "&client_secret=" KUKU_BAIDU_SECRET, s_refresh);
    return token_exchange(url);
}

// Only a fresh probe starts a recording. The two API hosts have failed
// independently in the field: pan.baidu.com can answer while d.pcs.baidu.com
// (the actual multipart upload host) times out.
static bool upload_host_reachable(void) {
    esp_http_client_config_t cfg = {
        .url = "https://d.pcs.baidu.com/",
        .crt_bundle_attach = esp_crt_bundle_attach,
        .method = HTTP_METHOD_GET,
        .timeout_ms = KUKU_BD_PROBE_TIMEOUT_MS,
        .buffer_size = 512,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    // An unauthenticated root request normally returns 403. Any non-5xx
    // HTTP response proves TCP, TLS and the upload host answered this probe.
    return err == ESP_OK && status >= 200 && status < 500;
}

static bool account_reachable(void) {
    for (int attempt = 0; attempt < 2; attempt++) {
        char url[384], resp[768] = {0};
        snprintf(url, sizeof(url),
            "https://pan.baidu.com/rest/2.0/xpan/nas?method=uinfo&access_token=%s",
            s_access);
        if (http_req(url, HTTP_METHOD_GET, NULL, 0, NULL,
                     resp, sizeof(resp), KUKU_BD_PROBE_TIMEOUT_MS) != 0) return false;
        cJSON *root = cJSON_Parse(resp);
        const cJSON *err = root ? cJSON_GetObjectItem(root, "errno") : NULL;
        int code = cJSON_IsNumber(err) ? (int)err->valuedouble : -1;
        cJSON_Delete(root);
        if (code == 0) return true;
        if (code != 111 || token_refresh() != 0) return false;
    }
    return false;
}

static void record_probe_task(void *arg) {
    (void)arg;
    // Do storage work in this worker, never in the input/button task.
    if (kuku_rec_free_kb() < KUKU_BD_MIN_RECORD_KB) kuku_rec_clean_synced();
    if (kuku_rec_free_kb() < KUKU_BD_MIN_RECORD_KB) {
        kuku_ui_set_status("存储不足，不能录音");
        goto done;
    }
    // An active upload already owns TLS and may refresh the token. Do not
    // allocate a second TLS connection or read that token concurrently.
    bool reachable = g_kuku.wifi_up && (!s_probe_uses_network ||
                     (upload_host_reachable() && account_reachable()));
    if (!reachable || !g_kuku.wifi_up) {
        kuku_ui_set_status("网盘不可达，未录音");
    } else if (g_kuku.bd_state != BD_READY && g_kuku.bd_state != BD_UPLOADING) {
        kuku_ui_set_status("网盘未就绪，不能录音");
    } else if (kuku_rec_start() == 0) {
        kuku_ui_set_status("");
    } else {
        kuku_ui_set_status("录音启动失败");
    }
done:
    portENTER_CRITICAL(&s_cloud_mux);
    s_probe_task_running = false;
    bool pending = s_upload_requested;
    portEXIT_CRITICAL(&s_cloud_mux);
    if (pending) kuku_baidu_upload_pass();
    vTaskDelete(NULL);
}

int kuku_baidu_request_recording(void) {
    if (!g_kuku.wifi_up) {
        kuku_ui_set_status("无网络，不能录音");
        return -2;
    }
    if ((g_kuku.bd_state != BD_READY && g_kuku.bd_state != BD_UPLOADING) ||
        !s_access[0]) {
        kuku_ui_set_status("网盘未就绪，不能录音");
        return -1;
    }
    portENTER_CRITICAL(&s_cloud_mux);
    if (g_kuku.recording || s_probe_task_running || s_reset_running || !s_access[0]) {
        portEXIT_CRITICAL(&s_cloud_mux);
        kuku_ui_set_status("请稍候");
        return -3;
    }
    s_probe_uses_network = !s_upload_task_running;
    s_probe_task_running = true;
    portEXIT_CRITICAL(&s_cloud_mux);
    if (xTaskCreate(record_probe_task, "kuku_bd_probe",
                    s_probe_uses_network ? KUKU_BD_PROBE_STACK : 4096,
                    NULL, 4, NULL) != pdPASS) {
        portENTER_CRITICAL(&s_cloud_mux);
        s_probe_task_running = false;
        portEXIT_CRITICAL(&s_cloud_mux);
        kuku_ui_set_status("网盘检查失败");
        return -6;
    }
    kuku_ui_set_status("检查网盘中");
    return 0;
}

// ---- 授权任务 ----------------------------------------------------------------
static void auth_task(void *arg) {
    (void)arg;
    int rc = -1;
    char url[256];
    while (rc != 0 && !s_auth_cancel) {
        // 旧码到期即隐藏二维码；获取失败时界面保持“正在连接”。
        s_user_code[0] = s_verify_url[0] = 0;
        char resp[768] = {0};
        snprintf(url, sizeof(url),
            "https://openapi.baidu.com/oauth/2.0/device/code"
            "?response_type=device_code&client_id=" KUKU_BAIDU_APPKEY
            "&scope=basic,netdisk");
        bool got_code = false;
        if (g_kuku.wifi_up &&
            http_req(url, HTTP_METHOD_GET, NULL, 0, NULL, resp, sizeof(resp), 12000) == 0) {
            cJSON *root = cJSON_Parse(resp);
            const cJSON *dc = root ? cJSON_GetObjectItem(root, "device_code") : NULL;
            const cJSON *uc = root ? cJSON_GetObjectItem(root, "user_code") : NULL;
            const cJSON *vu = root ? cJSON_GetObjectItem(root, "verification_url") : NULL;
            const cJSON *ex = root ? cJSON_GetObjectItem(root, "expires_in") : NULL;
            const cJSON *iv = root ? cJSON_GetObjectItem(root, "interval") : NULL;
            if (cJSON_IsString(dc) && cJSON_IsString(uc)) {
                got_code = true;
                strlcpy(s_user_code, uc->valuestring, sizeof(s_user_code));
                strlcpy(s_verify_url,
                        cJSON_IsString(vu) ? vu->valuestring : "openapi.baidu.com/device",
                        sizeof(s_verify_url));
                int interval = cJSON_IsNumber(iv) ? (int)iv->valuedouble : 5;
                int expires = cJSON_IsNumber(ex) ? (int)ex->valuedouble : 300;
                if (interval < 1) interval = 5;
                if (expires < 1) expires = 300;
                ESP_LOGI(TAG, "AUTH: 新授权码有效 %ds", expires);

                // 到期后立即申请下一码，不再退回“尚未授权”。
                int64_t deadline = boot_sec() + expires;
                rc = -2;
                while (boot_sec() < deadline && !s_auth_cancel) {
                    int64_t remaining = deadline - boot_sec();
                    int wait = remaining < interval ? (int)remaining : interval;
                    for (int second = 0; second < wait && !s_auth_cancel; second++)
                        vTaskDelay(pdMS_TO_TICKS(1000));
                    if (s_auth_cancel || boot_sec() >= deadline) break;
                    if (!g_kuku.wifi_up) continue;
                    snprintf(url, sizeof(url),
                        "https://openapi.baidu.com/oauth/2.0/token?grant_type=device_token"
                        "&code=%s&client_id=" KUKU_BAIDU_APPKEY
                        "&client_secret=" KUKU_BAIDU_SECRET, dc->valuestring);
                    rc = token_exchange(url);
                    if (rc == 0) break;
                }
                if (rc != 0) ESP_LOGI(TAG, "AUTH: 授权码到期，自动刷新");
            } else {
                ESP_LOGW(TAG, "device/code missing required fields");
            }
            cJSON_Delete(root);
        } else if (g_kuku.wifi_up) {
            ESP_LOGW(TAG, "device/code http fail");
        }
        if (!got_code) {
            for (int second = 0; second < 10 && !s_auth_cancel; second++)
                vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    if (rc == 0 && !s_auth_cancel) {
        set_state(BD_READY);
        sntp_start_once();
        ESP_LOGI(TAG, "AUTH: 授权成功,启动上传");
        s_auth_task_running = false;
        kuku_baidu_upload_pass();
    } else {
        s_auth_task_running = false;
    }
    vTaskDelete(NULL);
}

int kuku_baidu_auth_start(void) {
    if (!g_kuku.wifi_up) return -2;                 // 先联网
    if (g_kuku.bd_state == BD_READY) return 1;      // 已授权
    portENTER_CRITICAL(&s_cloud_mux);
    if (s_auth_task_running || s_upload_task_running || s_reset_running) {
        portEXIT_CRITICAL(&s_cloud_mux);
        return -1;
    }
    s_auth_cancel = false;
    s_auth_task_running = true;
    portEXIT_CRITICAL(&s_cloud_mux);
    s_user_code[0] = s_verify_url[0] = 0;           // 不展示上一轮已过期的授权码
    set_state(BD_WAIT_CODE);
    if (xTaskCreate(auth_task, "kuku_bd_auth", KUKU_BD_AUTH_STACK,
                    NULL, 4, NULL) != pdPASS) {
        s_auth_task_running = false;
        set_state(BD_NO_AUTH);
        return -3;
    }
    return 0;
}

// Local logout only: the account's cloud files and Wi-Fi credentials are untouched.
// Cancellation is cooperative, never vTaskDelete() a task inside TLS/NVS.
static void reset_task(void *arg) {
    (void)arg;
    while (s_auth_task_running) vTaskDelay(pdMS_TO_TICKS(100));
    nvs_handle_t h;
    esp_err_t result = nvs_open("kuku_bd", NVS_READWRITE, &h);
    if (result == ESP_OK) {
        result = nvs_erase_all(h);
        if (result == ESP_OK) result = nvs_commit(h);
        nvs_close(h);
    }
    if (result == ESP_OK) {
        memset(s_access, 0, sizeof(s_access));
        memset(s_refresh, 0, sizeof(s_refresh));
        s_at_exp_boot = 0;
        s_user_code[0] = s_verify_url[0] = s_up_name[0] = 0;
        s_up_done = s_up_total = 0;
        portENTER_CRITICAL(&s_cloud_mux);
        memset(s_cloud_files, 0, sizeof(s_cloud_files));
        s_cloud_count = s_cloud_page = s_cloud_status = 0;
        s_cloud_has_more = false;
        strlcpy(s_cloud_dir, KUKU_CLOUD_ROOT, sizeof(s_cloud_dir));
        s_image_status = 0;
        s_upload_requested = false;
        portEXIT_CRITICAL(&s_cloud_mux);
        set_state(BD_NO_AUTH);
        s_reset_status = 2;
        ESP_LOGI(TAG, "RESET: 网盘连接已重置，保留无线网络与录音");
    } else {
        set_state(s_access[0] ? BD_READY : BD_NO_AUTH);
        s_reset_status = -1;
        ESP_LOGW(TAG, "RESET: 本地授权清除失败 rc=%d", (int)result);
    }
    portENTER_CRITICAL(&s_cloud_mux);
    s_auth_cancel = false;
    s_reset_running = false;
    portEXIT_CRITICAL(&s_cloud_mux);
    vTaskDelete(NULL);
}

int kuku_baidu_reset_start(void) {
    portENTER_CRITICAL(&s_cloud_mux);
    if (s_reset_running || g_kuku.recording || g_kuku.playing ||
        s_upload_task_running || s_probe_task_running || s_cloud_task_running ||
        s_image_status == 1 || s_image_status == 3) {
        portEXIT_CRITICAL(&s_cloud_mux);
        return -3;
    }
    s_reset_running = true;
    s_auth_cancel = true;
    s_reset_status = 1;
    portEXIT_CRITICAL(&s_cloud_mux);
    if (xTaskCreate(reset_task, "kuku_bd_reset", 4096, NULL, 4, NULL) != pdPASS) {
        portENTER_CRITICAL(&s_cloud_mux);
        s_reset_running = s_auth_cancel = false;
        s_reset_status = -1;
        portEXIT_CRITICAL(&s_cloud_mux);
        return -4;
    }
    return 0;
}

int kuku_baidu_reset_status(void) { return s_reset_status; }

// ---- 分片响应 ---------------------------------------------------------------
// superfile2 成功响应只有 md5/request_id，不保证携带 errno=0。
static int part_response_status(const char *resp, char md5[33]) {
    md5[0] = 0;
    cJSON *root = cJSON_Parse(resp);
    if (!root) return -101;
    const cJSON *errno_j = cJSON_GetObjectItem(root, "errno");
    const cJSON *md5_j = cJSON_GetObjectItem(root, "md5");
    int status = -3;
    if (cJSON_IsNumber(errno_j)) {
        status = (int)errno_j->valuedouble;
    } else if (cJSON_IsString(md5_j) && strlen(md5_j->valuestring) == 32) {
        strlcpy(md5, md5_j->valuestring, 33);
        status = 0;
    }
    if (status == 0 && cJSON_IsString(md5_j) && strlen(md5_j->valuestring) == 32)
        strlcpy(md5, md5_j->valuestring, 33);
    cJSON_Delete(root);
    return status;
}

static bool block_md5_at(const char *json, int index, char md5[33]) {
    const char *p = json;
    for (int i = 0; i <= index; i++) {
        p = strchr(p, '"');
        if (!p) return false;
        const char *end = strchr(++p, '"');
        if (!end || end - p != 32) return false;
        if (i == index) {
            memcpy(md5, p, 32);
            md5[32] = 0;
            return true;
        }
        p = end + 1;
    }
    return false;
}

// ---- 上传 -------------------------------------------------------------------
// Create exactly this directory. rtype=0 never renames or overwrites a
// conflicting path. An existing path is accepted only after listing it proves
// it is a directory; a same-named file must leave the recording pending.
static int ensure_cloud_dir(const char *dir) {
    char encoded[3 * 80], body[3 * 80 + 32], url[640], resp[2048] = {0};
    if (strlen(dir) >= 80) return -1;
    url_encode(dir, encoded, sizeof(encoded));
    snprintf(url, sizeof(url),
        "https://pan.baidu.com/rest/2.0/xpan/file?method=create&access_token=%s", s_access);
    int n = snprintf(body, sizeof(body), "path=%s&isdir=1&rtype=0", encoded);
    if (http_req(url, HTTP_METHOD_POST, body, n,
                 "application/x-www-form-urlencoded", resp, sizeof(resp), 15000) != 0) return -1;
    cJSON *root = cJSON_Parse(resp);
    const cJSON *err = root ? cJSON_GetObjectItem(root, "errno") : NULL;
    int code = cJSON_IsNumber(err) ? (int)err->valuedouble : -100;
    const cJSON *isdir = root ? cJSON_GetObjectItem(root, "isdir") : NULL;
    const cJSON *path = root ? cJSON_GetObjectItem(root, "path") : NULL;
    bool created = code == 0 && cJSON_IsNumber(isdir) && isdir->valueint == 1 &&
                   cJSON_IsString(path) && !strcmp(path->valuestring, dir);
    cJSON_Delete(root);
    if (created) return 0;
    if (code == 111) return -111;
    if (code != -8) return -1;
    snprintf(url, sizeof(url),
        "https://pan.baidu.com/rest/2.0/xpan/file?method=list&access_token=%s&dir=%s&start=0&limit=1",
        s_access, encoded);
    if (http_req(url, HTTP_METHOD_GET, NULL, 0, NULL, resp, sizeof(resp), 15000) != 0) return -1;
    root = cJSON_Parse(resp);
    err = root ? cJSON_GetObjectItem(root, "errno") : NULL;
    code = cJSON_IsNumber(err) ? (int)err->valuedouble : -100;
    bool exists = code == 0 && cJSON_IsArray(cJSON_GetObjectItem(root, "list"));
    cJSON_Delete(root);
    return exists ? 0 : code == 111 ? -111 : -1;
}

static int prepare_recording_directory(const char *name) {
    char remote[160];
    if (!kuku_cloud_recording_path(name, remote, sizeof(remote))) return -1;
    *strrchr(remote, '/') = 0;
    int rc = ensure_cloud_dir(KUKU_CLOUD_ROOT);
    return rc == 0 ? ensure_cloud_dir(remote) : rc;
}

// 三步上传单个文件。返回 0 成功。
static int upload_file(const char *name) {
    char path[48];
    snprintf(path, sizeof(path), "/rec/%s", name);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size <= KUKU_WAV_HEADER_BYTES) { fclose(f); return -1; }

    char remote[160];
    if (!kuku_cloud_recording_path(name, remote, sizeof(remote))) {
        fclose(f); return -1;
    }
    char remote_enc[3 * sizeof(remote)];
    url_encode(remote, remote_enc, sizeof(remote_enc));

    char bljson[256];
    upload_diag_phase(2);
    if (md5_block_list(path, size, bljson, sizeof(bljson)) != 0) {
        fclose(f); return -1;
    }
    char bl_enc[3 * sizeof(bljson)];
    url_encode(bljson, bl_enc, sizeof(bl_enc));

    // Only one HTTP phase is live at a time. Share its scratch buffers so
    // the upload task leaves memory for the certificate handshake.
    union {
        struct { char body[1024], url[640], resp[1024]; } request;
        struct { char url[1120], resp[512]; } part;
    } scratch;

    // 1. precreate
    upload_diag_phase(3);
    char uploadid[80] = {0};
    int rtype = 1;
    {
        char *body=scratch.request.body, *url=scratch.request.url, *resp=scratch.request.resp;
        int n = snprintf(body, sizeof(scratch.request.body),
            "path=%s&size=%ld&isdir=0&autoinit=1&block_list=%s",
            remote_enc, size, bl_enc);
        snprintf(url, sizeof(scratch.request.url),
            "https://pan.baidu.com/rest/2.0/xpan/file?method=precreate&access_token=%s",
            s_access);
        if (http_req(url, HTTP_METHOD_POST, body, n,
                     "application/x-www-form-urlencoded", resp, sizeof(scratch.request.resp), 15000) != 0) {
            fclose(f); return -1;
        }
        cJSON *root = cJSON_Parse(resp);
        if (!root) { fclose(f); return -1; }
        const cJSON *upid = cJSON_GetObjectItem(root, "uploadid");
        const cJSON *rtype_j = cJSON_GetObjectItem(root, "return_type");
        const cJSON *errno_j = cJSON_GetObjectItem(root, "errno");
        if (cJSON_IsString(upid)) {
            strlcpy(uploadid, upid->valuestring, sizeof(uploadid));
            if (cJSON_IsNumber(rtype_j)) rtype = (int)rtype_j->valuedouble;
        } else {
            int e = cJSON_IsNumber(errno_j) ? (int)errno_j->valuedouble : -100;
            ESP_LOGW(TAG, "precreate errno=%d resp=%.100s", e, resp);
            cJSON_Delete(root); fclose(f);
            return e == 111 ? -111 : -2;   // 111: token 失效
        }
        cJSON_Delete(root);
    }

    if (rtype != 2) {   // 2 = 秒传成功,跳过分片
        // 2. superfile2 逐片(文件流式,不进 RAM)
        int nblk = (int)((size + KUKU_BD_BLOCK - 1) / KUKU_BD_BLOCK);
        for (int i = 0; i < nblk; i++) {
            long off = (long)i * KUKU_BD_BLOCK;
            long len = size - off > KUKU_BD_BLOCK ? KUKU_BD_BLOCK : size - off;
            fseek(f, off, SEEK_SET);
            char *url=scratch.part.url, *resp=scratch.part.resp;
            resp[0]=0;
            snprintf(url, sizeof(scratch.part.url),
                "https://d.pcs.baidu.com/rest/2.0/pcs/superfile2?method=upload"
                "&access_token=%s&type=tmpfile&path=%s&uploadid=%s&partseq=%d",
                s_access, remote_enc, uploadid, i);
            upload_diag_phase(4);
            int prc = http_upload_part(url, f, len, resp, sizeof(scratch.part.resp));
            char local_md5[33] = {0}, remote_md5[33] = {0};
            int pe = prc == 0 ? part_response_status(resp, remote_md5) : -3;
            if (pe == 0 &&
                (!block_md5_at(bljson, i, local_md5) ||
                 strcmp(local_md5, remote_md5) != 0)) {
                ESP_LOGW(TAG, "part %d md5 mismatch local=%s remote=%s", i,
                         local_md5, remote_md5);
                pe = -104;
            }
            if (pe != 0) {
                ESP_LOGW(TAG, "part %d fail prc=%d errno=%d resp=%.80s", i, prc, pe, resp);
                fclose(f);
                return pe == 111 ? -111 : -3;
            }
            ESP_LOGI(TAG, "PART: %s #%d md5=%s", name, i, remote_md5);
        }
    }
    fclose(f);

    // 3. create 合并
    upload_diag_phase(5);
    char *body=scratch.request.body, *url=scratch.request.url, *resp=scratch.request.resp;
    int n = snprintf(body, sizeof(scratch.request.body),
        "path=%s&size=%ld&isdir=0&uploadid=%s&block_list=%s",
        remote_enc, size, uploadid, bl_enc);
    snprintf(url, sizeof(scratch.request.url),
        "https://pan.baidu.com/rest/2.0/xpan/file?method=create&access_token=%s",
        s_access);
    if (http_req(url, HTTP_METHOD_POST, body, n,
                 "application/x-www-form-urlencoded", resp, sizeof(scratch.request.resp), 15000) != 0)
        return -4;
    int ce = -102;
    cJSON *root = cJSON_Parse(resp);
    if (root) {
        const cJSON *ce_j = cJSON_GetObjectItem(root, "errno");
        if (cJSON_IsNumber(ce_j)) ce = (int)ce_j->valuedouble;
        cJSON_Delete(root);
    }
    if (ce != 0) {
        ESP_LOGW(TAG, "create errno=%d resp=%.100s", ce, resp);
        return ce == 111 ? -111 : -5;
    }

    // Publish completion under the same catalog lock as recording publication.
    upload_diag_phase(6);
    if (kuku_rec_mark_uploaded(name) != 0) {
        ESP_LOGW(TAG, "已上传文件标记失败，保留待传: %s", name);
        return -6;
    }
    ESP_LOGI(TAG, "UP: %s -> %s (%ld B)", name, remote, size);
    return 0;
}

// 上传一轮:所有未同步(.WAV)文件逐个上传。
static void upload_task(void *arg) {
    (void)arg;
    set_state(BD_UPLOADING);
    // Keep ownership of TLS and this task stack across queued batches.
    esp_wifi_set_ps(WIFI_PS_NONE);
    s_up_total = s_up_done = 0;

    int64_t now_epoch = time(NULL);
    if (s_at_exp_boot > 0 && now_epoch > 1600000000 && now_epoch > s_at_exp_boot) {
        if (token_refresh() != 0) {
            set_state(BD_NO_AUTH);
            goto done;
        }
    }
    for (;;) {
        s_up_total = s_up_done = 0;
        int capacity = kuku_rec_count();
        char (*batch)[KUKU_MAX_NAME] = capacity > 0 ? malloc((size_t)capacity * sizeof(*batch)) : NULL;
        if (capacity > 0 && !batch) break;
        int count = kuku_rec_snapshot(batch, capacity);
        for (int i = 0; i < count; i++) {
            if (!g_kuku.wifi_up) break;
            const char *name = batch[i];
            size_t l = strlen(name);
            if (l < 8 || strcmp(name + l - 4, ".WAV") != 0) continue;
            s_up_total++;
            strlcpy(s_up_name, name, sizeof(s_up_name));
            int rc = -1;
            for (int attempt = 1; attempt <= KUKU_BD_UPLOAD_ATTEMPTS; attempt++) {
                upload_diag_begin();
                rc = prepare_recording_directory(name);
                if (rc == 0) rc = upload_file(name);
                upload_diag_end(rc);
                if (rc == 0 || rc == -111 || !g_kuku.wifi_up) break;
                if (attempt < KUKU_BD_UPLOAD_ATTEMPTS) {
                    ESP_LOGW(TAG, "UP: %s 传输失败 rc=%d, 第 %d/%d 次重试", name, rc,
                             attempt + 1, KUKU_BD_UPLOAD_ATTEMPTS);
                    vTaskDelay(pdMS_TO_TICKS(2000));
                }
            }
            if (rc == -111) {
                if (token_refresh() == 0) {
                    upload_diag_begin();
                    rc = prepare_recording_directory(name);
                    if (rc == 0) rc = upload_file(name);
                    upload_diag_end(rc);
                } else {
                    set_state(BD_NO_AUTH);
                    free(batch);
                    goto done;
                }
            }
            if (rc == 0) {
                s_up_done++;
                // Reclaim confirmed files before a later transfer can stall.
                // Only .UPD is removed; unsynced WAVs stay intact.
                kuku_rec_clean_synced();
            }
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        free(batch);
        portENTER_CRITICAL(&s_cloud_mux);
        s_cloud_status = 0;
        s_cloud_count = 0;
        s_cloud_has_more = false;
        bool again = s_upload_requested && g_kuku.wifi_up;
        s_upload_requested = false;
        portEXIT_CRITICAL(&s_cloud_mux);
        ESP_LOGI(TAG, "UP: 本轮 %d/%d 完成", s_up_done, s_up_total);
        if (!again) break;
        // Newly closed recordings use the same worker, without a second
        // 9 KiB task allocation or a telemetry TLS window between batches.
    }
done:
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    kuku_rec_refresh();
    int pending_count = kuku_rec_count();
    s_up_name[0] = 0;
    portENTER_CRITICAL(&s_cloud_mux);
    s_upload_requested = pending_count > 0 || s_upload_requested;
    if (g_kuku.bd_state == BD_UPLOADING)
        set_state(s_access[0] ? BD_READY : BD_NO_AUTH);
    s_upload_task_running = false;
    portEXIT_CRITICAL(&s_cloud_mux);
    if (s_up_total > 0)
        kuku_ui_set_status(pending_count == 0 ? "已同步网盘" : "同步失败，待重试");
    vTaskDelete(NULL);
}

int kuku_baidu_upload_pass(void) {
    if (!g_kuku.wifi_up) return -2;
    if ((g_kuku.bd_state != BD_READY && g_kuku.bd_state != BD_UPLOADING) ||
        s_access[0] == 0) return -1;
    portENTER_CRITICAL(&s_cloud_mux);
    if (s_upload_task_running || s_auth_task_running || s_probe_task_running ||
        s_reset_running || !s_access[0]) {
        s_upload_requested = true;
        portEXIT_CRITICAL(&s_cloud_mux);
        return -3;
    }
    s_upload_task_running = true;
    s_upload_requested = false;
    portEXIT_CRITICAL(&s_cloud_mux);
    if (xTaskCreate(upload_task, "kuku_bd_up", KUKU_BD_UPLOAD_STACK,
                    NULL, 4, NULL) != pdPASS) {
        portENTER_CRITICAL(&s_cloud_mux);
        s_upload_task_running = false;
        s_upload_requested = true;
        portEXIT_CRITICAL(&s_cloud_mux);
        return -4;
    }
    return 0;
}

void kuku_baidu_on_recording_saved(void) {
    // 保存完成才排队；若上一轮仍在上传，结束时再扫一遍新 WAV。
    portENTER_CRITICAL(&s_cloud_mux);
    s_upload_requested = true;
    portEXIT_CRITICAL(&s_cloud_mux);
    int rc = kuku_baidu_upload_pass();
    if (rc != 0 && rc != -1 && rc != -2 && rc != -3)
        ESP_LOGW(TAG, "录音后上传派发失败 rc=%d", rc);
}

// ---- UI 读取接口 --------------------------------------------------------------
void kuku_baidu_get_auth(char *url, size_t ucap, char *code, size_t ccap) {
    strlcpy(url, s_verify_url, ucap);
    strlcpy(code, s_user_code, ccap);
}

void kuku_baidu_get_progress(char *name, size_t cap, int *done, int *total) {
    strlcpy(name, s_up_name, cap);
    *done = s_up_done;
    *total = s_up_total;
}

// Wi-Fi 事件入口(main.c 的 wifi 事件回调转发)。
void kuku_baidu_on_wifi(bool up) {
    if (up) {
        sntp_start_once();
        kuku_baidu_upload_pass();   // 已授权就自动补传
    }
}

void kuku_baidu_retry_pending(void) {
    // The 60-second heartbeat retries a failed batch even if Wi-Fi never
    // transitions through disconnected. No storage or network I/O here.
    if (s_upload_requested && g_kuku.wifi_up &&
        g_kuku.bd_state == BD_READY && !s_probe_task_running)
        kuku_baidu_upload_pass();
}

void kuku_baidu_init(void) {
    nvs_load_tokens();   // 有存 token → 直接 BD_READY
    if (g_kuku.bd_state == BD_READY) ESP_LOGI(TAG, "已存授权 token,上传就绪");
    // Convert the portrait saved by older firmware before the first UI draw.
    FILE *avatar = fopen(KUKU_IMAGE_AVATAR_PATH, "rb");
    if (avatar) {
        fclose(avatar);
    } else if (kuku_baidu_jpeg_baseline(KUKU_IMAGE_LEGACY_AVATAR_PATH) &&
               kuku_image_render_bmp(KUKU_IMAGE_LEGACY_AVATAR_PATH,
                                     KUKU_BD_AVATAR_TMP, 64) == 0) {
        if (rename(KUKU_BD_AVATAR_TMP, KUKU_IMAGE_AVATAR_PATH) != 0)
            remove(KUKU_BD_AVATAR_TMP);
    }
}
