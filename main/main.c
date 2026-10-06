// main/main.c —— Netdisk Recording Badge 应用入口:初始化、按键分发、页面状态机、心跳。
//
// 按键语义:
//   下+确认    任意页/熄屏时开始录音并回工牌主页
//   任意键     熄屏唤醒(录音中仍保持工牌状态页)
//   OK 长按    主页=开始/停止录音;各子页=返回主页
//   OK 短按    主页=菜单;菜单=进入;文件页=回放;Wi-Fi页=扫描;
//              扫描页=选网进键盘;键盘=按键;文件页=播放/分享示例
//   上/下 短按  菜单/列表/键盘=移动;回放中=音量
//   上/下 长按  文件/扫描/键盘页=快进 ±6 条(学城市电台键盘页);键盘页 OK长按=GO
//   下 长按     主页=熄屏
//
// Wi-Fi(v0.3.2 起纯 STA + 屏上配网,不做 SoftAP 门户):
//   开机自动扫描回连已存网络(家/办公室/手机热点,谁在连谁);
//   屏上配网:WiFi页 OK → 扫描列表 → 选网 → 屏上键盘输密码 → GO 连接存 NVS;
//   串口同样可配:WIFI SET <ssid>|<pass> / SCAN / LIST / DEL <n>。
#include "kuku_app.h"
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "kuku_endurance.h"
#include "bsp_pins.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "main";

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

static QueueHandle_t s_input_queue;
static volatile bool s_input_ready;
static kuku_page_t s_page = KUKU_PAGE_HOME;
static uint32_t s_last_activity_ms;
static uint8_t s_brightness = 100;
static int s_wake_button = -1;
static uint32_t s_chord_until_ms;

// 空闲熄屏:主页 15 s 无操作自动熄屏(录音/回放时除外)。
#define IDLE_TIMEOUT_MS 15000

static void backlight_apply(void) {
    bsp_display_backlight(g_kuku.screen_off ? 0 : s_brightness);
}

static void screen_off(bool off) {
    if (g_kuku.screen_off == off) return;
    g_kuku.screen_off = off;
    backlight_apply();
}

// ---- Wi-Fi 事件(esp-wifi-connect 回调,事件任务上下文) ----------------------
static void on_wifi_event(int evt, const char *data) {
    switch (evt) {
        case 2:   // connected
            ESP_LOGI(TAG, "Wi-Fi connected: %s", data);
            g_kuku.wifi_up = true;
            kuku_baidu_on_wifi(true);
            break;
        case 3:   // disconnected
            g_kuku.wifi_up = false;
            kuku_baidu_on_wifi(false);
            break;
        default:
            // 0/1=扫描/连接中,不打日志。
            break;
    }
}

