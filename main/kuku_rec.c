// main/kuku_rec.c —— 录音引擎:ES8311 采集 → FATFS 落盘;以及本地回放。
//
// 设计要点:
// - PCM WAV 16 kHz/16bit/单声道,32000 bytes/s,5 MB 分区约 2.5 分钟。
// - 停止时回写 WAV 头的长度字段(fseek 到 0 重写 44 字节),保证断电残留
//   文件至少头部仍指向前一次 flush 的长度 —— 但断电瞬间的 data 长度字段
//   仍是初始 0,播放器按 0 长度处理;数据本体在,可用工具修复。v0.1 接受。
// - 时间有效时文件名 YYYYMMDD_HHMMSS.WAV；同秒录音追加序号。
//   未校时时回退 REC00000001.WAV，序号持久化在 NVS，只增不减。
// - 录音与回放互斥;回放走同一 I2S(16k/16/1,无需切格式)。
#include "kuku_app.h"
#include "kuku_rec_filename.h"
#include "kuku_wav.h"
#include "kuku_rec_progress.h"

#include "bsp_audio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "wear_levelling.h"
#include "esp_vfs_fat.h"
#include "dirent.h"
#include "stdio.h"
#include "string.h"
#include "sys/stat.h"
#include "time.h"
#include "unistd.h"

static const char *TAG = "kuku_rec";

#define REC_MOUNT_POINT "/rec"
#define REC_TASK_STACK 4096
#define CAPTURE_TASK_STACK 3072
// Keep 256 ms of queued PCM while using smaller capture/write/play scratch
// buffers. The queue's time budget must not shrink with the I/O chunk size.
#define REC_BUFFER_BYTES 8192
#define PLAY_TASK_STACK 4096
#define NVS_NS "kuku"

kuku_state_t g_kuku;   // 全局状态(在头文件声明为 extern)

static wl_handle_t s_wl = WL_INVALID_HANDLE;
static bool s_mounted;
// Only catalog operations hold this mutex; never hold it across PCM or HTTPS.
static SemaphoreHandle_t s_catalog_mutex;
static TaskHandle_t s_rec_task;
static volatile bool s_rec_stop_req;
static int s_rec_result;
static char s_rec_file[KUKU_MAX_NAME];
static StreamBufferHandle_t s_pcm_stream;
static volatile bool s_capture_done;
static volatile bool s_capture_failed;
static volatile uint32_t s_capture_bytes;
static uint32_t s_seq;

static TaskHandle_t s_play_task;
static volatile bool s_play_stop_req;
static char s_play_file[KUKU_MAX_NAME];
static volatile uint32_t s_play_done_ms;

// ---- 文件枚举(带缓存;录音完成后失效) ---------------------------------
struct kuku_rec_entry {
    char name[KUKU_MAX_NAME];
};

static struct kuku_rec_entry s_cache[KUKU_MAX_FILES];
static int s_cache_n = -1;

static void cache_invalidate(void) {
    s_cache_n = -1;
}

void kuku_rec_refresh(void) {
    if (!s_catalog_mutex) return;
    xSemaphoreTake(s_catalog_mutex, portMAX_DELAY);
    cache_invalidate();
    xSemaphoreGive(s_catalog_mutex);
}

static void cache_load(void) {
    if (s_cache_n >= 0) return;
    int n = 0;
    DIR *d = opendir(REC_MOUNT_POINT);
    if (!d) {
        s_cache_n = 0;
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < KUKU_MAX_FILES) {
        size_t len = strlen(e->d_name);
        if (len < 8 || strcmp(e->d_name + len - 4, ".WAV") != 0) continue;
        strlcpy(s_cache[n].name, e->d_name, KUKU_MAX_NAME);
        n++;
    }
    closedir(d);
    // 按名字升序 = 时间升序(REC 序号单调)。
    for (int i = 1; i < n; i++) {
        struct kuku_rec_entry key = s_cache[i];
        int j = i - 1;
        while (j >= 0 && strcmp(s_cache[j].name, key.name) > 0) {
            s_cache[j + 1] = s_cache[j];
            j--;
        }
        s_cache[j + 1] = key;
    }
    s_cache_n = n;
}

