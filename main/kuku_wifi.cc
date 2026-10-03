// main/kuku_wifi.cc —— C 桥接实现:WifiManager / SsidManager 单例的极薄封装。
//
// 纯 STA:支持保存多个网络并自动回连。
#include "kuku_wifi.h"

#include "wifi_manager.h"
#include "ssid_manager.h"

#include <esp_log.h>
#include <esp_wifi.h>
#include <string.h>
#include <algorithm>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mutex>
#include <vector>

static kuku_wifi_evt_cb_t s_cb = nullptr;

static int evt_code(WifiEvent e) {
    switch (e) {
        case WifiEvent::Scanning:         return 0;
        case WifiEvent::Connecting:       return 1;
        case WifiEvent::Connected:        return 2;
        case WifiEvent::Disconnected:     return 3;
    }
    return 9;
}

void kuku_wifi_init(kuku_wifi_evt_cb_t cb) {
    s_cb = cb;
    WifiManagerConfig cfg;
    cfg.station_hostname = "netdisk-recording-badge";
    cfg.station_scan_min_interval_seconds = 5;
    cfg.station_scan_max_interval_seconds = 30;
    if (!WifiManager::GetInstance().Initialize(cfg)) {
        ESP_LOGE("kuku_wifi", "WifiManager init failed");
        return;
    }
    WifiManager::GetInstance().SetEventCallback(
        [](WifiEvent e, const std::string &d) {
            if (s_cb) s_cb(evt_code(e), d.c_str());
        });
}

int kuku_wifi_saved_count(void) {
    return (int)SsidManager::GetInstance().GetSsidList().size();
}

void kuku_wifi_start_station(void) {
    WifiManager::GetInstance().StartStation();
}

// SsidManager 自身无锁,而扫描回调(WiFi 事件任务)每轮都读 GetSsidList()。
// 所以改列表前先 StopStation(注销事件+停扫描定时器),改完再 StartStation
// 顺带触发立即扫描 —— 安全与"写完马上连"一个动作解决。
int kuku_wifi_add(const char *ssid, const char *pass) {
    if (!ssid || !ssid[0] || !pass || !pass[0]) return -1;
    WifiManager &m = WifiManager::GetInstance();
    m.StopStation();
    // Manual scan starts the radio without a managed station instance.
    // Stop it explicitly so StartStation receives WIFI_EVENT_STA_START.
    esp_wifi_stop();
    SsidManager &s = SsidManager::GetInstance();
    const auto &list = s.GetSsidList();
    for (int i = (int)list.size() - 1; i >= 0; i--)   // 同 SSID 去重(倒序删安全)
        if (list[i].ssid == ssid) s.RemoveSsid(i);
    s.AddSsid(ssid, pass);
    m.StartStation();
    ESP_LOGI("kuku_wifi", "凭证已存并重启 STA: %s", ssid);
    return 0;
}

int kuku_wifi_saved_get(int idx, char *out, size_t cap) {
    const auto &list = SsidManager::GetInstance().GetSsidList();
    if (idx < 0 || idx >= (int)list.size() || !out || cap == 0) return -1;
    strlcpy(out, list[idx].ssid.c_str(), cap);
    return 0;
}

int kuku_wifi_del(int idx) {
    if (idx < 0 || idx >= kuku_wifi_saved_count()) return -1;
    WifiManager &m = WifiManager::GetInstance();
    m.StopStation();
    SsidManager::GetInstance().RemoveSsid(idx);
    m.StartStation();
    return 0;
}

void kuku_wifi_restart_station(void) {
    WifiManager &m = WifiManager::GetInstance();
    m.StopStation();
    esp_wifi_stop();
    m.StartStation();
}

// ---- 屏上配网:纯 STA 现场扫描 ----------------------------------------------
// 组件的 WifiStation 有自己的扫描定时器,且 HandleScanResult 会消费扫描结果。
// 扫描任务先 StopStation 独占 radio,再自己阻塞扫描 —— 避免两边抢 records。
struct ScanItem {
    char ssid[33];
    int rssi;
    bool secure;
};
static std::mutex s_scan_mtx;
static std::vector<ScanItem> s_scan_items;
static volatile bool s_scan_done = true;
static volatile bool s_scan_abort = false;
static TaskHandle_t s_scan_task;