// ---- 按键处理(输入任务上下文,非 LVGL 任务) ------------------------------
static void handle_input(const input_event_t *in) {
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_last_activity_ms = now_ms;
    if (g_kuku.recording && s_brightness < 100) {
        s_brightness = 100;
        backlight_apply();
    }

    // The ADC ladder exposes DOWN+OK as a fourth voltage band. Dispatch it
    // before wake/page/playback handling, so one gesture works everywhere.
    if (in->btn == BSP_BTN_DOWN_OK) {
        if (in->event != BSP_BTN_PRESS || g_kuku.recording) return;
        s_chord_until_ms = now_ms + 1000;
        s_wake_button = -1;
        int rc = g_kuku.playing ? kuku_play_stop() : 0;
        if (rc == 0) {
            if (s_page == KUKU_PAGE_WIFI_SCAN || s_page == KUKU_PAGE_WIFI_KB) {
                kuku_wifi_scan_cancel();
                kuku_wifi_restart_station();
            }
            rc = kuku_baidu_request_recording();
        }
        s_chord_until_ms = (uint32_t)(esp_timer_get_time() / 1000) + 1000;
        s_page = KUKU_PAGE_HOME;
        s_brightness = 100;
        screen_off(false);
        if (bsp_lvgl_lock(500)) {
            kuku_ui_goto(KUKU_PAGE_HOME);
            bsp_lvgl_unlock();
        }
        if (rc != 0) ESP_LOGW(TAG, "录音请求未派发 rc=%d", rc);
        return;
    }

    // 录音中:只接受 OK 长按停止,状态留在工牌页顶栏。
    if (g_kuku.recording) {
        if (in->btn == BSP_BTN_OK && in->event == BSP_BTN_LONG) {
            int rc = kuku_rec_stop();
            // 停止后回到主页并亮屏,给出"已保存"的确认感。
            if (bsp_lvgl_lock(500)) {
                kuku_ui_goto(KUKU_PAGE_HOME);
                bsp_lvgl_unlock();
            }
            s_page = KUKU_PAGE_HOME;
            s_brightness = 100;
            screen_off(false);
            kuku_ui_set_status(rc == 0 ? "已保存，待云端确认" : "保存未完成");
        }
        return;
    }

    // A chord may briefly classify as a DOWN/OK release. Do not turn that
    // trailing event into navigation or a second action.
    if ((in->btn == BSP_BTN_DOWN || in->btn == BSP_BTN_OK) &&
        (int32_t)(s_chord_until_ms - now_ms) > 0) return;

    // Consume the entire wake gesture, not only PRESS_DOWN. Physical buttons
    // emit PRESS followed by CLICK/DOUBLE/LONG for the same gesture.
    if (s_wake_button == (int)in->btn) {
        if (in->event != BSP_BTN_PRESS) s_wake_button = -1;
        return;
    }
    if (g_kuku.screen_off) {
        s_brightness = 100;
        screen_off(false);
        if (in->event == BSP_BTN_PRESS) s_wake_button = (int)in->btn;
        return;
    }

    // Wi-Fi 状态页:OK=扫描附近网络,下键=管理已存网络。
    if (s_page == KUKU_PAGE_WIFI) {
        if (in->btn == BSP_BTN_OK && in->event == BSP_BTN_LONG) goto back_home;
        if (in->btn == BSP_BTN_OK && in->event == BSP_BTN_CLICK) {
            kuku_wifi_scan_start();
            s_page = KUKU_PAGE_WIFI_SCAN;
            if (bsp_lvgl_lock(500)) { kuku_ui_goto(KUKU_PAGE_WIFI_SCAN); bsp_lvgl_unlock(); }
        } else if (in->btn == BSP_BTN_DOWN && in->event == BSP_BTN_CLICK) {
            s_page = KUKU_PAGE_WIFI_SAVED;
            if (bsp_lvgl_lock(500)) { kuku_ui_goto(KUKU_PAGE_WIFI_SAVED); bsp_lvgl_unlock(); }
        }
        return;
    }

    if (s_page == KUKU_PAGE_WIFI_SAVED) {
        if (in->btn == BSP_BTN_OK && in->event == BSP_BTN_LONG) {
            s_page = KUKU_PAGE_WIFI;
            if (bsp_lvgl_lock(500)) { kuku_ui_goto(KUKU_PAGE_WIFI); bsp_lvgl_unlock(); }
        } else if (in->btn == BSP_BTN_UP && in->event == BSP_BTN_CLICK) {
            if (bsp_lvgl_lock(500)) { kuku_ui_saved_move(-1); bsp_lvgl_unlock(); }
        } else if (in->btn == BSP_BTN_DOWN && in->event == BSP_BTN_CLICK) {
            if (bsp_lvgl_lock(500)) { kuku_ui_saved_move(1); bsp_lvgl_unlock(); }
        } else if (in->btn == BSP_BTN_OK && in->event == BSP_BTN_CLICK &&
                   kuku_wifi_saved_count() > 0) {
            s_page = KUKU_PAGE_WIFI_DELETE;
            if (bsp_lvgl_lock(500)) { kuku_ui_goto(KUKU_PAGE_WIFI_DELETE); bsp_lvgl_unlock(); }
        }
        return;
    }

    if (s_page == KUKU_PAGE_WIFI_DELETE) {
        if (in->btn == BSP_BTN_OK && in->event == BSP_BTN_LONG) {
            s_page = KUKU_PAGE_WIFI_SAVED;
            if (bsp_lvgl_lock(500)) { kuku_ui_goto(KUKU_PAGE_WIFI_SAVED); bsp_lvgl_unlock(); }
        } else if (in->btn == BSP_BTN_OK && in->event == BSP_BTN_CLICK) {
            int rc = kuku_wifi_del(kuku_ui_saved_sel());
            s_page = KUKU_PAGE_WIFI_SAVED;
            if (bsp_lvgl_lock(500)) { kuku_ui_goto(KUKU_PAGE_WIFI_SAVED); bsp_lvgl_unlock(); }
            if (rc != 0) ESP_LOGW(TAG, "删除已存网络失败");
        }
        return;
    }

    // 扫描列表页:上/下=移动(长按快进6),OK=给选中网络输密码,OK长按=放弃扫描回状态页。
    if (s_page == KUKU_PAGE_WIFI_SCAN) {
        bool sc = in->event == BSP_BTN_CLICK;
        bool sl = in->event == BSP_BTN_LONG;
        if (in->btn == BSP_BTN_OK && sl) {
            kuku_wifi_scan_cancel();
            kuku_wifi_restart_station();       // 恢复常规扫描回连
            s_page = KUKU_PAGE_WIFI;
            if (bsp_lvgl_lock(500)) { kuku_ui_goto(KUKU_PAGE_WIFI); bsp_lvgl_unlock(); }
        } else if (in->btn == BSP_BTN_OK && sc) {
            char ssid[33]; int rssi; bool secure;
            if (kuku_wifi_scan_get(kuku_ui_scan_sel(), ssid, sizeof(ssid),
                                   &rssi, &secure)) {
                if (bsp_lvgl_lock(500)) {
                    kuku_ui_kb_open(ssid);
                    s_page = KUKU_PAGE_WIFI_KB;
                    kuku_ui_goto(KUKU_PAGE_WIFI_KB);
                    bsp_lvgl_unlock();
                }
            }
        } else if (in->btn == BSP_BTN_UP && sc) {
            if (bsp_lvgl_lock(500)) { kuku_ui_scan_move(-1); bsp_lvgl_unlock(); }
        } else if (in->btn == BSP_BTN_DOWN && sc) {
            if (bsp_lvgl_lock(500)) { kuku_ui_scan_move(1); bsp_lvgl_unlock(); }
        } else if (in->btn == BSP_BTN_UP && sl) {
            if (bsp_lvgl_lock(500)) { kuku_ui_scan_move(-6); bsp_lvgl_unlock(); }
        } else if (in->btn == BSP_BTN_DOWN && sl) {
            if (bsp_lvgl_lock(500)) { kuku_ui_scan_move(6); bsp_lvgl_unlock(); }
        }
        return;
    }

    // 密码键盘页:OK=按键,OK长按=GO连接,上/下=移动/快进,退格在空密码时回列表。
    if (s_page == KUKU_PAGE_WIFI_KB) {
        bool kc = in->event == BSP_BTN_CLICK;
        bool kl = in->event == BSP_BTN_LONG;
        if (in->btn == BSP_BTN_OK && (kc || kl)) {
            int rc;
            if (!bsp_lvgl_lock(500)) return;
            rc = kl ? 1 : kuku_ui_kb_key();
            bsp_lvgl_unlock();
            if (rc == 1) {                     // GO:存凭证并立即回连
                char ssid[33], pass[65];
                kuku_ui_kb_get(ssid, sizeof(ssid), pass, sizeof(pass));
                kuku_wifi_scan_cancel();
                kuku_wifi_add(ssid, pass);
                s_page = KUKU_PAGE_WIFI;
                if (bsp_lvgl_lock(500)) { kuku_ui_goto(KUKU_PAGE_WIFI); bsp_lvgl_unlock(); }
            } else if (rc == 2) {              // 退格在空密码 → 回扫描列表
                s_page = KUKU_PAGE_WIFI_SCAN;
                if (bsp_lvgl_lock(500)) { kuku_ui_goto(KUKU_PAGE_WIFI_SCAN); bsp_lvgl_unlock(); }
            }
        } else if (in->btn == BSP_BTN_UP && kc) {
            if (bsp_lvgl_lock(500)) { kuku_ui_kb_move(-1); bsp_lvgl_unlock(); }
        } else if (in->btn == BSP_BTN_DOWN && kc) {
            if (bsp_lvgl_lock(500)) { kuku_ui_kb_move(1); bsp_lvgl_unlock(); }
        } else if (in->btn == BSP_BTN_UP && kl) {
            if (bsp_lvgl_lock(500)) { kuku_ui_kb_move(-6); bsp_lvgl_unlock(); }
        } else if (in->btn == BSP_BTN_DOWN && kl) {
            if (bsp_lvgl_lock(500)) { kuku_ui_kb_move(6); bsp_lvgl_unlock(); }
        }
        return;
    }

    // 回放中:OK 停止;上/下调音量;其余吞掉。
    if (g_kuku.playing) {
        if (in->btn == BSP_BTN_OK && in->event == BSP_BTN_CLICK) {
            kuku_play_stop();
        } else if (in->event == BSP_BTN_CLICK &&
                   (in->btn == BSP_BTN_UP || in->btn == BSP_BTN_DOWN)) {
            int v = (int)g_kuku.volume + (in->btn == BSP_BTN_UP ? 10 : -10);
            if (v < 0) v = 0;
            if (v > 100) v = 100;
            g_kuku.volume = (uint8_t)v;
            bsp_audio_set_volume(g_kuku.volume);
        }
        return;
    }

    bool is_click = in->event == BSP_BTN_CLICK;
    bool is_long = in->event == BSP_BTN_LONG;

    switch (s_page) {
        case KUKU_PAGE_HOME:
            if (in->btn == BSP_BTN_OK && is_long) {
                if (kuku_baidu_request_recording() == 0) {
                    s_brightness = 100;
                    backlight_apply();
                }
            } else if (in->btn == BSP_BTN_OK && is_click) {
                s_page = KUKU_PAGE_MENU;
                if (bsp_lvgl_lock(500)) { kuku_ui_goto(KUKU_PAGE_MENU); bsp_lvgl_unlock(); }
            } else if (in->btn == BSP_BTN_DOWN && is_long) {
                screen_off(true);
            }
            break;

        case KUKU_PAGE_MENU:
            if (in->btn == BSP_BTN_OK && is_long) goto back_home;
            if (in->btn == BSP_BTN_UP && is_click) {
                if (bsp_lvgl_lock(500)) { kuku_ui_menu_move(-1); bsp_lvgl_unlock(); }
            } else if (in->btn == BSP_BTN_DOWN && is_click) {
                if (bsp_lvgl_lock(500)) { kuku_ui_menu_move(1); bsp_lvgl_unlock(); }
            } else if (in->btn == BSP_BTN_OK && is_click) {
                switch (kuku_ui_menu_sel()) {
                    case 0:
                        s_page = KUKU_PAGE_FILES;
                        if (bsp_lvgl_lock(500)) { kuku_ui_goto(KUKU_PAGE_FILES); bsp_lvgl_unlock(); }
                        if (g_kuku.bd_state < 2) kuku_baidu_auth_start();
                        else kuku_baidu_list_request(0);
                        break;
                    case 1:
                        s_page = KUKU_PAGE_WIFI;
                        if (bsp_lvgl_lock(500)) { kuku_ui_goto(KUKU_PAGE_WIFI); bsp_lvgl_unlock(); }
                        break;
                    case 3:
                        s_page = KUKU_PAGE_BD_RESET;
                        if (bsp_lvgl_lock(500)) { kuku_ui_goto(KUKU_PAGE_BD_RESET); bsp_lvgl_unlock(); }
                        break;
                    case 2:
                        s_page = KUKU_PAGE_ABOUT;
                        if (bsp_lvgl_lock(500)) { kuku_ui_goto(KUKU_PAGE_ABOUT); bsp_lvgl_unlock(); }
                        break;
                }
            }
            break;

        case KUKU_PAGE_BD_RESET:
            if (in->btn == BSP_BTN_OK && is_long) goto back_home;
            if (kuku_ui_reset_status() == 1) break;
            if ((in->btn == BSP_BTN_UP || in->btn == BSP_BTN_DOWN) && is_click) {
                if (bsp_lvgl_lock(500)) { kuku_ui_reset_move(); bsp_lvgl_unlock(); }
            } else if (in->btn == BSP_BTN_OK && is_click) {
                if (kuku_ui_reset_status() == 2) {
                    s_page = KUKU_PAGE_FILES;
                    if (bsp_lvgl_lock(500)) { kuku_ui_goto(KUKU_PAGE_FILES); bsp_lvgl_unlock(); }
                    kuku_baidu_auth_start();
                } else if (!kuku_ui_reset_confirmed()) {
                    goto back_home;
                } else {
                    int rc = kuku_baidu_reset_start();
                    if (bsp_lvgl_lock(500)) { kuku_ui_reset_feedback(rc); bsp_lvgl_unlock(); }
                }
            }
            break;

        case KUKU_PAGE_FILES:
            if (in->btn == BSP_BTN_OK && is_long) {
                if (kuku_baidu_list_is_root()) goto back_home;
                if (kuku_baidu_list_up() == 0 && bsp_lvgl_lock(500)) {
                    kuku_ui_files_reset_sel();
                    kuku_ui_goto(KUKU_PAGE_FILES);
                    bsp_lvgl_unlock();
                }
                break;
            }
            if (g_kuku.bd_state < 2) {
                if (in->btn == BSP_BTN_OK && is_click) kuku_baidu_auth_start();
                break;
            }
            if (in->btn == BSP_BTN_UP && is_click) {
                if (bsp_lvgl_lock(500)) { kuku_ui_files_move(-1); bsp_lvgl_unlock(); }
            } else if (in->btn == BSP_BTN_DOWN && is_click) {
                if (bsp_lvgl_lock(500)) { kuku_ui_files_move(1); bsp_lvgl_unlock(); }
            } else if (in->btn == BSP_BTN_UP && is_long) {
                // 长按快进(LEO 键盘页同款 ±6)。
                if (bsp_lvgl_lock(500)) { kuku_ui_files_move(-6); bsp_lvgl_unlock(); }
            } else if (in->btn == BSP_BTN_DOWN && is_long) {
                if (bsp_lvgl_lock(500)) { kuku_ui_files_move(6); bsp_lvgl_unlock(); }
            } else if (in->btn == BSP_BTN_OK && is_click) {
                kuku_cloud_file_t cloud = {0};
                char local[KUKU_MAX_NAME] = {0};
                int action = kuku_ui_files_selected(&cloud, local, sizeof(local));
                if (action == 1) {
                    if (kuku_play_start(local) != 0) kuku_ui_set_status("播放失败");
                } else if (action == 2) {
                    if (cloud.is_dir) {
                        if (kuku_baidu_list_enter(&cloud) == 0 && bsp_lvgl_lock(500)) {
                            kuku_ui_files_reset_sel();
                            kuku_ui_goto(KUKU_PAGE_FILES);
                            bsp_lvgl_unlock();
                        }
                    } else if (kuku_baidu_is_jpeg(cloud.name)) {
                        if (kuku_baidu_image_request(&cloud) == 0) {
                            if (bsp_lvgl_lock(500)) {
                                kuku_ui_image_open(&cloud);
                                s_page = KUKU_PAGE_IMAGE;
                                kuku_ui_goto(KUKU_PAGE_IMAGE);
                                bsp_lvgl_unlock();
                            }
                        } else kuku_ui_set_status("图片暂不可读取");
                    } else if (bsp_lvgl_lock(500)) {
                        kuku_ui_share_open(&cloud);
                        s_page = KUKU_PAGE_SHARE_LINK;
                        kuku_ui_goto(KUKU_PAGE_SHARE_LINK);
                        bsp_lvgl_unlock();
                    }
                } else if (action == 3 || action == 4) {
                    int page = 0;
                    kuku_baidu_list_status(&page, NULL, NULL, NULL);
                    if (kuku_baidu_list_request(page + (action == 4 ? 1 : -1)) == 0 &&
                        bsp_lvgl_lock(500)) {
                        kuku_ui_files_reset_sel();
                        kuku_ui_files_refresh();
                        bsp_lvgl_unlock();
                    }
                } else {
                    kuku_baidu_list_request(0);
                }
            }
            break;

        case KUKU_PAGE_SHARE_LINK:
            if (in->btn == BSP_BTN_OK && is_long) {
                s_page = KUKU_PAGE_FILES;
                if (bsp_lvgl_lock(500)) { kuku_ui_goto(KUKU_PAGE_FILES); bsp_lvgl_unlock(); }
            }
            break;

        case KUKU_PAGE_IMAGE:
            if (in->btn == BSP_BTN_OK && is_long) {
                s_page = KUKU_PAGE_FILES;
                if (bsp_lvgl_lock(500)) { kuku_ui_goto(KUKU_PAGE_FILES); bsp_lvgl_unlock(); }
            } else if (in->btn == BSP_BTN_OK && is_click &&
                       kuku_baidu_image_status() == 2) {
                if (kuku_baidu_avatar_save() != 0)
                    kuku_ui_set_status("照片保存失败");
            }
            break;

        case KUKU_PAGE_ABOUT:
            if (in->btn == BSP_BTN_OK && is_long) goto back_home;
            if (in->btn == BSP_BTN_UP && is_click) {
                if (bsp_lvgl_lock(500)) { kuku_ui_guide_move(-1); bsp_lvgl_unlock(); }
            } else if (in->btn == BSP_BTN_DOWN && is_click) {
                if (bsp_lvgl_lock(500)) { kuku_ui_guide_move(1); bsp_lvgl_unlock(); }
            }
            break;

        // Wi-Fi 页 / 扫描页 / 键盘页在上面已提前处理并 return;
        // 此处仅为覆盖枚举。
        case KUKU_PAGE_WIFI:
        case KUKU_PAGE_WIFI_SCAN:
        case KUKU_PAGE_WIFI_KB:
        case KUKU_PAGE_WIFI_SAVED:
        case KUKU_PAGE_WIFI_DELETE:
            break;
        }
        return;

    back_home:
        s_page = KUKU_PAGE_HOME;
        if (bsp_lvgl_lock(500)) { kuku_ui_goto(KUKU_PAGE_HOME); bsp_lvgl_unlock(); }
    }