int kuku_rec_count(void) {
    if (!s_catalog_mutex) return 0;
    xSemaphoreTake(s_catalog_mutex, portMAX_DELAY);
    cache_load();
    int count = s_cache_n;
    xSemaphoreGive(s_catalog_mutex);
    return count;
}

bool kuku_rec_name_copy(int idx, char *out, size_t cap) {
    if (!s_catalog_mutex || !out || !cap) return false;
    xSemaphoreTake(s_catalog_mutex, portMAX_DELAY);
    cache_load();
    bool found = idx >= 0 && idx < s_cache_n;
    if (found) strlcpy(out, s_cache[idx].name, cap);
    else out[0] = 0;
    xSemaphoreGive(s_catalog_mutex);
    return found;
}

int kuku_rec_snapshot(char (*names)[KUKU_MAX_NAME], int cap) {
    if (!s_catalog_mutex || !names || cap <= 0) return 0;
    xSemaphoreTake(s_catalog_mutex, portMAX_DELAY);
    cache_load();
    int count = s_cache_n < cap ? s_cache_n : cap;
    for (int i = 0; i < count; ++i)
        strlcpy(names[i], s_cache[i].name, KUKU_MAX_NAME);
    xSemaphoreGive(s_catalog_mutex);
    return count;
}

int kuku_rec_mark_uploaded(const char *name) {
    char path[64], synced[64];
    snprintf(path, sizeof(path), REC_MOUNT_POINT "/%s", name);
    strlcpy(synced, path, sizeof(synced));
    size_t len = strlen(synced);
    if (len < 4 || strcmp(synced + len - 4, ".WAV")) return -1;
    strlcpy(synced + len - 4, ".UPD", 5);
    xSemaphoreTake(s_catalog_mutex, portMAX_DELAY);
    int rc = rename(path, synced);
    cache_invalidate();
    xSemaphoreGive(s_catalog_mutex);
    return rc;
}

long kuku_rec_free_kb(void) {
    if (!s_mounted) return -1;
    // WL 分区 FATFS 扇区为 4096B,手工按 512B 换算会差 8 倍;
    // 官方 API 自带正确扇区尺寸。
    uint64_t total = 0, free_b = 0;
    if (esp_vfs_fat_info(REC_MOUNT_POINT, &total, &free_b) != ESP_OK) return -1;
    return (long)(free_b / 1024);
}

uint32_t kuku_rec_file_ms(const char *name) {
    if (!s_mounted || !name) return 0;
    char path[48];
    snprintf(path, sizeof(path), REC_MOUNT_POINT "/%s", name);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    if (sz <= KUKU_WAV_HDR) return 0;
    return kuku_wav_ms((uint32_t)(sz - KUKU_WAV_HDR), KUKU_SAMPLE_RATE_HZ, 1, 16);
}

bool kuku_rec_current_name(char *out, size_t cap) {
    if (!g_kuku.recording || !out || cap == 0) return false;
    strlcpy(out, s_rec_file, cap);
    return true;
}

// A power loss leaves .REC outside the upload catalog. Repair its WAV length
// on boot, then publish it; never upload a still-open or unfinalized header.
static void recover_recordings(void) {
    DIR *dir = opendir(REC_MOUNT_POINT);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        size_t len = strlen(entry->d_name);
        if (len < 8 || len >= KUKU_MAX_NAME || strcmp(entry->d_name + len - 4, ".REC")) continue;
        char path[64], final[64];
        snprintf(path, sizeof(path), REC_MOUNT_POINT "/%.31s", entry->d_name);
        strlcpy(final, path, sizeof(final));
        strlcpy(final + strlen(final) - 4, ".WAV", 5);
        struct stat existing;
        if (stat(final, &existing) == 0) continue;
        FILE *file = fopen(path, "rb+");
        if (!file) continue;
        uint8_t header[KUKU_WAV_HDR];
        bool valid = fread(header, 1, sizeof(header), file) == sizeof(header) &&
                     !memcmp(header, "RIFF", 4) && !memcmp(header + 8, "WAVE", 4);
        bool sized = fseek(file, 0, SEEK_END) == 0;
        long size = sized ? ftell(file) : -1;
        valid = valid && size > KUKU_WAV_HDR;
        if (valid) {
            uint32_t bytes = (uint32_t)(size - KUKU_WAV_HDR) & ~1U;
            kuku_wav_fill(header, KUKU_SAMPLE_RATE_HZ, 1, 16, bytes);
            valid = fseek(file, 0, SEEK_SET) == 0 &&
                    fwrite(header, 1, sizeof(header), file) == sizeof(header);
        }
        valid = fclose(file) == 0 && valid;
        if (valid && rename(path, final) == 0)
            ESP_LOGI(TAG, "Recovered: %s", entry->d_name);
    }
    closedir(dir);
}

