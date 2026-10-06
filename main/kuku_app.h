// main/kuku_app.h —— Netdisk Recording Badge 应用层公共定义。
//
// 网盘录音工牌:电子工牌主页 + 录音 + 无线网络 + 百度网盘。
//   下+确认    任意页/熄屏时开始录音并回工牌主页
//   OK 长按    主页=开始/停止录音;子页=返回
//   OK 短按    主页=进入菜单;列表=确认
//   上/下 短按  菜单/列表=移动;播放中=音量
//   上/下 长按  文件列表=快进 ±6 条
//   下 长按     熄屏(任意键唤醒)
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "kuku_cloud_path.h"

// 录音参数:16 kHz / 16 bit / 单声道 PCM WAV,32000 bytes/s。
#define KUKU_SAMPLE_RATE_HZ 16000
#define KUKU_REC_CHUNK_BYTES 1024
#define KUKU_WAV_HEADER_BYTES 44
#define KUKU_MAX_FILES 200
#define KUKU_MAX_NAME 32
#define KUKU_REC_SEGMENT_SECONDS 60
#define KUKU_TEST_REC_MAX_SECONDS 600
#define KUKU_CLOUD_PAGE_SIZE 10
#define KUKU_CLOUD_NAME_MAX 96

typedef struct {
    uint64_t fs_id;
    uint64_t size;
    uint32_t mtime;
    bool is_dir;
    char name[KUKU_CLOUD_NAME_MAX];
} kuku_cloud_file_t;

// 页面标识。
typedef enum {
    KUKU_PAGE_HOME = 0,
    KUKU_PAGE_MENU,
    KUKU_PAGE_FILES,
    KUKU_PAGE_WIFI,        // 状态页:OK=开始扫描
    KUKU_PAGE_WIFI_SCAN,   // 屏上配网:周围网络列表:OK=选网输密码
    KUKU_PAGE_WIFI_KB,     // 屏上配网:键盘输密码:OK=按键,OK长按=连接
    KUKU_PAGE_WIFI_SAVED,  // 已存网络列表:OK=确认删除
    KUKU_PAGE_WIFI_DELETE, // 删除确认页
    KUKU_PAGE_SHARE_LINK,
    KUKU_PAGE_ABOUT,
    KUKU_PAGE_IMAGE,
    KUKU_PAGE_BD_RESET,
} kuku_page_t;

// 百度网盘状态(g_kuku.bd_state): 0 未授权 1 等用户输码 2 已授权 3 上传中。

// 应用当前状态快照,UI 定时器读取渲染;录音引擎与网络模块写入各自字段。
typedef struct {
    volatile bool recording;       // 正在录音
    volatile bool playing;         // 正在回放
    volatile bool screen_off;      // 背光为 0
    volatile bool wifi_up;         // STA 已连上
    volatile uint8_t bd_state;     // 百度网盘状态(见上)
    volatile uint32_t rec_ms;      // 本段录音已录时长
    volatile uint32_t rec_bytes;   // 本段已写字节(不含头)
    volatile uint8_t volume;       // 回放音量 0..100
} kuku_state_t;

extern kuku_state_t g_kuku;

// UI -----------------------------------------------------------------------
void kuku_ui_init(void);
void kuku_ui_goto(kuku_page_t page);
void kuku_ui_set_status(const char *text);          // 工牌页底部临时提示
void kuku_ui_files_refresh(void);                   // 重建文件列表内容
// 文件页选中项:1 本地待传,2 云端文件,3 上一页,4 下一页,0 无。
int kuku_ui_files_selected(kuku_cloud_file_t *cloud, char *local, size_t cap);
void kuku_ui_files_reset_sel(void);
void kuku_ui_share_open(const kuku_cloud_file_t *file);
void kuku_ui_image_open(const kuku_cloud_file_t *file);
void kuku_ui_image_refresh(void);
void kuku_ui_wifi_refresh(void);                    // 刷新 Wi-Fi 页
void kuku_ui_saved_refresh(void);
void kuku_ui_saved_move(int delta);
int  kuku_ui_saved_sel(void);
void kuku_ui_reset_move(void);
bool kuku_ui_reset_confirmed(void);
int kuku_ui_reset_status(void);
void kuku_ui_reset_feedback(int result);
void kuku_ui_baidu_refresh(void);                   // 刷新未授权页
void kuku_ui_set_play_progress(uint32_t done_ms, uint32_t total_ms);
void kuku_ui_timer_start(void);
int  kuku_ui_menu_sel(void);
void kuku_ui_menu_move(int delta);
void kuku_ui_guide_move(int delta);
int  kuku_ui_files_sel(void);
void kuku_ui_files_move(int delta);
// 屏上配网(扫描列表 + 键盘,状态由 kuku_ui.c 持有):
void kuku_ui_scan_move(int delta);
int  kuku_ui_scan_sel(void);
void kuku_ui_kb_open(const char *ssid);   // 进入键盘页(选定 SSID)
int  kuku_ui_kb_key(void);                // 按下当前键:0=已处理 1=GO连接 2=退回列表
void kuku_ui_kb_move(int delta);
void kuku_ui_kb_get(char *ssid, size_t scap, char *pass, size_t pcap);

