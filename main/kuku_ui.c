// main/kuku_ui.c —— Netdisk Recording Badge 全部界面(自有设计,不复用基线 demo 壳)。
//
// v0.3:版面与菜单交互向 LEO RADIO(玩法65 城市电台)看齐:
//   - 配色换成 LEO 的深蓝黑 + 琥珀语言
//   - 菜单用整行圆角块,选中项琥珀底+深色字;移动只换色不整页重建
//   - 修复 v0.2 bug:menu_move 也走 kuku_ui_goto(),而 goto 无条件把选中
//     序号清零 → 上下键按了光标永远回第一项,看起来"无效"
//   - 文件页改 6 行滚动窗口(跟随选中),支持上/下长按快进(main.c 派发)
#include "kuku_app.h"
#include "kuku_baidu_auth_link.h"

#include "bsp_battery.h"
#include "bsp_display.h"
#include "lvgl.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "stdio.h"
#include "string.h"
#include "time.h"

LV_FONT_DECLARE(kuku_font_14);
LV_FONT_DECLARE(kuku_font_20);
LV_FONT_DECLARE(kuku_font_28);
#include "../assets/fonts/kuku_font_inventory.h"

// ---- LEO RADIO 配色 ---------------------------------------------------------
#define COLOR_BG      0x071017   // 深蓝黑背景
#define COLOR_PANEL   0x0D1B23   // 面板
#define COLOR_PANEL2  0x11262E   // 浅一档面板
#define COLOR_GRID    0x24404A   // 分隔线
#define COLOR_TEXT    0xF4F0DE   // 主文字(暖白)
#define COLOR_DIM     0x849BA0   // 次要文字
#define COLOR_ACCENT  0xFFB74D   // 琥珀主色
#define COLOR_ACCENT2 0x7F5A29   // 琥珀暗色(选中行底)
#define COLOR_GREEN   0x4ED39A   // 就绪/成功
#define COLOR_DANGER  0xFF5D62   // 录音/低电

static kuku_page_t s_page = KUKU_PAGE_HOME;
static lv_obj_t *s_scr;

// 每页动态标签(UI 定时器刷新)。
static lv_obj_t *s_batt;          // 所有页右上角电量(≤15% 变红)
static lv_obj_t *s_wifi_state_icon;
static lv_obj_t *s_cloud_state_icon;
static lv_obj_t *s_rec_icon;      // Quiet ring; center lights during recording.
static lv_obj_t *s_rec_icon_dot;
static lv_obj_t *s_home_status;   // 工牌页底部临时提示
static lv_obj_t *s_image_hint;
static lv_obj_t *s_wifi_label;    // Wi-Fi 页内容
static lv_obj_t *s_bd_label;      // 百度页内容
static lv_obj_t *s_bd_code;
static lv_obj_t *s_bd_detail;
static lv_obj_t *s_bd_status;
static lv_obj_t *s_bd_qr;
static lv_obj_t *s_bd_qr_box;
static char s_bd_qr_link[96];

static int s_menu_sel;
static int s_guide_page;
static lv_obj_t *s_guide_text;
static lv_obj_t *s_guide_count;
static lv_obj_t *s_guide_section;
static lv_obj_t *s_guide_nav;
static int s_files_sel;
static int s_saved_sel;
static bool s_files_list_mode;
static char s_share_name[KUKU_CLOUD_NAME_MAX];
static char s_image_name[KUKU_CLOUD_NAME_MAX];
static lv_obj_t *s_image_panel;
static lv_obj_t *s_image_state;
static lv_obj_t *s_image_saved_label;
static bool s_image_shown;
static uint32_t s_list_next_request_ms;

// 主页 toast:停止录音后的 SAVED/REC FAIL 等提示保持 2 秒再回 READY。
static char s_toast[40];
static uint32_t s_toast_until_ms;

// ---- LEO 风格基础件 ---------------------------------------------------------
static lv_obj_t *make_box(lv_obj_t *parent, int x, int y, int w, int h,
                          uint32_t color, int radius) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    return o;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, int x, int y,
                            int w, uint32_t color, const lv_font_t *font) {
    lv_obj_t *o = lv_label_create(parent);
    lv_label_set_text(o, text ? text : "");
    lv_obj_set_pos(o, x, y);
    lv_obj_set_width(o, w);
    lv_obj_set_style_text_font(o, font, 0);
    lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
    lv_label_set_long_mode(o, LV_LABEL_LONG_DOT);
    return o;
}

static void center(lv_obj_t *o) { lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0); }
static void right(lv_obj_t *o) { lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_RIGHT, 0); }

static void style_screen(lv_obj_t *scr) {
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_BG), 0);
}

static void make_status_bar(void) {
    // Fixed top-bar slots: Wi-Fi 132, cloud 151, record 171, battery 188.
    // 宽 40:"100%" 在 33px 里会被 DOT 换行成两行(实机截图确认过)。
    s_wifi_state_icon = make_label(s_scr, "网", 132, 11, 16,
                                   COLOR_DIM, &kuku_font_14);
    center(s_wifi_state_icon);
    s_cloud_state_icon = make_label(s_scr, "云", 151, 11, 16,
                                    COLOR_DIM, &kuku_font_14);
    center(s_cloud_state_icon);
    s_batt = make_label(s_scr, "--%", 188, 11, 40, COLOR_DIM, &kuku_font_14);
    right(s_batt);
    s_rec_icon = make_box(s_scr, 171, 12, 14, 14, COLOR_PANEL2, 7);
    lv_obj_set_style_border_color(s_rec_icon, lv_color_hex(COLOR_GRID), 0);
    lv_obj_set_style_border_width(s_rec_icon, 1, 0);
    s_rec_icon_dot = make_box(s_rec_icon, 4, 4, 6, 6, 0x9C6360, 3);
    lv_obj_add_flag(s_rec_icon_dot, LV_OBJ_FLAG_HIDDEN);
}

static void status_bar_refresh(void) {
    int soc = bsp_battery_soc();
    if (s_batt) {
        if (soc >= 0) {
            lv_label_set_text_fmt(s_batt, "%d%%", soc);
            lv_obj_set_style_text_color(s_batt,
                lv_color_hex(soc <= 15 ? COLOR_DANGER : COLOR_DIM), 0);
        } else {
            lv_label_set_text(s_batt, "--");
        }
    }
    if (s_wifi_state_icon)
        lv_obj_set_style_text_color(s_wifi_state_icon,
            lv_color_hex(g_kuku.wifi_up ? COLOR_GREEN : COLOR_DANGER), 0);
    if (s_cloud_state_icon) {
        uint32_t color = COLOR_DIM;
        if (g_kuku.bd_state == 1 || g_kuku.bd_state == 3) color = COLOR_ACCENT;
        else if (g_kuku.bd_state == 2)
            color = g_kuku.wifi_up ? COLOR_GREEN : COLOR_DANGER;
        lv_obj_set_style_text_color(s_cloud_state_icon, lv_color_hex(color), 0);
    }
    if (s_rec_icon_dot) {
        if (g_kuku.recording) lv_obj_remove_flag(s_rec_icon_dot, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_rec_icon_dot, LV_OBJ_FLAG_HIDDEN);
    }
}

// 带边框的内容面板(LEO 配网页同款)。
static lv_obj_t *make_panel(int x, int y, int w, int h) {
    lv_obj_t *p = make_box(s_scr, x, y, w, h, COLOR_PANEL, 10);
    lv_obj_set_style_border_color(p, lv_color_hex(COLOR_GRID), 0);
    lv_obj_set_style_border_width(p, 1, 0);
    return p;
}