// ---- 初始化 -------------------------------------------------------------
int kuku_rec_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要擦除重初始化(会丢 Wi-Fi 配置与序号)");
        // 注意:不自动擦除 —— NVS 里可能有 LEO RADIO 留下的 Wi-Fi 凭据。
        // v0.1 返回错误,由用户决定。正常情况下社区固件不会让 NVS 版本不兼容。
        return -1;
    }
    if (err != ESP_OK) return -1;

    const esp_vfs_fat_mount_config_t cfg = {
        .max_files = 4,
        .format_if_mount_failed = true,   // 首次挂载必格式化
        .allocation_unit_size = 4096,
    };
    err = esp_vfs_fat_spiflash_mount_rw_wl(REC_MOUNT_POINT, "storage", &cfg, &s_wl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "录音分区挂载失败: %s", esp_err_to_name(err));
        return -1;
    }
    s_catalog_mutex = xSemaphoreCreateMutex();
    if (!s_catalog_mutex) return -1;
    s_mounted = true;
    recover_recordings();

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_get_u32(h, "seq", &s_seq);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "存储就绪: %d 个录音, 剩余 %ld KB, 下一序号 %lu",
             kuku_rec_count(), kuku_rec_free_kb(), (unsigned long)s_seq);
    return 0;
}

static uint32_t next_seq(void) {
    s_seq++;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, "seq", s_seq);
        nvs_commit(h);
        nvs_close(h);
    }
    return s_seq;
}

static bool rec_name_exists(const char *name) {
    char path[64];
    snprintf(path, sizeof(path), REC_MOUNT_POINT "/%s", name);
    struct stat st;
    if (stat(path, &st) == 0) return true;
    char active_path[64];
    strlcpy(active_path, path, sizeof(active_path));
    strlcpy(active_path + strlen(active_path) - 4, ".REC", 5);
    if (stat(active_path, &st) == 0) return true;

    // Synced files are renamed to .UPD and must also reserve their timestamp so
    // a later recording never reuses the same remote filename.
    char synced[KUKU_MAX_NAME];
    strlcpy(synced, name, sizeof(synced));
    char *ext = strrchr(synced, '.');
    if (ext) strlcpy(ext, ".UPD", (size_t)(synced + sizeof(synced) - ext));
    snprintf(path, sizeof(path), REC_MOUNT_POINT "/%s", synced);
    return stat(path, &st) == 0;
}

static bool choose_rec_name_at(char *out, size_t cap, bool consume_sequence, time_t now) {
    uint32_t seq = consume_sequence ? next_seq() : s_seq + 1;

    if (now >= (time_t)1600000000) {
        for (unsigned duplicate = 0; duplicate < 1000; duplicate++) {
            if (!kuku_rec_filename_build(now, seq, duplicate, out, cap)) return false;
            if (!rec_name_exists(out)) return true;
        }
    }

    // No synchronized clock, or every timestamp suffix was occupied. Sequence
    // fallback remains unique even if NVS was restored while old files remain.
    for (unsigned attempt = 0; attempt < 1000; attempt++) {
        if (!kuku_rec_filename_build(0, seq, 0, out, cap)) return false;
        if (!rec_name_exists(out)) return true;
        seq = consume_sequence ? next_seq() : seq + 1;
    }
    out[0] = 0;
    return false;
}

static bool choose_rec_name(char *out, size_t cap, bool consume_sequence) {
    return choose_rec_name_at(out, cap, consume_sequence, time(NULL));
}

bool kuku_rec_preview_name(char *out, size_t cap) {
    if (!s_mounted || !out || cap == 0) return false;
    return choose_rec_name(out, cap, false);
}