// Wi-Fi(桥接 esp-wifi-connect,实现见 kuku_wifi.cc) ----------------------------
#include "kuku_wifi.h"

// 百度网盘 --------------------------------------------------------------------
void kuku_baidu_init(void);
int  kuku_baidu_auth_start(void);                   // 0=开始 1=已授权 <0 失败
int  kuku_baidu_reset_start(void); // Async local logout; -3 busy, -4 task failure.
int  kuku_baidu_reset_status(void); // Last reset result: 0 idle, 1 busy, 2 done, -1 error.
int  kuku_baidu_upload_pass(void);                  // 0=已派发任务
void kuku_baidu_on_wifi(bool up);
void kuku_baidu_retry_pending(void);               // 心跳触发失败补传
void kuku_baidu_on_recording_saved(void);           // WAV 关闭后请求补传
// 0=已派发联网检查并将在通过后开始录音；负数=立即拒绝。
// -1 未授权，-2 无 Wi-Fi，-3 正忙，-6 派发失败。
// 待传文件、空间不足、网盘不可达由后台检查后提示，不启动录音。
int  kuku_baidu_request_recording(void);
void kuku_baidu_get_auth(char *url, size_t ucap, char *code, size_t ccap);
void kuku_baidu_get_progress(char *name, size_t cap, int *done, int *total);
// Retained until reboot, including failures recovered after USB disconnect.
typedef struct {
    uint32_t attempts, completed, failures, last_ms, max_ms, phase_ms;
    int last_rc, failure_rc;
    uint8_t phase, failure_phase;
    uint32_t failure_ms;
} kuku_upload_diag_t;
void kuku_baidu_get_upload_diag(kuku_upload_diag_t *out);
// 列出应用目录中的云端文件。状态:0 未加载,1 加载中,2 就绪,-1 出错。
int  kuku_baidu_list_request(int page);
int  kuku_baidu_list_enter(const kuku_cloud_file_t *folder);
int  kuku_baidu_list_up(void); // 0=requested, 1=already at root, negative=busy/error
bool kuku_baidu_list_is_root(void);
void kuku_baidu_list_dir(char *out, size_t cap);
void kuku_baidu_list_status(int *page, int *count, int *status, bool *has_more);
bool kuku_baidu_list_get(int idx, kuku_cloud_file_t *out);
bool kuku_baidu_is_jpeg(const char *name);
bool kuku_baidu_jpeg_baseline(const char *path);
int  kuku_baidu_image_request(const kuku_cloud_file_t *file);
int  kuku_baidu_avatar_save(void);
int  kuku_baidu_image_status(void); // 1=loading,2=ready,3=saving,4=saved,<0=error
int  kuku_image_render_bmp(const char *jpeg_path, const char *bmp_path, int edge);

#define KUKU_IMAGE_PREVIEW_PATH "/rec/kimg.jpg"
#define KUKU_IMAGE_PREVIEW_EDGE 232
#define KUKU_IMAGE_PREVIEW_BMP_PATH "/rec/kimg.bmp"
#define KUKU_IMAGE_AVATAR_PATH  "/rec/kava.bmp"
#define KUKU_IMAGE_LEGACY_AVATAR_PATH "/rec/kava.jpg"
#define KUKU_IMAGE_PREVIEW_LV   "S:/rec/kimg.bmp"
#define KUKU_IMAGE_AVATAR_LV    "S:/rec/kava.bmp"

// 录音与存储 ----------------------------------------------------------------
// 挂载 /rec 分区并恢复 NVS 序号;失败返回错误(应用仍可跑,无录音)。
int kuku_rec_init(void);
int kuku_rec_start(void);                            // 幂等:已在录返回 0
int kuku_rec_stop(void);
int kuku_play_start(const char *name);               // 回放,录音中拒绝
int kuku_play_stop(void);
int kuku_rec_count(void);                            // 现有录音文件数
long kuku_rec_free_kb(void);                         // 剩余空间 KB,-1 未知
bool kuku_rec_name_copy(int idx, char *out, size_t cap);
int kuku_rec_snapshot(char (*names)[KUKU_MAX_NAME], int cap);
int kuku_rec_mark_uploaded(const char *name);
uint32_t kuku_rec_file_ms(const char *name);         // 由文件大小推算时长
bool kuku_rec_current_name(char *out, size_t cap);   // 正在写的文件名
bool kuku_rec_preview_name(char *out, size_t cap);   // 下一文件名预览(不消耗序号)
int kuku_rec_clean_synced(void);                     // 删除已同步的 .UPD 文件并返回数量
void kuku_rec_refresh(void);                         // 外部改名后刷新录音目录缓存

// 串口测试钩子(自动化验收用) --------------------------------------------------
void kuku_test_start(void);
int kuku_test_key(int button, bool long_press); // 0 UP, 1 OK, 2 DOWN, 3 CHORD
int kuku_test_page(void);