static void scan_task(void *) {
    WifiManager::GetInstance().StopStation();
    // radio 已被 StopStation 关掉;以空闲 STA 重启(不装组件扫描定时器)。
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();

    for (int attempt = 0; attempt < 8 && !s_scan_abort; ++attempt) {
        esp_err_t err = esp_wifi_scan_start(nullptr, true);   // 阻塞扫描全信道
        if (err != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        uint16_t n = 0;
        esp_wifi_scan_get_ap_num(&n);
        auto *recs = (wifi_ap_record_t *)malloc(n * sizeof(wifi_ap_record_t));
        if (!recs) break;
        esp_wifi_scan_get_ap_records(&n, recs);

        // 按 SSID 去重(保留最强,reclist 已按 RSSI 降序在 esp_wifi 内部排过?不赌,自己排)。
        std::vector<ScanItem> items;
        for (uint16_t i = 0; i < n; i++) {
            const char *ssid = (const char *)recs[i].ssid;
            if (!ssid[0]) continue;
            bool dup = false;
            for (auto &it : items)
                if (strncmp(it.ssid, ssid, sizeof(it.ssid)) == 0) { dup = true; break; }
            if (dup) continue;
            ScanItem it = {};
            strlcpy(it.ssid, ssid, sizeof(it.ssid));
            it.rssi = recs[i].rssi;
            it.secure = recs[i].authmode != WIFI_AUTH_OPEN;
            items.push_back(it);
        }
        free(recs);
        std::sort(items.begin(), items.end(),
                  [](const ScanItem &a, const ScanItem &b) { return a.rssi > b.rssi; });
        if (items.size() > 16) items.resize(16);
        {
            std::lock_guard<std::mutex> lock(s_scan_mtx);
            s_scan_items = items;
        }
        break;   // 一次成功扫描即收工
    }
    s_scan_done = true;
    s_scan_task = nullptr;
    vTaskDelete(nullptr);
}

void kuku_wifi_scan_start(void) {
    if (s_scan_task) return;                     // 已在扫
    { std::lock_guard<std::mutex> lock(s_scan_mtx); s_scan_items.clear(); }
    s_scan_done = false;
    s_scan_abort = false;
    if (xTaskCreate(scan_task, "kuku_scan", 4096, nullptr, 4, &s_scan_task) != pdPASS) {
        s_scan_task = nullptr;
        s_scan_done = true;
    }
}

bool kuku_wifi_scan_done(void) { return s_scan_done; }

void kuku_wifi_scan_cancel(void) { s_scan_abort = true; }

int kuku_wifi_scan_count(void) {
    std::lock_guard<std::mutex> lock(s_scan_mtx);
    return (int)s_scan_items.size();
}

bool kuku_wifi_scan_get(int idx, char *ssid, size_t cap, int *rssi, bool *secure) {
    std::lock_guard<std::mutex> lock(s_scan_mtx);
    if (idx < 0 || idx >= (int)s_scan_items.size()) return false;
    const ScanItem &it = s_scan_items[idx];
    strlcpy(ssid, it.ssid, cap);
    if (rssi) *rssi = it.rssi;
    if (secure) *secure = it.secure;
    return true;
}

bool kuku_wifi_is_connected(void) {
    return WifiManager::GetInstance().IsConnected();
}

void kuku_wifi_get_ssid(char *out, size_t cap) {
    std::string s = WifiManager::GetInstance().GetSsid();
    strlcpy(out, s.c_str(), cap);
}

void kuku_wifi_get_ip(char *out, size_t cap) {
    std::string s = WifiManager::GetInstance().GetIpAddress();
    strlcpy(out, s.c_str(), cap);
}

int kuku_wifi_get_rssi(void) {
    return WifiManager::GetInstance().GetRssi();
}