static void input_task(void *arg) {
    (void)arg;
    input_event_t in;
    for (;;) {
        if (xQueueReceive(s_input_queue, &in, portMAX_DELAY) == pdTRUE) {
            handle_input(&in);
        }
    }
}

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (!s_input_ready || !s_input_queue) return;
    const input_event_t in = { .btn = btn, .event = ev };
    (void)xQueueSend(s_input_queue, &in, 0);
}

int kuku_test_key(int button, bool long_press) {
    if (!s_input_ready || button < 0 || button > 3) return -1;
    const bsp_btn_t buttons[] = { BSP_BTN_UP, BSP_BTN_OK, BSP_BTN_DOWN,
                                   BSP_BTN_DOWN_OK };
    const input_event_t press = { .btn = buttons[button], .event = BSP_BTN_PRESS };
    if (xQueueSend(s_input_queue, &press, 0) != pdTRUE) return -1;
    // Same PRESS -> semantic event order as BSP. Include hold time and the
    // single-click classification interval; this is not an ADC/contact test.
    vTaskDelay(pdMS_TO_TICKS(long_press ? CONFIG_BUTTON_LONG_PRESS_TIME_MS + 50 :
                            CONFIG_BUTTON_SHORT_PRESS_TIME_MS + 100));
    const input_event_t in = { .btn = buttons[button],
        .event = long_press ? BSP_BTN_LONG : BSP_BTN_CLICK };
    return xQueueSend(s_input_queue, &in, 0) == pdTRUE ? 0 : -1;
}