int kuku_rec_clean_synced(void) {
    if (!s_mounted || g_kuku.playing) return -1;
    xSemaphoreTake(s_catalog_mutex, portMAX_DELAY);
    DIR *d = opendir(REC_MOUNT_POINT);
    if (!d) { xSemaphoreGive(s_catalog_mutex); return -1; }

    int removed = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (len < 5 || len >= KUKU_MAX_NAME ||
            strcmp(e->d_name + len - 4, ".UPD") != 0) continue;
        char path[sizeof(REC_MOUNT_POINT) + KUKU_MAX_NAME];
        strlcpy(path, REC_MOUNT_POINT "/", sizeof(path));
        strlcat(path, e->d_name, sizeof(path));
        if (unlink(path) == 0) {
            removed++;
        } else {
            ESP_LOGW(TAG, "删除已同步文件失败: %s", path);
        }
    }
    closedir(d);
    cache_invalidate();
    xSemaphoreGive(s_catalog_mutex);
    ESP_LOGI(TAG, "已清理 %d 个同步文件, 剩余 %ld KB", removed, kuku_rec_free_kb());
    return removed;
}

// ---- 录音任务 ------------------------------------------------------------
// A dedicated high-priority reader drains I2S even while the writer waits for
// FAT/Flash. The 8 KiB queue holds about 256 ms of PCM without growing with
// recording length. Upload reads are bounded and run at lower priority.
static void capture_task(void *arg) {
    (void)arg;
    static int16_t pcm[KUKU_REC_CHUNK_BYTES / 2];
    int64_t start_us = esp_timer_get_time();
    while (!s_rec_stop_req) {
        if (bsp_audio_read(pcm, sizeof(pcm)) != ESP_OK) {
            s_capture_failed = true;
            break;
        }
        size_t sent = xStreamBufferSend(s_pcm_stream, pcm, sizeof(pcm),
                                        pdMS_TO_TICKS(50));
        s_capture_bytes += sent;
        kuku_rec_progress_capture((uint32_t)sent);
        g_kuku.rec_ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000);
        if (sent != sizeof(pcm)) {
            s_capture_failed = true;
            ESP_LOGE(TAG, "PCM queue overrun: %u/%u", (unsigned)sent, (unsigned)sizeof(pcm));
            break;
        }
        // File rotation does not restart capture; a network loss ends the
        // session after this chunk so the writer can safely save it.
        if (!g_kuku.wifi_up) break;
    }
    s_capture_done = true;
    vTaskDelete(NULL);
}

// Close each completed segment before publishing it to the uploader. The
// capture worker keeps draining I2S during rotation; queued PCM crosses the
// boundary by exact sample bytes rather than stopping/restarting the microphone.
static bool rec_segment_open(FILE **file, char *path, char *final_path, time_t started) {
    if (!choose_rec_name_at(s_rec_file, sizeof(s_rec_file), true, started)) return false;
    snprintf(final_path, 48, REC_MOUNT_POINT "/%s", s_rec_file);
    strlcpy(path, final_path, 48);
    strlcpy(path + strlen(path) - 4, ".REC", 5);
    *file = fopen(path, "wb");
    if (!*file) return false;
    uint8_t hdr[KUKU_WAV_HDR];
    kuku_wav_fill(hdr, KUKU_SAMPLE_RATE_HZ, 1, 16, 0);
    return fwrite(hdr, 1, sizeof(hdr), *file) == sizeof(hdr);
}

static bool rec_segment_finish(FILE **file, const char *path, const char *final_path,
                               uint32_t written, bool io_ok) {
    uint8_t hdr[KUKU_WAV_HDR];
    kuku_wav_fill(hdr, KUKU_SAMPLE_RATE_HZ, 1, 16, written);
    bool finalized = fseek(*file, 0, SEEK_SET) == 0;
    finalized = fwrite(hdr, 1, sizeof(hdr), *file) == sizeof(hdr) && finalized && io_ok;
    finalized = fclose(*file) == 0 && finalized;
    *file = NULL;
    xSemaphoreTake(s_catalog_mutex, portMAX_DELAY);
    bool published = written > 0 && finalized && rename(path, final_path) == 0;
    cache_invalidate();
    xSemaphoreGive(s_catalog_mutex);
    if (published) {
        ESP_LOGI(TAG, "SEGMENT: %s bytes=%lu", s_rec_file, (unsigned long)written);
        kuku_baidu_on_recording_saved();
    } else if (written > 0) {
        ESP_LOGE(TAG, "录音发布失败，保留 REC 文件");
    }
    return published;
}

