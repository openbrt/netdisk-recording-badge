// main/kuku_wifi.h —— esp-wifi-connect 的 C 桥接(纯 STA 用法)。
//
// v0.3.1 起设备只做 STA(主动连别人),不再开 SoftAP 配网门户:
//   - 凭证可经屏上键盘或串口 WIFI SET 写入 SsidManager(NVS 持久化,最多 10 条)
//   - 开机/写入后自动扫描,已存网络谁在就连谁(家/办公室/手机热点)
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// 事件码: 0 scanning / 1 connecting(ssid) / 2 connected(ssid) / 3 disconnected(reason)
typedef void (*kuku_wifi_evt_cb_t)(int evt, const char *data);

void kuku_wifi_init(kuku_wifi_evt_cb_t cb);   // 全局 Wi-Fi 初始化(一次)
int  kuku_wifi_saved_count(void);             // NVS 中已存网络数
void kuku_wifi_start_station(void);           // 启动 STA(扫描+回连已存网络)
// 写入凭证并重启 STA(去重同 SSID;停扫后再改列表,避免与扫描任务竞态)。
// 返回 0 成功。
int  kuku_wifi_add(const char *ssid, const char *pass);
int  kuku_wifi_saved_get(int idx, char *out, size_t cap);   // 0 成功
int  kuku_wifi_del(int idx);                // 0 成功,越界返回 -1
void kuku_wifi_restart_station(void);         // 停启一次,强制立即重扫

// ---- 屏上配网用:纯 STA 现场扫描(非 SoftAP 门户) ---------------------------
// 启动后台扫描任务:停掉组件扫描定时器独占 radio,阻塞扫描一次,
// 按 SSID 去重取最强信号、RSSI 降序,最多 16 条。结果用 scan_get 读取。
void kuku_wifi_scan_start(void);
bool kuku_wifi_scan_done(void);               // 扫描完成(或已放弃)
void kuku_wifi_scan_cancel(void);             // 请求中止(任务在下个间隙退出)
int  kuku_wifi_scan_count(void);
bool kuku_wifi_scan_get(int idx, char *ssid, size_t cap, int *rssi, bool *secure);

bool kuku_wifi_is_connected(void);
void kuku_wifi_get_ssid(char *out, size_t cap);
void kuku_wifi_get_ip(char *out, size_t cap);
int  kuku_wifi_get_rssi(void);

#ifdef __cplusplus
}
#endif