int kuku_test_page(void) { return s_page; }

// ---- 空闲熄屏 + 60s 心跳 ----------------------------------------------------
static void idle_timer_cb(void *arg) {
    (void)arg;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (g_kuku.recording && !g_kuku.screen_off && s_brightness > 15 &&
        now - s_last_activity_ms > IDLE_TIMEOUT_MS) {
        s_brightness = 15;
        backlight_apply();
        return;
    }
    if (!g_kuku.screen_off && !g_kuku.recording && !g_kuku.playing &&
        s_page == KUKU_PAGE_HOME &&
        now - s_last_activity_ms > IDLE_TIMEOUT_MS) {
        screen_off(true);
        ESP_LOGI(TAG, "空闲熄屏");
    }
}

static void heartbeat_cb(void *arg) {
    (void)arg;
    kuku_baidu_retry_pending();
    ESP_LOGI(TAG, "[HB] free=%u min=%u largest=%u rec=%d play=%d wifi=%d bd=%u files=%d freeKB=%ld",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             g_kuku.recording, g_kuku.playing,
             g_kuku.wifi_up, g_kuku.bd_state,
             kuku_rec_count(), kuku_rec_free_kb());
}

// ---- 入口 -------------------------------------------------------------------
void app_main(void) {
    ESP_LOGI(TAG, "Netdisk Recording Badge v0.4.1 启动(中文界面)");

    bsp_i2c_init();

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败: MOSI=%d SCLK=%d CS=%d DC=%d BL=%d",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }

    if (kuku_rec_init() != 0) {
        ESP_LOGE(TAG, "录音存储不可用(仍可进 UI)");
    }
    kuku_baidu_init();
    bsp_battery_init();
    g_kuku.volume = 80;

    if (bsp_lvgl_lock(1000)) {
        kuku_ui_init();
        kuku_ui_timer_start();
        bsp_lvgl_unlock();
    }
    bsp_display_backlight(s_brightness);
    s_last_activity_ms = (uint32_t)(esp_timer_get_time() / 1000);

    s_input_queue = xQueueCreate(8, sizeof(input_event_t));
    if (s_input_queue &&
        xTaskCreate(input_task, "kuku_input", 6144, NULL, 5, NULL) == pdPASS &&
        bsp_button_init(on_key, NULL) == ESP_OK) {
        s_input_ready = true;
    } else {
        ESP_LOGE(TAG, "按键初始化失败");
    }

    const esp_timer_create_args_t idle_args = { .callback = idle_timer_cb,
                                                .name = "kuku_idle" };
    esp_timer_handle_t idle_timer;
    if (esp_timer_create(&idle_args, &idle_timer) == ESP_OK)
        esp_timer_start_periodic(idle_timer, 1000 * 1000);

    const esp_timer_create_args_t hb_args = { .callback = heartbeat_cb,
                                              .name = "kuku_hb" };
    esp_timer_handle_t hb_timer;
    if (esp_timer_create(&hb_args, &hb_timer) == ESP_OK)
        esp_timer_start_periodic(hb_timer, 60 * 1000 * 1000);

    // ---- Wi-Fi:纯 STA(主动连已存网络),不开 SoftAP 配网门户 ----
    kuku_wifi_init(on_wifi_event);
    if (kuku_wifi_saved_count() == 0) {
        ESP_LOGI(TAG, "无已存网络:串口 'WIFI SET <ssid>|<pass>' 配网"
                      "(支持多条:家/办公室/手机热点)");
    }
    // 扫描回连:已存网络谁在连谁;没有也照常起(空转扫,背压 30s)。
    kuku_wifi_start_station();

    ESP_LOGI(TAG, "就绪: 下+确认任意页请求云端检查后录音, 菜单可回放/Wi-Fi/网盘");
    kuku_test_start();
    kuku_endurance_start();
}