static lv_obj_t *make_hint(const char *text) {
    lv_obj_t *h = make_label(s_scr, text, 12, 283, 216, COLOR_DIM, &kuku_font_14);
    center(h);
    return h;
}

static void make_title(const char *text) {
    make_label(s_scr, text, 12, 10, 102, COLOR_TEXT, &kuku_font_20);
    make_status_bar();
}

// ---- 主页 ------------------------------------------------------------------
static void home_build(void) {
    make_label(s_scr, "录音工牌", 12, 10, 100, COLOR_TEXT, &kuku_font_20);
    make_status_bar();
    make_box(s_scr, 12, 38, 216, 1, COLOR_GRID, 0);

    // Identity-card silhouette with no personal fields to maintain.
    // The passive NFC tag is independent of this screen and its artwork.
    lv_obj_t *card = make_box(s_scr, 13, 51, 214, 211, 0xF6F3EA, 12);
    make_box(card, 0, 0, 214, 48, 0x17465B, 12);
    make_box(card, 0, 37, 214, 11, 0x17465B, 0);
    make_label(card, "网盘录音工牌", 14, 8, 155, 0xFFFFFF, &kuku_font_20);
    make_label(card, "连续录音 · 自动上传", 14, 32, 160, 0xDCE9E8, &kuku_font_14);

    lv_obj_t *avatar = make_box(card, 75, 56, 64, 64, 0xD8E5E2, 32);
    lv_image_header_t avatar_info;
    FILE *avatar_file = fopen(KUKU_IMAGE_AVATAR_PATH, "rb");
    bool have_avatar = avatar_file != NULL;
    if (avatar_file) fclose(avatar_file);
    if (have_avatar) lv_image_cache_drop(KUKU_IMAGE_AVATAR_LV);
    if (have_avatar && lv_image_decoder_get_info(KUKU_IMAGE_AVATAR_LV, &avatar_info) == LV_RESULT_OK) {
        ESP_LOGI("kuku_img", "avatar decoder ready %ux%u",
                 (unsigned)avatar_info.w, (unsigned)avatar_info.h);
        lv_obj_t *photo = lv_image_create(avatar);
        lv_image_set_src(photo, KUKU_IMAGE_AVATAR_LV);
        lv_obj_set_size(photo, 64, 64);
        lv_obj_set_pos(photo, 0, 0);
        lv_image_set_inner_align(photo, LV_IMAGE_ALIGN_CENTER);
    } else {
        lv_obj_t *initial = make_label(avatar, "REC", 0, 16, 64,
                                        0x17465B, &kuku_font_28);
        center(initial);
    }
    lv_obj_t *name = make_label(card, "随身记录", 15, 124, 184,
                                 0x17333D, &kuku_font_28);
    center(name);
    lv_obj_t *role = make_label(card, "NETDISK RECORDING", 15, 157, 184,
                                 0x4A6770, &kuku_font_14);
    center(role);
    make_box(card, 15, 182, 184, 1, 0xC5D3D1, 0);
    make_label(card, "随身照片", 15, 187, 70, 0x4A6770, &kuku_font_14);
    make_label(card, "云端归档", 137, 187, 62, 0x4A6770, &kuku_font_14);

    s_home_status = make_label(s_scr, "", 12, 270, 216,
                                COLOR_ACCENT, &kuku_font_14);
    center(s_home_status);
    lv_obj_t *h = make_label(s_scr, "OK：功能菜单", 12, 296, 216,
                             COLOR_DIM, &kuku_font_14);
    center(h);
}

// ---- 菜单页(LEO 设置页样式:整行圆角块,选中琥珀底深色字) -------------------
static const char *const MENU_ITEMS[] = {
    "网盘文件", "无线网络", "操作指南", "重置网盘连接"
};
#define MENU_N 4
static lv_obj_t *s_menu_row[MENU_N];
static lv_obj_t *s_menu_lbl[MENU_N];

static void menu_apply_sel(void) {
    for (int i = 0; i < MENU_N; i++) {
        if (!s_menu_row[i] || !s_menu_lbl[i]) continue;
        bool active = i == s_menu_sel;
        lv_obj_set_style_bg_color(s_menu_row[i],
                                  lv_color_hex(active ? COLOR_ACCENT : COLOR_PANEL), 0);
        lv_obj_set_style_text_color(s_menu_lbl[i],
                                    lv_color_hex(active ? COLOR_BG : COLOR_TEXT), 0);
    }
}

static void menu_build(void) {
    make_title("菜单");
    for (int i = 0; i < MENU_N; i++) {
        s_menu_row[i] = make_box(s_scr, 24, 45 + i * 38, 192, 32, COLOR_PANEL, 7);
        s_menu_lbl[i] = make_label(s_menu_row[i], MENU_ITEMS[i], 0, 1, 192,
                                   COLOR_TEXT, &kuku_font_20);
        center(s_menu_lbl[i]);
    }
    menu_apply_sel();
    make_hint("OK 选择 / 长按 OK 返回");
}

// ---- 本地待传 + 云端目录(云端每页 10 项,服务器按修改时间倒序) ------------
#define FILES_ROWS 6
static lv_obj_t *s_files_row[FILES_ROWS];
static lv_obj_t *s_files_name[FILES_ROWS];
static lv_obj_t *s_files_info[FILES_ROWS];
static lv_obj_t *s_files_state;

static bool pending_at(int idx, char *out, size_t cap) {
    int found = 0;
    for (int i = kuku_rec_count() - 1; i >= 0; i--) {
        char name[KUKU_MAX_NAME];
        if (!kuku_rec_name_copy(i, name, sizeof(name))) continue;
        size_t len = strlen(name);
        if (len < 4 || strcmp(name + len - 4, ".WAV") != 0) continue;
        if (found++ == idx) {
            if (out && cap) strlcpy(out, name, cap);
            return true;
        }
    }
    return false;
}

static int pending_count(void) {
    if (!kuku_baidu_list_is_root()) return 0;
    int count = 0;
    while (pending_at(count, NULL, 0)) count++;
    return count;
}

static int files_total(void) {
    int page = 0, count = 0, status = 0;
    bool more = false;
    kuku_baidu_list_status(&page, &count, &status, &more);
    return pending_count() + (page > 0 ? 1 : 0) + count +
           (status == 2 && more ? 1 : 0);
}

static void files_build(void) {
    char dir[KUKU_CLOUD_PATH_MAX];
    kuku_baidu_list_dir(dir, sizeof(dir));
    const char *title = kuku_baidu_list_is_root() ? "网盘文件" : strrchr(dir, '/') + 1;
    lv_obj_t *t = make_label(s_scr, title, 12, 12, 120, COLOR_TEXT,
                             &kuku_font_20);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    make_status_bar();
    s_files_state = make_label(s_scr, "", 12, 40, 216, COLOR_DIM,
                               &kuku_font_14);
    center(s_files_state);
    for (int r = 0; r < FILES_ROWS; r++) {
        s_files_row[r] = make_box(s_scr, 12, 64 + r * 34, 216, 29, COLOR_PANEL, 5);
        s_files_name[r] = make_label(s_files_row[r], "", 9, 6, 148, COLOR_DIM,
                                     &kuku_font_14);
        s_files_info[r] = make_label(s_files_row[r], "", 162, 7, 46, COLOR_DIM,
                                     &kuku_font_14);
        right(s_files_info[r]);
    }
    make_box(s_scr, 12, 277, 216, 1, COLOR_GRID, 0);
    make_hint("OK 操作 / 长按 OK 返回");
}