static void rec_task(void *arg) {
    (void)arg;
    char path[48], final_path[48];
    FILE *f = NULL;
    time_t started = time(NULL);
    uint64_t total = 0;
    uint32_t written = 0, segments = 0;
    const uint32_t segment_bytes = KUKU_SAMPLE_RATE_HZ * 2U * KUKU_REC_SEGMENT_SECONDS;
    bool io_ok = rec_segment_open(&f, path, final_path, started);
    bool stop_by_space = false;
    s_capture_done = false;
    s_capture_failed = false;
    s_capture_bytes = 0;
    if (!io_ok || xTaskCreate(capture_task, "kuku_capture", CAPTURE_TASK_STACK,
                             NULL, 12, NULL) != pdPASS) {
        s_capture_done = true;
        s_capture_failed = true;
    }
    static uint8_t pcm[KUKU_REC_CHUNK_BYTES];
    while (!s_capture_done || xStreamBufferBytesAvailable(s_pcm_stream) > 0) {
        size_t got = xStreamBufferReceive(s_pcm_stream, pcm, sizeof(pcm), pdMS_TO_TICKS(100));
        if (!got) continue;
        size_t offset = 0;
        while (offset < got && io_ok) {
            if (!f) {
                // Use the first sample's timestamp, not upload/rotation time.
                time_t epoch = started >= (time_t)1600000000
                             ? started + (time_t)(total / (KUKU_SAMPLE_RATE_HZ * 2U)) : 0;
                io_ok = rec_segment_open(&f, path, final_path, epoch);
                written = 0;
                if (!io_ok) break;
            }
            size_t bytes = got - offset;
            if (bytes > segment_bytes - written) bytes = segment_bytes - written;
            size_t n = fwrite(pcm + offset, 1, bytes, f);
            offset += n;
            written += n;
            total += n;
            g_kuku.rec_bytes = (uint32_t)total;
            kuku_rec_progress_write(total, segments);
            if (n != bytes) { io_ok = false; break; }
            if (written == segment_bytes) {
                io_ok = rec_segment_finish(&f, path, final_path, written, io_ok);
                written = 0;
                if (io_ok) { ++segments; kuku_rec_progress_write(total, segments); }
            }
        }
        if (!io_ok) {
            s_rec_stop_req = true;
            break;
        }
        // Leave room for queued PCM, WAV finalization and FAT metadata. A
        // failed/offline upload never causes unsynced recordings to be erased.
        long free_kb = kuku_rec_free_kb();
        if (free_kb < 0 || free_kb < 64 + REC_BUFFER_BYTES / 1024) {
            stop_by_space = true;
            s_rec_stop_req = true;
        }
    }
    s_rec_stop_req = true;
    while (!s_capture_done) vTaskDelay(pdMS_TO_TICKS(10));
    if (f) {
        bool published = rec_segment_finish(&f, path, final_path, written, io_ok);
        io_ok = published && io_ok;
        if (published) ++segments;
        kuku_rec_progress_write(total, segments);
    }
    vStreamBufferDelete(s_pcm_stream);
    s_pcm_stream = NULL;
    ESP_LOGI(TAG, "SESSION: segments=%lu bytes=%llu ms=%lu%s",
             (unsigned long)segments, (unsigned long long)total,
             (unsigned long)g_kuku.rec_ms, stop_by_space ? " (空间满自动停)" : "");
    ESP_LOGI(TAG, "PCM: captured=%lu written=%llu failure=%d",
             (unsigned long)s_capture_bytes, (unsigned long long)total, s_capture_failed || !io_ok);
    if (s_capture_failed || !io_ok) kuku_ui_set_status("录音未完成，检查待传文件");
    else if (stop_by_space) kuku_ui_set_status("暂存空间不足，已保存");
    else if (!g_kuku.wifi_up) kuku_ui_set_status("已保存，联网后补传");
    s_rec_result = segments > 0 && !s_capture_failed && io_ok ? 0 : -1;
    kuku_rec_progress_end(s_capture_failed ? KUKU_REC_CAPTURE :
                          !io_ok ? KUKU_REC_STORAGE :
                          stop_by_space ? KUKU_REC_SPACE :
                          !g_kuku.wifi_up ? KUKU_REC_NETWORK : KUKU_REC_MANUAL);
    s_rec_task = NULL;
    g_kuku.recording = false;
    vTaskDelete(NULL);
}