void kuku_ui_files_refresh(void) {
    if (s_page != KUKU_PAGE_FILES || !s_files_list_mode || !s_files_state) return;
    int page = 0, cloud_count = 0, status = 0;
    bool more = false;
    kuku_baidu_list_status(&page, &cloud_count, &status, &more);
    int local_count = pending_count();
    int total = files_total();
    if (s_files_sel >= total) s_files_sel = 0;

    if (total <= 0) {
        lv_label_set_text(s_files_state, status == 1 ? "正在读取网盘…" :
                          status < 0 ? "网盘读取失败，稍后重试" :
                          !g_kuku.wifi_up ? "无线网络未连接" : "网盘目录暂无文件");
        lv_obj_set_style_text_color(s_files_state, lv_color_hex(COLOR_ACCENT), 0);
        for (int r = 0; r < FILES_ROWS; r++)
            lv_obj_add_flag(s_files_row[r], LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_set_style_text_color(s_files_state, lv_color_hex(COLOR_DIM), 0);
    lv_label_set_text_fmt(s_files_state, "第 %d 页  %d / %d  %s",
                          page + 1, s_files_sel + 1, total,
                          status == 1 ? "读取中" : "上下选择");

    // 窗口跟随:选中项尽量停在窗口第 3 行(LEO wifi list 同款算法)。
    int first = total <= FILES_ROWS
                  ? 0
                  : (s_files_sel > 2 ? s_files_sel - 2 : 0);
    if (total > FILES_ROWS && first > total - FILES_ROWS) first = total - FILES_ROWS;

    for (int r = 0; r < FILES_ROWS; r++) {
        int idx = first + r;
        if (idx >= total) {
            lv_obj_add_flag(s_files_row[r], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s_files_row[r], LV_OBJ_FLAG_HIDDEN);
        bool active = idx == s_files_sel;
        lv_obj_set_style_bg_color(s_files_row[r],
                                  lv_color_hex(active ? COLOR_ACCENT2 : COLOR_PANEL), 0);
        if (idx < local_count) {
            char name[KUKU_MAX_NAME];
            pending_at(idx, name, sizeof(name));
            lv_label_set_text(s_files_name[r], name);
            char up_name[KUKU_MAX_NAME] = {0};
            int done = 0, total_up = 0;
            kuku_baidu_get_progress(up_name, sizeof(up_name), &done, &total_up);
            lv_label_set_text(s_files_info[r],
                              g_kuku.bd_state == 3 && strcmp(name, up_name) == 0
                                  ? "传输中" : "待传");
        } else if (page > 0 && idx == local_count) {
            lv_label_set_text(s_files_name[r], "上一页");
            lv_label_set_text(s_files_info[r], "");
        } else if (status == 2 && more && idx == total - 1) {
            lv_label_set_text(s_files_name[r], "下一页");
            lv_label_set_text(s_files_info[r], "");
        } else {
            int cloud_idx = idx - local_count - (page > 0 ? 1 : 0);
            kuku_cloud_file_t file = {0};
            if (kuku_baidu_list_get(cloud_idx, &file)) {
                lv_label_set_text(s_files_name[r], file.name);
                if (file.is_dir) lv_label_set_text(s_files_info[r], "目录");
                else if (file.mtime) {
                    time_t stamp = file.mtime;
                    struct tm tm;
                    localtime_r(&stamp, &tm);
                    lv_label_set_text_fmt(s_files_info[r], "%02d-%02d",
                                          tm.tm_mon + 1, tm.tm_mday);
                } else lv_label_set_text(s_files_info[r], "云端");
            } else {
                lv_label_set_text(s_files_name[r], "");
                lv_label_set_text(s_files_info[r], "");
            }
        }
        lv_obj_set_style_text_color(s_files_name[r],
                                    lv_color_hex(active ? COLOR_TEXT : COLOR_DIM), 0);
        lv_obj_set_style_text_color(s_files_info[r],
                                    lv_color_hex(active ? COLOR_ACCENT : COLOR_DIM), 0);
    }
}

int kuku_ui_files_selected(kuku_cloud_file_t *cloud, char *local, size_t cap) {
    int page = 0, count = 0, status = 0;
    bool more = false;
    kuku_baidu_list_status(&page, &count, &status, &more);
    int local_count = pending_count();
    if (s_files_sel < local_count)
        return pending_at(s_files_sel, local, cap) ? 1 : 0;
    int idx = s_files_sel - local_count;
    if (page > 0) {
        if (idx == 0) return 3;
        idx--;
    }
    if (idx < count) return kuku_baidu_list_get(idx, cloud) ? 2 : 0;
    return status == 2 && more && idx == count ? 4 : 0;
}

void kuku_ui_files_reset_sel(void) { s_files_sel = 0; }

void kuku_ui_share_open(const kuku_cloud_file_t *file) {
    strlcpy(s_share_name, file ? file->name : "", sizeof(s_share_name));
}

void kuku_ui_image_open(const kuku_cloud_file_t *file) {
    strlcpy(s_image_name, file ? file->name : "", sizeof(s_image_name));
}

static void image_build(void) {
    make_title("查看图片");
    s_image_panel = make_panel(2, 41, 236, 236);
    s_image_state = make_label(s_image_panel, "正在读取图片…", 10, 102, 216,
                               COLOR_ACCENT, &kuku_font_14);
    center(s_image_state);
    s_image_saved_label = make_label(s_scr, "已设为工牌照片", 12, 283,
                                      216, COLOR_GREEN, &kuku_font_14);
    center(s_image_saved_label);
    lv_obj_add_flag(s_image_saved_label, LV_OBJ_FLAG_HIDDEN);
    s_image_shown = false;
    s_image_hint = make_hint("OK 设为照片 / 长按 OK 返回");
    kuku_ui_image_refresh();
}

void kuku_ui_image_refresh(void) {
    if (s_page != KUKU_PAGE_IMAGE || !s_image_state) return;
    int status = kuku_baidu_image_status();
    if (status == 2 || status == 4) {
        if (!s_image_shown) {
            lv_image_cache_drop(KUKU_IMAGE_PREVIEW_LV);
            lv_image_header_t info;
            if (lv_image_decoder_get_info(KUKU_IMAGE_PREVIEW_LV, &info) != LV_RESULT_OK) {
                lv_label_set_text(s_image_state, "图片格式不支持");
                return;
            }
            ESP_LOGI("kuku_img", "preview decoder ready %ux%u",
                     (unsigned)info.w, (unsigned)info.h);
            lv_obj_t *image = lv_image_create(s_image_panel);
            lv_image_set_src(image, KUKU_IMAGE_PREVIEW_LV);
            lv_obj_set_pos(image, 2, 2);
            lv_obj_set_size(image, KUKU_IMAGE_PREVIEW_EDGE, KUKU_IMAGE_PREVIEW_EDGE);
            lv_image_set_inner_align(image, LV_IMAGE_ALIGN_CENTER);
            lv_image_set_antialias(image, false);
            s_image_shown = true;
        }
        lv_obj_add_flag(s_image_state, LV_OBJ_FLAG_HIDDEN);
        if (status == 4) {
            lv_obj_add_flag(s_image_hint, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_image_saved_label, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        lv_obj_remove_flag(s_image_state, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_image_state,
            status == 1 ? "正在读取图片…" :
            status == 3 ? "正在保存照片…" :
            status < 0 ? "图片读取或保存失败" : "等待图片");
    }
}

static void share_build(void) {
    make_title("分享文件");
    lv_obj_t *p = make_panel(12, 43, 216, 240);
    lv_obj_t *name = make_label(p, s_share_name, 12, 8, 192,
                                COLOR_TEXT, &kuku_font_14);
    center(name);
    lv_obj_t *box = make_box(p, 34, 31, 148, 148, 0xFFFFFF, 0);
    lv_obj_t *qr = lv_qrcode_create(box);
    lv_qrcode_set_size(qr, 116);
    lv_qrcode_set_dark_color(qr, lv_color_hex(0x000000));
    lv_qrcode_set_light_color(qr, lv_color_hex(0xFFFFFF));
    lv_qrcode_set_quiet_zone(qr, false);
    lv_obj_set_pos(qr, 16, 16);
    // Reserved documentation URL: this demonstration code never grants file access.
    static const char demo_url[] = "https://example.com/kuku-share-demo";
    lv_qrcode_update(qr, demo_url, sizeof(demo_url) - 1);
    lv_obj_t *demo = make_label(p, "示例二维码", 12, 183, 192,
                                 COLOR_ACCENT, &kuku_font_14);
    center(demo);
    lv_obj_t *note = make_label(p, "分享需认证开通\n企业批量应用请联系项目作者", 12, 204,
                                 192, COLOR_DIM, &kuku_font_14);
    center(note);
    make_hint("长按 OK 返回文件列表");
}

// ---- Wi-Fi 页(纯状态显示,OK=进扫描) ----------------------------------------
static void wifi_build(void) {
    make_title("无线网络");
    lv_obj_t *p = make_panel(18, 50, 204, 150);
    s_wifi_label = make_label(p, "", 12, 16, 180, COLOR_TEXT, &kuku_font_14);
    center(s_wifi_label);
    make_hint("OK 扫描 / 下键已存 / 长按OK返回");
    kuku_ui_wifi_refresh();
}

void kuku_ui_wifi_refresh(void) {
    if (s_page != KUKU_PAGE_WIFI || !s_wifi_label) return;
    if (g_kuku.wifi_up) {
        char ssid[34], ip[16];
        kuku_wifi_get_ssid(ssid, sizeof(ssid));
        kuku_wifi_get_ip(ip, sizeof(ip));
        lv_label_set_text_fmt(s_wifi_label, "已连接：%s\nIP: %s\n信号：%d\n\n已存网络：%d",
                              ssid, ip, kuku_wifi_get_rssi(),
                              kuku_wifi_saved_count());
    } else {
        lv_label_set_text_fmt(s_wifi_label,
            "未连接网络\n已存网络：%d\n\n按 OK 扫描附近网络",
            kuku_wifi_saved_count());
    }
}

// ---- 已存网络:最多 10 条,确认页避免误删 -------------------------------
#define SAVED_ROWS 6
static lv_obj_t *s_saved_row[SAVED_ROWS];
static lv_obj_t *s_saved_name[SAVED_ROWS];
static lv_obj_t *s_saved_state;

static void saved_build(void) {
    make_title("已存网络");
    s_saved_state = make_label(s_scr, "", 12, 40, 216, COLOR_DIM, &kuku_font_14);
    center(s_saved_state);
    for (int r = 0; r < SAVED_ROWS; r++) {
        s_saved_row[r] = make_box(s_scr, 12, 64 + r * 34, 216, 29, COLOR_PANEL, 5);
        s_saved_name[r] = make_label(s_saved_row[r], "", 9, 6, 198, COLOR_DIM,
                                    &kuku_font_14);
    }
    make_box(s_scr, 12, 277, 216, 1, COLOR_GRID, 0);
    make_hint("OK 选择删除 / 长按OK返回");
}

void kuku_ui_saved_refresh(void) {
    if (s_page != KUKU_PAGE_WIFI_SAVED || !s_saved_state) return;
    int total = kuku_wifi_saved_count();
    if (s_saved_sel >= total) s_saved_sel = total > 0 ? total - 1 : 0;
    if (total == 0) {
        lv_label_set_text(s_saved_state, "暂无已存网络");
        for (int r = 0; r < SAVED_ROWS; r++)
            lv_obj_add_flag(s_saved_row[r], LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text_fmt(s_saved_state, "%d / %d   上下键选择",
                          s_saved_sel + 1, total);
    int first = total <= SAVED_ROWS ? 0 : (s_saved_sel > 2 ? s_saved_sel - 2 : 0);
    if (total > SAVED_ROWS && first > total - SAVED_ROWS)
        first = total - SAVED_ROWS;
    for (int r = 0; r < SAVED_ROWS; r++) {
        int idx = first + r;
        if (idx >= total) {
            lv_obj_add_flag(s_saved_row[r], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s_saved_row[r], LV_OBJ_FLAG_HIDDEN);
        bool active = idx == s_saved_sel;
        lv_obj_set_style_bg_color(s_saved_row[r],
                                  lv_color_hex(active ? COLOR_ACCENT2 : COLOR_PANEL), 0);
        char ssid[33] = {0};
        kuku_wifi_saved_get(idx, ssid, sizeof(ssid));
        lv_label_set_text(s_saved_name[r], ssid);
        lv_obj_set_style_text_color(s_saved_name[r],
                                    lv_color_hex(active ? COLOR_TEXT : COLOR_DIM), 0);
    }
}

void kuku_ui_saved_move(int delta) {
    int total = kuku_wifi_saved_count();
    if (total <= 0) return;
    s_saved_sel = ((s_saved_sel + delta) % total + total) % total;
    kuku_ui_saved_refresh();
}

int kuku_ui_saved_sel(void) { return s_saved_sel; }

static void saved_delete_build(void) {
    make_title("删除网络");
    lv_obj_t *p = make_panel(18, 67, 204, 145);
    char ssid[33] = {0};
    kuku_wifi_saved_get(s_saved_sel, ssid, sizeof(ssid));
    lv_obj_t *t = make_label(p, "", 12, 20, 180, COLOR_TEXT, &kuku_font_20);
    lv_label_set_text_fmt(t, "删除这条网络记忆？\n\n%s", ssid);
    center(t);
    make_hint("OK 删除 / 长按 OK 取消");
}

// ---- 屏上配网:扫描列表(6 行滚动窗口,LEO wifi list 同款) --------------------
#define SCAN_ROWS 6
static lv_obj_t *s_scan_row[SCAN_ROWS];
static lv_obj_t *s_scan_name[SCAN_ROWS];
static lv_obj_t *s_scan_info[SCAN_ROWS];
static lv_obj_t *s_scan_state;
static int s_scan_sel;

static void scan_build(void) {
    lv_obj_t *t = make_label(s_scr, "附近网络", 12, 12, 120, COLOR_TEXT,
                             &kuku_font_20);
    (void)t;
    make_status_bar();
    s_scan_state = make_label(s_scr, "正在扫描…", 12, 40, 216, COLOR_ACCENT,
                              &kuku_font_14);
    center(s_scan_state);
    for (int r = 0; r < SCAN_ROWS; r++) {
        s_scan_row[r] = make_box(s_scr, 12, 64 + r * 34, 216, 29, COLOR_PANEL, 5);
        s_scan_name[r] = make_label(s_scan_row[r], "", 9, 6, 148, COLOR_DIM,
                                    &kuku_font_14);
        s_scan_info[r] = make_label(s_scan_row[r], "", 162, 7, 46, COLOR_DIM,
                                    &kuku_font_14);
        right(s_scan_info[r]);
    }
    make_box(s_scr, 12, 277, 216, 1, COLOR_GRID, 0);
    make_hint("OK 输入密码 / 长按OK返回");
}

static void scan_refresh(void) {
    if (s_page != KUKU_PAGE_WIFI_SCAN || !s_scan_state) return;
    int total = kuku_wifi_scan_count();
    if (!kuku_wifi_scan_done()) {
        lv_label_set_text(s_scan_state, "正在扫描…");
        lv_obj_set_style_text_color(s_scan_state, lv_color_hex(COLOR_ACCENT), 0);
    } else if (total <= 0) {
        lv_label_set_text(s_scan_state, "未发现网络");
        lv_obj_set_style_text_color(s_scan_state, lv_color_hex(COLOR_DANGER), 0);
    } else {
        lv_label_set_text_fmt(s_scan_state, "%d / %d   上下键选择",
                              s_scan_sel + 1, total);
        lv_obj_set_style_text_color(s_scan_state, lv_color_hex(COLOR_DIM), 0);
    }

    if (s_scan_sel >= total) s_scan_sel = 0;
    int first = total <= SCAN_ROWS ? 0
                                   : (s_scan_sel > 2 ? s_scan_sel - 2 : 0);
    if (first > total - SCAN_ROWS) first = total - SCAN_ROWS;
    if (first < 0) first = 0;

    for (int r = 0; r < SCAN_ROWS; r++) {
        int idx = first + r;
        if (idx >= total) {
            lv_obj_add_flag(s_scan_row[r], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s_scan_row[r], LV_OBJ_FLAG_HIDDEN);
        bool active = idx == s_scan_sel;
        char ssid[33];
        int rssi = 0;
        bool secure = false;
        kuku_wifi_scan_get(idx, ssid, sizeof(ssid), &rssi, &secure);
        lv_obj_set_style_bg_color(s_scan_row[r],
                                  lv_color_hex(active ? COLOR_ACCENT2 : COLOR_PANEL), 0);
        lv_label_set_text(s_scan_name[r], ssid);
        lv_obj_set_style_text_color(s_scan_name[r],
                                    lv_color_hex(active ? COLOR_TEXT : COLOR_DIM), 0);
        lv_label_set_text_fmt(s_scan_info[r], "%s %d", secure ? "*" : "O", rssi);
        lv_obj_set_style_text_color(s_scan_info[r],
                                    lv_color_hex(active ? COLOR_ACCENT : COLOR_DIM), 0);
    }
}

// ---- 屏上配网:键盘(LEO 同款:3 页键位,每键单 label 兼底色块省 LVGL 堆) ------
#define KB_MAX_KEYS 35
#define KB_COLS 6
static const char *const KB_LOWER[] = {
    "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l",
    "m", "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x",
    "y", "z", "ABC", "123", "<", "GO",
};
static const char *const KB_UPPER[] = {
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L",
    "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X",
    "Y", "Z", "abc", "123", "<", "GO",
};
static const char *const KB_SYMBOL[] = {
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "-", "_",
    ".", "@", "#", "$", "%", "&", "*", "!", "+", "=", "?", "/",
    ":", ";", "(", ")", "[", "]", "abc", "ABC", "SP", "<", "GO",
};
static lv_obj_t *s_kb_key[KB_MAX_KEYS];
static lv_obj_t *s_kb_network;
static lv_obj_t *s_kb_mask;
static lv_obj_t *s_kb_status;
static char s_kb_ssid[33];
static char s_kb_pass[65];
static int s_kb_page;    // 0 lower / 1 upper / 2 symbols
static int s_kb_sel;

static int kb_count(int page) {
    switch (page) {
        case 0:  return (int)(sizeof(KB_LOWER) / sizeof(KB_LOWER[0]));
        case 1:  return (int)(sizeof(KB_UPPER) / sizeof(KB_UPPER[0]));
        default: return (int)(sizeof(KB_SYMBOL) / sizeof(KB_SYMBOL[0]));
    }
}

static const char *kb_key(int page, int idx) {
    int n = kb_count(page);
    if (idx < 0 || idx >= n) return "";
    switch (page) {
        case 0:  return KB_LOWER[idx];
        case 1:  return KB_UPPER[idx];
        default: return KB_SYMBOL[idx];
    }
}

static void kb_build(void) {
    make_label(s_scr, "输入密码", 12, 10, 102, COLOR_TEXT, &kuku_font_20);
    make_status_bar();
    s_kb_network = make_label(s_scr, s_kb_ssid, 12, 39, 216, COLOR_ACCENT,
                              &kuku_font_14);
    center(s_kb_network);

    lv_obj_t *pass_box = make_box(s_scr, 18, 66, 204, 34, COLOR_PANEL, 6);
    s_kb_mask = make_label(pass_box, "请输入密码", 8, 8, 188, COLOR_DIM,
                           &kuku_font_14);
    center(s_kb_mask);

    // 每键单个 label 兼作底色块(LEO 教训:box+label 34 键会爆 24KB LVGL 堆)。
    for (int i = 0; i < KB_MAX_KEYS; i++) {
        int x = 15 + (i % KB_COLS) * 35;
        int y = 109 + (i / KB_COLS) * 28;
        lv_obj_t *k = make_label(s_scr, "", x, y, 31, COLOR_TEXT,
                                 &kuku_font_14);
        lv_obj_set_size(k, 31, 24);
        lv_obj_set_style_bg_color(k, lv_color_hex(COLOR_PANEL2), 0);
        lv_obj_set_style_bg_opa(k, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(k, 4, 0);
        lv_obj_set_style_pad_top(k, 1, 0);
        center(k);
        s_kb_key[i] = k;
    }
    make_box(s_scr, 12, 281, 216, 1, COLOR_GRID, 0);
    s_kb_status = make_label(s_scr, "空密码选<返回 / 长OK连接", 12, 287, 216,
                             COLOR_DIM, &kuku_font_14);
    center(s_kb_status);
}

static void kb_refresh(void) {
    if (s_page != KUKU_PAGE_WIFI_KB || !s_kb_network) return;
    lv_label_set_text(s_kb_network, s_kb_ssid);

    char mask[70];
    size_t len = strlen(s_kb_pass);
    size_t stars = len > 38 ? 38 : len;
    memset(mask, '*', stars);
    mask[stars] = '\0';
    lv_label_set_text(s_kb_mask, len ? mask : "请输入密码");
    lv_obj_set_style_text_color(s_kb_mask,
                                lv_color_hex(len ? COLOR_TEXT : COLOR_DIM), 0);

    int n = kb_count(s_kb_page);
    for (int i = 0; i < KB_MAX_KEYS; i++) {
        if (!s_kb_key[i]) continue;
        if (i >= n) {
            lv_obj_add_flag(s_kb_key[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s_kb_key[i], LV_OBJ_FLAG_HIDDEN);
        bool active = i == s_kb_sel;
        lv_obj_set_style_bg_color(s_kb_key[i],
                                  lv_color_hex(active ? COLOR_ACCENT : COLOR_PANEL2), 0);
        const char *key = kb_key(s_kb_page, i);
        lv_label_set_text(s_kb_key[i], strcmp(key, "GO") == 0 ? "连接" :
                          strcmp(key, "SP") == 0 ? "空格" : key);
        lv_obj_set_style_text_color(s_kb_key[i],
                                    lv_color_hex(active ? COLOR_BG : COLOR_TEXT), 0);
    }
}

// ---- 百度网盘页 ----------------------------------------------------------------
static void baidu_build(void) {
    make_title("网盘文件");
    lv_obj_t *p = make_panel(12, 48, 216, 226);
    s_bd_label = make_label(p, "", 12, 12, 192, COLOR_TEXT, &kuku_font_28);
    s_bd_code = make_label(p, "", 12, 52, 192, COLOR_ACCENT, &kuku_font_28);
    s_bd_detail = make_label(p, "", 12, 98, 192, COLOR_TEXT, &kuku_font_20);
    s_bd_status = make_label(p, "", 12, 192, 192, COLOR_DIM, &kuku_font_20);
    // 132px canvas plus 16px white border: four quiet modules per side for
    // the usual version-4, four-pixels-per-module authorization QR code.
    s_bd_qr_box = make_box(p, 26, 32, 164, 164, 0xFFFFFF, 0);
    s_bd_qr = lv_qrcode_create(s_bd_qr_box);
    lv_qrcode_set_size(s_bd_qr, 132);
    lv_qrcode_set_dark_color(s_bd_qr, lv_color_hex(0x000000));
    lv_qrcode_set_light_color(s_bd_qr, lv_color_hex(0xFFFFFF));
    lv_qrcode_set_quiet_zone(s_bd_qr, false);
    lv_obj_set_pos(s_bd_qr, 16, 16);
    lv_obj_add_flag(s_bd_qr_box, LV_OBJ_FLAG_HIDDEN);
    s_bd_qr_link[0] = 0;
    center(s_bd_label);
    center(s_bd_code);
    center(s_bd_detail);
    center(s_bd_status);
    make_hint("扫码授权 / 长按 OK 返回");
    kuku_ui_baidu_refresh();
}

void kuku_ui_baidu_refresh(void) {
    if (s_page != KUKU_PAGE_FILES || s_files_list_mode || !s_bd_label) return;
    lv_label_set_text(s_bd_code, "");
    lv_label_set_text(s_bd_detail, "");
    lv_label_set_text(s_bd_status, "");
    lv_obj_add_flag(s_bd_qr_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_bd_label, 12, 12);
    lv_obj_set_style_text_font(s_bd_label, &kuku_font_28, 0);
    lv_obj_set_pos(s_bd_code, 12, 52);
    lv_obj_set_style_text_font(s_bd_code, &kuku_font_28, 0);
    switch (g_kuku.bd_state) {
        case 1: {   // WAIT_CODE
            char url[64], code[16];
            kuku_baidu_get_auth(url, sizeof(url), code, sizeof(code));
            char qr_link[96];
            if (kuku_baidu_auth_link(code, qr_link, sizeof(qr_link))) {
                bool ready = !strcmp(qr_link, s_bd_qr_link);
                if (!ready && lv_qrcode_update(s_bd_qr, qr_link, strlen(qr_link)) == LV_RESULT_OK) {
                    strlcpy(s_bd_qr_link, qr_link, sizeof(s_bd_qr_link));
                    ready = true;
                }
                if (ready) {
                    lv_obj_set_pos(s_bd_label, 12, 3);
                    lv_obj_set_style_text_font(s_bd_label, &kuku_font_20, 0);
                    lv_label_set_text(s_bd_label, "扫码授权");
                    lv_obj_set_pos(s_bd_code, 12, 198);
                    lv_obj_set_style_text_font(s_bd_code, &kuku_font_20, 0);
                    lv_label_set_text_fmt(s_bd_code, "授权码 %s", code);
                    lv_obj_remove_flag(s_bd_qr_box, LV_OBJ_FLAG_HIDDEN);
                    break;
                }
            }
            // Omit only the scheme; keep the full host/path readable on two lines.
            const char *address = !strncmp(url, "https://", 8) ? url + 8 :
                                  !strncmp(url, "http://", 7) ? url + 7 : url;
            char display_url[64];
            strlcpy(display_url, address, sizeof(display_url));
            char *path = strchr(display_url, '/');
            if (path && strlen(display_url) + 1 < sizeof(display_url)) {
                memmove(path + 1, path, strlen(path) + 1);
                *path = '\n';
            }
            lv_label_set_text(s_bd_label, "授权码");
            lv_label_set_text(s_bd_code, code);
            lv_label_set_text_fmt(s_bd_detail, "手机浏览器打开：\n%s", display_url);
            lv_label_set_text(s_bd_status, code[0] ? "等待授权…" : "正在连接…");
            break;
        }
        case 2: {   // READY
            char name[KUKU_MAX_NAME] = "-";
            int done = 0, total = 0;
            kuku_baidu_get_progress(name, sizeof(name), &done, &total);
            lv_label_set_text(s_bd_label, "已授权");
            if (name[0] != 0) {
                lv_label_set_text_fmt(s_bd_detail, "最近：%s\n已完成 %d/%d", name, done, total);
                lv_label_set_text(s_bd_status, "按 OK 再次上传");
            } else {
                lv_label_set_text(s_bd_status, "按 OK 开始上传");
            }
            break;
        }
        case 3: {   // UPLOADING
            char name[KUKU_MAX_NAME] = "-";
            int done = 0, total = 0;
            kuku_baidu_get_progress(name, sizeof(name), &done, &total);
            lv_label_set_text(s_bd_label, "正在上传");
            lv_label_set_text_fmt(s_bd_code, "%d/%d", done, total);
            lv_label_set_text(s_bd_detail, name);
            break;
        }
        default:    // NO_AUTH
            lv_label_set_text(s_bd_label, "等待网盘授权");
            lv_label_set_text(s_bd_detail,
                g_kuku.wifi_up
                  ? "正在获取扫码授权码"
                  : "请先连接无线网络");
            break;
    }
}

// ---- Local cloud logout, default to cancel ----------------------------------
static bool s_reset_confirm;
static bool s_reset_started;
static lv_obj_t *s_reset_message;
static lv_obj_t *s_reset_choice[2];
static void reset_refresh(void) {
    if (!s_reset_message) return;
    int status = s_reset_started ? kuku_baidu_reset_status() : 0;
    if (status == 1) {
        lv_label_set_text(s_reset_message, "正在重置…\n请稍候");
    } else if (status == 2) {
        lv_label_set_text(s_reset_message, "连接已重置\nOK：重新扫码授权");
    } else if (status < 0) {
        lv_label_set_text(s_reset_message, "重置失败\nOK：重试");
    }
    for (int i = 0; i < 2; i++) {
        if (status != 0) lv_obj_add_flag(s_reset_choice[i], LV_OBJ_FLAG_HIDDEN);
        else {
            lv_obj_remove_flag(s_reset_choice[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(s_reset_choice[i],
                lv_color_hex((s_reset_confirm == (i == 1)) ? COLOR_ACCENT : COLOR_PANEL2), 0);
            lv_obj_set_style_text_color(lv_obj_get_child(s_reset_choice[i], 0),
                lv_color_hex((s_reset_confirm == (i == 1)) ? COLOR_BG : COLOR_TEXT), 0);
        }
    }
}
static void reset_build(void) {
    make_title("重置连接");
    lv_obj_t *p = make_panel(12, 48, 216, 228);
    s_reset_confirm = false;
    s_reset_started = kuku_baidu_reset_status() == 1;
    s_reset_message = make_label(p,
        "清除工牌网盘授权\n需重新扫码连接\n保留Wi-Fi、照片和录音",
        10, 12, 196, COLOR_TEXT, &kuku_font_14);
    lv_obj_set_style_text_line_space(s_reset_message, 8, 0);
    const char *labels[] = { "取消", "确认重置" };
    for (int i = 0; i < 2; i++) {
        s_reset_choice[i] = make_box(p, 10, 126 + i * 40, 196, 34, COLOR_PANEL2, 6);
        lv_obj_t *label = make_label(s_reset_choice[i], labels[i], 0, 3, 196,
                                     COLOR_TEXT, &kuku_font_20);
        center(label);
    }
    make_hint("上下选择 / OK确认 / 长按返回");
    reset_refresh();
}
void kuku_ui_reset_move(void) {
    if (!s_reset_started) s_reset_confirm = !s_reset_confirm;
    reset_refresh();
}
bool kuku_ui_reset_confirmed(void) { return s_reset_confirm; }
int kuku_ui_reset_status(void) { return s_reset_started ? kuku_baidu_reset_status() : 0; }
void kuku_ui_reset_feedback(int result) {
    if (result == 0) s_reset_started = true;
    else {
        s_reset_started = false;
        lv_label_set_text(s_reset_message,
            result == -3 ? "正在录音或传输\n请完成后重试" : "无法重置\nOK：重试");
    }
    reset_refresh();
}

// ---- 操作指南 --------------------------------------------------------------
static const char *const GUIDE_PAGES[] = {
    "任意键唤醒，短按OK菜单\n长按OK：开始或停止录音\n每分钟分段，持续录音\n每段上传后清理暂存\n断网 / 暂存满时保存停止",
    "录音按日期上传网盘\n目录：OK进入，长按返回\n返回再进可刷新目录\n图片：OK设为工牌照片\n网盘库库AI：转写或摘要",
    "菜单 > 无线网络 > OK\nOK：扫描附近网络\n空密码选<返回列表\n已存网络自动回连\n下键查看已存网络",
    "菜单 > 重置网盘连接\n上下选择，OK确认重置\n录音或传输时不可重置\n保留Wi-Fi、照片和录音\n重置后OK重新扫码授权",
    "电脑开启续航测试后\n拔下USB，后台收集数据\n录音仍用原来的按键\n测试不改变屏幕亮度\n后台结果为续航估算"
};
static const char *const GUIDE_SECTIONS[] = {
    "录音", "网盘文件", "无线网络", "重置连接", "续航测试"
};
static const char *const GUIDE_NAV[] = {
    "下键下一页 / 长按OK返回",
    "上下键翻页 / 长按OK返回",
    "上下键翻页 / 长按OK返回",
    "上下键翻页 / 长按OK返回",
    "上键上一页 / 长按OK返回"
};
#define GUIDE_N 5

static void guide_refresh(void) {
    if (s_guide_text) lv_label_set_text(s_guide_text, GUIDE_PAGES[s_guide_page]);
    if (s_guide_section) lv_label_set_text(s_guide_section, GUIDE_SECTIONS[s_guide_page]);
    if (s_guide_nav) lv_label_set_text(s_guide_nav, GUIDE_NAV[s_guide_page]);
    if (s_guide_count)
        lv_label_set_text_fmt(s_guide_count, "第%d/%d页", s_guide_page + 1, GUIDE_N);
}

void kuku_ui_guide_move(int delta) {
    int next = s_guide_page + delta;
    if (next < 0 || next >= GUIDE_N) return;
    s_guide_page = next;
    if (s_page == KUKU_PAGE_ABOUT) guide_refresh();
}

static void about_build(void) {
    make_title("操作指南");
    lv_obj_t *p = make_panel(12, 47, 216, 224);
    s_guide_section = make_label(p, "", 10, 10, 112, COLOR_ACCENT, &kuku_font_14);
    s_guide_count = make_label(p, "", 126, 10, 80, COLOR_ACCENT, &kuku_font_14);
    right(s_guide_count);
    make_box(p, 10, 34, 196, 1, COLOR_GRID, 0);
    s_guide_text = make_label(p, "", 10, 45, 196, COLOR_TEXT, &kuku_font_14);
    lv_obj_set_style_text_line_space(s_guide_text, 7, 0);
    lv_obj_t *nav = make_box(p, 8, 164, 200, 31, COLOR_PANEL2, 6);
    s_guide_nav = make_label(nav, "", 5, 6, 190, COLOR_ACCENT, &kuku_font_14);
    center(s_guide_nav);
    guide_refresh();
}

// ---- 页面切换 --------------------------------------------------------------
void kuku_ui_goto(kuku_page_t page) {
    // ★ 选中序号只在"进入页面"时重置;页内上下移动只换色/改文本。
    //   (v0.2 bug:这里无条件清零,menu_move 移动后又被这里归零 → 光标不动。)
    if (page == KUKU_PAGE_MENU && s_page != KUKU_PAGE_MENU) s_menu_sel = 0;
    if (page == KUKU_PAGE_ABOUT && s_page != KUKU_PAGE_ABOUT) s_guide_page = 0;
    if (page == KUKU_PAGE_FILES && s_page != KUKU_PAGE_FILES &&
        s_page != KUKU_PAGE_SHARE_LINK && s_page != KUKU_PAGE_IMAGE) s_files_sel = 0;
    if (page == KUKU_PAGE_WIFI_SCAN && s_page != KUKU_PAGE_WIFI_SCAN) s_scan_sel = 0;
    if (page == KUKU_PAGE_WIFI_SAVED && s_page != KUKU_PAGE_WIFI_DELETE)
        s_saved_sel = 0;

    // 删除旧页所有对象再建新页(全量重建,简单可靠)。
    lv_obj_t *old = lv_screen_active();
    lv_obj_clean(old);
    style_screen(old);
    s_scr = old;

    s_page = page;
    if (page == KUKU_PAGE_FILES) {
        s_files_list_mode = g_kuku.bd_state >= 2;
        s_list_next_request_ms = (uint32_t)(esp_timer_get_time() / 1000) + 15000;
    }
    s_batt = s_wifi_state_icon = s_cloud_state_icon = NULL;
    s_rec_icon = s_rec_icon_dot = s_home_status = NULL;
    s_image_panel = s_image_state = s_image_saved_label = NULL;
    s_guide_text = s_guide_count = s_guide_section = s_guide_nav = NULL;
    s_reset_message = NULL;
    s_reset_choice[0] = s_reset_choice[1] = NULL;
    s_wifi_label = s_bd_label = s_files_state = NULL;
    s_bd_code = s_bd_detail = s_bd_status = NULL;
    s_bd_qr = NULL;
    s_bd_qr_box = NULL;
    s_bd_qr_link[0] = 0;
    s_scan_state = s_kb_network = s_kb_mask = s_kb_status = NULL;
    s_saved_state = NULL;
    for (int i = 0; i < MENU_N; i++) s_menu_row[i] = s_menu_lbl[i] = NULL;
    for (int r = 0; r < FILES_ROWS; r++)
        s_files_row[r] = s_files_name[r] = s_files_info[r] = NULL;
    for (int r = 0; r < SCAN_ROWS; r++)
        s_scan_row[r] = s_scan_name[r] = s_scan_info[r] = NULL;
    for (int r = 0; r < SAVED_ROWS; r++)
        s_saved_row[r] = s_saved_name[r] = NULL;
    for (int i = 0; i < KB_MAX_KEYS; i++) s_kb_key[i] = NULL;

    switch (page) {
        case KUKU_PAGE_HOME:   home_build(); break;
        case KUKU_PAGE_MENU:   menu_build(); break;
        case KUKU_PAGE_BD_RESET: reset_build(); break;
        case KUKU_PAGE_FILES:
            if (s_files_list_mode) { files_build(); kuku_ui_files_refresh(); }
            else baidu_build();
            break;
        case KUKU_PAGE_WIFI:   wifi_build(); break;
        case KUKU_PAGE_WIFI_SCAN: scan_build(); scan_refresh(); break;
        case KUKU_PAGE_WIFI_KB:   kb_build(); kb_refresh(); break;
        case KUKU_PAGE_WIFI_SAVED: saved_build(); kuku_ui_saved_refresh(); break;
        case KUKU_PAGE_WIFI_DELETE: saved_delete_build(); break;
        case KUKU_PAGE_SHARE_LINK: share_build(); break;
        case KUKU_PAGE_ABOUT:  about_build(); break;
        case KUKU_PAGE_IMAGE:  image_build(); break;
    }
    status_bar_refresh();
}

void kuku_ui_init(void) {
    const lv_font_t *fonts[] = { &kuku_font_14, &kuku_font_20, &kuku_font_28 };
    int missing = 0;
    for (unsigned f = 0; f < 3; f++) {
        for (unsigned i = 0; i < sizeof(kuku_font_inventory) / sizeof(kuku_font_inventory[0]); i++) {
            lv_font_glyph_dsc_t glyph = {0};
            if (!lv_font_get_glyph_dsc(fonts[f], &glyph, kuku_font_inventory[i], 0) || glyph.is_placeholder) {
                ESP_LOGE("kuku_font", "missing U+%04lx", (unsigned long)kuku_font_inventory[i]);
                missing++;
            }
        }
    }
    ESP_LOGI("kuku_font", "coverage %s missing=%d", missing ? "FAIL" : "PASS", missing);
    s_scr = lv_screen_active();
    style_screen(s_scr);
    kuku_ui_goto(KUKU_PAGE_HOME);
}

void kuku_ui_set_status(const char *text) {
    // Called by the input task. Guard both the toast buffer and LVGL objects.
    if (!bsp_lvgl_lock(500)) return;
    strlcpy(s_toast, text ? text : "", sizeof(s_toast));
    s_toast_until_ms = (uint32_t)(esp_timer_get_time() / 1000) + 2000;
    if (s_page == KUKU_PAGE_HOME && s_home_status) {
        lv_obj_set_style_text_color(s_home_status, lv_color_hex(COLOR_ACCENT), 0);
        lv_label_set_text(s_home_status, s_toast);
    }
    bsp_lvgl_unlock();
}

void kuku_ui_set_play_progress(uint32_t done_ms, uint32_t total_ms) {
    if (!bsp_lvgl_lock(500)) return;
    if (s_page == KUKU_PAGE_HOME && s_home_status) {
        lv_obj_set_style_text_color(s_home_status, lv_color_hex(COLOR_GREEN), 0);
        lv_label_set_text_fmt(s_home_status, "%lu:%02lu / %lu:%02lu",
            (unsigned long)(done_ms / 60000), (unsigned long)(done_ms / 1000 % 60),
            (unsigned long)(total_ms / 60000), (unsigned long)(total_ms / 1000 % 60));
    }
    bsp_lvgl_unlock();
}

// 菜单/文件页选中项访问(main.c 的按键处理用)。
int kuku_ui_menu_sel(void) { return s_menu_sel; }
void kuku_ui_menu_move(int delta) {
    s_menu_sel = (s_menu_sel + delta + MENU_N) % MENU_N;
    if (s_page == KUKU_PAGE_MENU) menu_apply_sel();   // 只换色,不重建
}
int kuku_ui_files_sel(void) { return s_files_sel; }
void kuku_ui_files_move(int delta) {
    if (!s_files_list_mode) return;
    int total = files_total();
    if (total <= 0) return;
    if (s_files_sel >= total) s_files_sel = 0;
    s_files_sel = ((s_files_sel + delta) % total + total) % total;
    kuku_ui_files_refresh();
}

// ---- 屏上配网:扫描列表/键盘的状态与动作 --------------------------------------
int kuku_ui_scan_sel(void) { return s_scan_sel; }
void kuku_ui_scan_move(int delta) {
    int total = kuku_wifi_scan_count();
    if (total <= 0) return;
    if (s_scan_sel >= total) s_scan_sel = 0;
    s_scan_sel = ((s_scan_sel + delta) % total + total) % total;
    scan_refresh();
}

void kuku_ui_kb_open(const char *ssid) {
    strlcpy(s_kb_ssid, ssid ? ssid : "", sizeof(s_kb_ssid));
    s_kb_pass[0] = '\0';
    s_kb_page = 0;
    s_kb_sel = 0;
}

void kuku_ui_kb_move(int delta) {
    int n = kb_count(s_kb_page);
    if (n <= 0) return;
    s_kb_sel = ((s_kb_sel + delta) % n + n) % n;
    kb_refresh();
}

// 按下当前选中键(调用方须持 LVGL 锁;内部完成重绘,模式同 kuku_ui_menu_move)。
// 返回:0=已处理(已重绘) 1=GO(连接) 2=退回扫描列表。
int kuku_ui_kb_key(void) {
    const char *key = kb_key(s_kb_page, s_kb_sel);
    if (!key[0]) return 0;
    int rc = 0;
    if (strcmp(key, "ABC") == 0) { s_kb_page = 1; s_kb_sel = 0; }
    else if (strcmp(key, "abc") == 0) { s_kb_page = 0; s_kb_sel = 0; }
    else if (strcmp(key, "123") == 0) { s_kb_page = 2; s_kb_sel = 0; }
    else if (strcmp(key, "<") == 0) {
        size_t len = strlen(s_kb_pass);
        if (len) { s_kb_pass[len - 1] = '\0'; }   // 退格
        else rc = 2;                              // 空密码时退格=返回列表
    }
    else if (strcmp(key, "GO") == 0) return 1;
    else if (strlen(s_kb_pass) < 63)
        strlcat(s_kb_pass, strcmp(key, "SP") == 0 ? " " : key, sizeof(s_kb_pass));
    if (rc == 0) kb_refresh();
    return rc;
}

void kuku_ui_kb_get(char *ssid, size_t scap, char *pass, size_t pcap) {
    if (ssid) strlcpy(ssid, s_kb_ssid, scap);
    if (pass) strlcpy(pass, s_kb_pass, pcap);
}

// ---- 500ms 刷新定时器 -------------------------------------------------------
static void ui_timer_cb(lv_timer_t *t) {
    (void)t;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    status_bar_refresh();
    if (s_page == KUKU_PAGE_HOME && s_home_status && !g_kuku.playing) {
        if (s_toast[0] && now_ms < s_toast_until_ms) {
            lv_obj_set_style_text_color(s_home_status, lv_color_hex(COLOR_ACCENT), 0);
            lv_label_set_text(s_home_status, s_toast);
        } else if (g_kuku.recording) {
            lv_obj_set_style_text_color(s_home_status, lv_color_hex(COLOR_GREEN), 0);
            lv_label_set_text_fmt(s_home_status, "录音 %lu:%02lu | 长按OK停止",
                (unsigned long)(g_kuku.rec_ms / 60000),
                (unsigned long)(g_kuku.rec_ms / 1000 % 60));
        } else {
            lv_label_set_text(s_home_status, "");
        }
    }
    if (s_page == KUKU_PAGE_BD_RESET) reset_refresh();
    if (s_page == KUKU_PAGE_WIFI) kuku_ui_wifi_refresh();
    if (s_page == KUKU_PAGE_WIFI_SCAN) scan_refresh();
    if (s_page == KUKU_PAGE_IMAGE) kuku_ui_image_refresh();
    if (s_page == KUKU_PAGE_FILES) {
        if (s_files_list_mode != (g_kuku.bd_state >= 2)) {
            kuku_ui_goto(KUKU_PAGE_FILES);
        } else if (!s_files_list_mode) {
            if (g_kuku.bd_state == 0 && g_kuku.wifi_up) kuku_baidu_auth_start();
            kuku_ui_baidu_refresh();
        } else {
            int status = 0;
            int page = 0;
            kuku_baidu_list_status(&page, NULL, &status, NULL);
            // Keep a successful directory snapshot while the user browses.
            // A background reload clears its entries and resets the selection.
            // Re-entering the directory explicitly refreshes it; failed reads
            // still retry after the bounded backoff.
            if (g_kuku.bd_state == 2 && (status == 0 ||
                (status < 0 && (int32_t)(now_ms - s_list_next_request_ms) >= 0))) {
                if (kuku_baidu_list_request(status == 0 ? 0 : page) == 0)
                    s_list_next_request_ms = now_ms + (status < 0 ? 15000 : 60000);
            }
            kuku_ui_files_refresh();
        }
    }
}

// 供 main.c 在主页建好后启动定时器。
void kuku_ui_timer_start(void) {
    lv_timer_create(ui_timer_cb, 500, NULL);
}