int kuku_rec_start(void) {
    if (g_kuku.recording) return 0;
    if (g_kuku.playing) return -1;
    if (!s_mounted) return -1;
    if (bsp_audio_init() != ESP_OK) return -1;
    if (bsp_audio_set_format(KUKU_SAMPLE_RATE_HZ, 16, 1) != ESP_OK) return -1;

    s_pcm_stream = xStreamBufferCreate(REC_BUFFER_BYTES, KUKU_REC_CHUNK_BYTES);
    if (!s_pcm_stream) return -1;
    s_rec_result = -1;
    kuku_rec_progress_begin();
    s_rec_stop_req = false;
    g_kuku.rec_bytes = 0;
    g_kuku.rec_ms = 0;
    g_kuku.recording = true;   // 先置位再创建任务,避免竞态
    if (xTaskCreate(rec_task, "kuku_rec", REC_TASK_STACK, NULL, 10, &s_rec_task) != pdPASS) {
        g_kuku.recording = false;
        vStreamBufferDelete(s_pcm_stream);
        s_pcm_stream = NULL;
        return -1;
    }
    return 0;
}

int kuku_rec_stop(void) {
    if (!g_kuku.recording) return 0;
    s_rec_stop_req = true;
    // 任务最迟一个 chunk 周期(~128ms)内退出。
    for (int i = 0; i < 50 && s_rec_task; i++) vTaskDelay(pdMS_TO_TICKS(20));
    return s_rec_task ? -1 : s_rec_result;
}

// ---- 回放任务 ------------------------------------------------------------
static void play_task(void *arg) {
    (void)arg;
    char path[48];
    snprintf(path, sizeof(path), REC_MOUNT_POINT "/%s", s_play_file);
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "回放打开失败: %s", path);
        g_kuku.playing = false;
        s_play_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, KUKU_WAV_HDR, SEEK_SET);
    uint32_t total_ms = kuku_wav_ms((uint32_t)(sz - KUKU_WAV_HDR),
                                    KUKU_SAMPLE_RATE_HZ, 1, 16);

    static uint8_t buf[KUKU_REC_CHUNK_BYTES];
    size_t n;
    while (!s_play_stop_req && (n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (bsp_audio_write(buf, n) != ESP_OK) break;
        s_play_done_ms += kuku_wav_ms((uint32_t)n, KUKU_SAMPLE_RATE_HZ, 1, 16);
        kuku_ui_set_play_progress(s_play_done_ms, total_ms);
    }
    fclose(f);
    g_kuku.playing = false;
    ESP_LOGI(TAG, "回放结束: %s", s_play_file);
    s_play_task = NULL;
    vTaskDelete(NULL);
}

int kuku_play_start(const char *name) {
    if (g_kuku.playing || g_kuku.recording || !s_mounted || !name) return -1;
    if (name[0] == '\0') return -1;
    // 只允许本目录下的白名单文件名,防路径注入。
    for (const char *p = name; *p; p++) {
        char c = *p;
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '.')) return -1;
    }
    if (bsp_audio_init() != ESP_OK) return -1;
    if (bsp_audio_set_format(KUKU_SAMPLE_RATE_HZ, 16, 1) != ESP_OK) return -1;
    bsp_audio_set_volume(g_kuku.volume);

    strlcpy(s_play_file, name, sizeof(s_play_file));
    s_play_stop_req = false;
    s_play_done_ms = 0;
    g_kuku.playing = true;
    if (xTaskCreate(play_task, "kuku_play", PLAY_TASK_STACK, NULL, 10,
                    &s_play_task) != pdPASS) {
        g_kuku.playing = false;
        return -1;
    }
    return 0;
}

int kuku_play_stop(void) {
    if (!g_kuku.playing) return 0;
    s_play_stop_req = true;
    for (int i = 0; i < 50 && s_play_task; i++) vTaskDelay(pdMS_TO_TICKS(20));
    return s_play_task ? -1 : 0;
}
