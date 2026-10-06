// main/kuku_test.c —— 串口测试钩子(自动化验收 + 串口配网入口)。
//
// 通过 USB 串口读取单行命令,支持:
//   REC <sec>        录音 sec 秒后自动停止
//   LS               列出全部录音文件(名称/大小/时长)
//   DUMP <name>      校验 WAV 头并统计采样峰值(判空音)
//   FREE             剩余空间
//   FILENAME         预览下一段录音文件名
//   CLEAN SYNCED     删除已成功同步并改名为 .UPD 的本地文件
//   UI <n>           跳到第 n 页(0主页1菜单2文件3WiFi4扫描5键盘6已存7删除8分享示例9关于10图片)
//                    —— 无手环境验证各页 LVGL 渲染是否 crash
//   WIFI SET <ssid>|<pass>  存凭证并立即回连(| 分隔,SSID/密码带空格用这种;
//                           无 | 时按第一个空格分)。大小写敏感。
//   WIFI SCAN / LIST / DEL <n> / STA / INFO
// 命令行以 \n 结尾,命令词大小写不敏感(参数保持原样)。产品按键逻辑不受影响。
#include "kuku_app.h"
#include "kuku_endurance.h"
#include "kuku_console_line.h"
#include "kuku_rec_progress.h"

#include "bsp_display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

static const char *TAG = "kuku_test";

static void cmd_ls(void) {
    int n = kuku_rec_count();
    printf("LS: %d file(s), freeKB=%ld\r\n", n, kuku_rec_free_kb());
    for (int i = 0; i < n; i++) {
        char name[KUKU_MAX_NAME];
        if (!kuku_rec_name_copy(i, name, sizeof(name))) continue;
        printf("  [%d] %s  %lu ms\r\n", i, name,
               (unsigned long)kuku_rec_file_ms(name));
    }
}

static void cmd_dump(const char *name) {
    if (!name || !name[0]) { printf("DUMP: missing name\r\n"); return; }
    char path[280];
    snprintf(path, sizeof(path), "/rec/%s", name);
    FILE *f = fopen(path, "rb");
    if (!f) { printf("DUMP: cannot open %s\r\n", name); return; }

    uint8_t hdr[KUKU_WAV_HEADER_BYTES];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        printf("DUMP: short header\r\n"); fclose(f); return;
    }
    printf("DUMP %s hdr:", name);
    for (int i = 0; i < (int)sizeof(hdr); i++) printf(" %02X", hdr[i]);
    printf("\r\n");

    // RIFF/WAVE 粗校验
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        printf("DUMP: BAD RIFF/WAVE magic\r\n"); fclose(f); return;
    }
    uint32_t sr = hdr[24] | (hdr[25] << 8) | ((uint32_t)hdr[26] << 16) | ((uint32_t)hdr[27] << 24);
    uint16_t bits = hdr[34] | (hdr[35] << 8);
    uint16_t ch = hdr[22] | (hdr[23] << 8);
    printf("DUMP: sample_rate=%lu bits=%u ch=%u\r\n", (unsigned long)sr, bits, ch);

    // 采样统计:峰值与 RMS(判空音)
    int16_t buf[512];
    size_t samples = 0, nonzero = 0;
    int32_t peak = 0; double sum_sq = 0.0;
    size_t r, chunks = 0;
    while ((r = fread(buf, 2, 512, f)) > 0) {
        for (size_t i = 0; i < r; i++) {
            samples++;
            int32_t v = buf[i];
            if (v != 0) nonzero++;
            int32_t a = v < 0 ? -v : v;
            if (a > peak) peak = a;
            sum_sq += (double)v * v;
        }
        // Large recordings can take longer than the task watchdog period to
        // scan.  Let the idle task run regularly while retaining exact stats.
        if ((++chunks & 31U) == 0) vTaskDelay(pdMS_TO_TICKS(1));
    }
    fclose(f);
    double rms = samples ? sum_sq / samples : 0;
    // sqrt 手算(避免拉 libm 依赖)
    double lo = 0, hi = 32768;
    for (int i = 0; i < 40; i++) { double m = (lo + hi) / 2; if (m * m < rms) lo = m; else hi = m; }
    printf("DUMP: samples=%lu nonzero=%lu peak=%ld rms=%.1f -> %s\r\n",
           (unsigned long)samples, (unsigned long)nonzero, (long)peak,
           (lo + hi) / 2, peak > 300 ? "HAS SIGNAL" : "SILENT?");
}

static void cmd_rec(int sec) {
    if (sec < 1) sec = 5;
    if (sec > KUKU_TEST_REC_MAX_SECONDS) sec = KUKU_TEST_REC_MAX_SECONDS;
    printf("REC: checking cloud before %d s recording ...\r\n", sec);
    int rc = kuku_baidu_request_recording();
    if (rc != 0) { printf("REC: blocked rc=%d\r\n", rc); return; }
    for (int i = 0; i < 225 && !g_kuku.recording; i++)
        vTaskDelay(pdMS_TO_TICKS(200));
    if (!g_kuku.recording) { printf("REC: cloud check failed, no recording\r\n"); return; }
    for (int i = 0; i < sec; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        printf("REC: %d/%d s bytes=%lu\r\n", i + 1, sec,
               (unsigned long)g_kuku.rec_bytes);
    }
    char name[KUKU_MAX_NAME] = {0};
    bool has_name = kuku_rec_current_name(name, sizeof(name));  // 停止前取,s_rec_file 停后清空
    kuku_rec_stop();
    vTaskDelay(pdMS_TO_TICKS(800));  // 等落盘收尾
    if (has_name)
        printf("REC: stopped, file=%s\r\n", name);
    else
        printf("REC: stopped (no file name)\r\n");
}

static void cmd_wifi(const char *arg) {
    // arg 指向原始行(未大写化),SSID/密码大小写敏感;子命令用 strncasecmp。
    if (strncasecmp(arg, "SET", 3) == 0) {
        const char *p = arg + 3;
        while (*p == ' ') p++;
        char ssid[36], pass[68];
        const char *sep = strchr(p, '|');          // '|' 优先:SSID 或密码带空格
        if (sep) {
            size_t n = (size_t)(sep - p);
            if (n >= sizeof(ssid)) n = sizeof(ssid) - 1;
            memcpy(ssid, p, n);
            ssid[n] = '\0';
            strlcpy(pass, sep + 1, sizeof(pass));
        } else {                                    // 无 '|':第一个空格分
            if (sscanf(p, "%35s", ssid) != 1) { ssid[0] = 0; }
            const char *sp = p + strlen(ssid);
            while (*sp == ' ') sp++;
            strlcpy(pass, sp, sizeof(pass));
        }
        if (!ssid[0] || !pass[0]) {
            printf("WIFI: usage SET <ssid>|<pass>\r\n");
            return;
        }
        printf("WIFI: saving '%s' & restarting STA\r\n", ssid);
        if (kuku_wifi_add(ssid, pass) == 0)
            printf("WIFI: saved, connecting...\r\n");
        else
            printf("WIFI: SET fail (empty arg?)\r\n");
    } else if (strncasecmp(arg, "LIST", 4) == 0) {
        int n = kuku_wifi_saved_count();
        printf("WIFI: %d saved\r\n", n);
        char ssid[36];
        for (int i = 0; i < n; i++)
            if (kuku_wifi_saved_get(i, ssid, sizeof(ssid)) == 0)
                printf("  [%d] %s\r\n", i, ssid);
    } else if (strncasecmp(arg, "DEL", 3) == 0) {
        int idx = atoi(arg + 3);
        if (idx < 0 || idx >= kuku_wifi_saved_count()) {
            printf("WIFI: bad index (0..%d)\r\n", kuku_wifi_saved_count() - 1);
            return;
        }
        char ssid[36];
        kuku_wifi_saved_get(idx, ssid, sizeof(ssid));
        if (kuku_wifi_del(idx) == 0)
            printf("WIFI: deleted [%d] %s\r\n", idx, ssid);
        else
            printf("WIFI: delete failed\r\n");
    } else if (strncasecmp(arg, "SCAN", 4) == 0 || strncasecmp(arg, "RESULTS", 7) == 0) {
        bool cached = strncasecmp(arg, "RESULTS", 7) == 0;
        if (!cached) {
        printf("WIFI: scanning...\r\n");
        kuku_wifi_scan_start();
        while (!kuku_wifi_scan_done()) vTaskDelay(pdMS_TO_TICKS(500));
        }
        int n = kuku_wifi_scan_count();
        printf("WIFI: %d network(s)\r\n", n);
        char ssid[33]; int rssi; bool secure;
        for (int i = 0; i < n; i++) {
            kuku_wifi_scan_get(i, ssid, sizeof(ssid), &rssi, &secure);
            printf("  [%d] %s %s %d dBm\r\n", i, ssid,
                   secure ? "*" : "OPEN", rssi);
        }
    } else if (strncasecmp(arg, "STA", 3) == 0) {
        printf("WIFI: restarting station\r\n");
        kuku_wifi_restart_station();
    } else if (strncasecmp(arg, "INFO", 4) == 0 || arg[0] == 0) {
        char ssid[34], ip[16];
        wifi_mode_t mode = WIFI_MODE_NULL;
        esp_wifi_get_mode(&mode);
        kuku_wifi_get_ssid(ssid, sizeof(ssid));
        kuku_wifi_get_ip(ip, sizeof(ip));
        printf("WIFI: mode=%s connected=%d saved=%d ssid=%s ip=%s rssi=%d\r\n",
               mode == WIFI_MODE_STA ? "STA" : "OTHER",
               kuku_wifi_is_connected(), kuku_wifi_saved_count(),
               ssid, ip, kuku_wifi_get_rssi());
    } else {
        printf("WIFI: subs: SET s|p / LIST / DEL n / STA / INFO\r\n");
    }
}

static void cmd_baidu(const char *arg) {
    if (strncmp(arg, "AUTH", 4) == 0) {
        int rc = kuku_baidu_auth_start();
        if (rc == 1) printf("BAIDU: already authorized\r\n");
        else if (rc == -2) printf("BAIDU: no wifi, connect first\r\n");
        else if (rc != 0) printf("BAIDU: auth start fail rc=%d\r\n", rc);
        else printf("BAIDU: auth task started\r\n");
    } else if (strncmp(arg, "UPLOAD", 6) == 0) {
        int rc = kuku_baidu_upload_pass();
        printf("BAIDU: upload pass rc=%d\r\n", rc);
    } else if (strncmp(arg, "LIST", 4) == 0) {
        if (strncmp(arg + 4, " STATUS", 7) != 0)
            printf("BAIDU: list request rc=%d\r\n", kuku_baidu_list_request(0));
        int page = 0, count = 0, status = 0;
        bool more = false;
        kuku_baidu_list_status(&page, &count, &status, &more);
        printf("BAIDU: list page=%d count=%d status=%d more=%d\r\n",
               page, count, status, more);
        char dir[KUKU_CLOUD_PATH_MAX];
        kuku_baidu_list_dir(dir, sizeof(dir));
        printf("BAIDU: dir=%s\r\n", dir);
        for (int i = 0; i < count; ++i) {
            kuku_cloud_file_t file;
            if (kuku_baidu_list_get(i, &file))
                printf("BAIDU: entry[%d] dir=%d %s\r\n", i, file.is_dir, file.name);
        }
    } else if (strncmp(arg, "IMAGES", 6) == 0) {
        int count = 0;
        kuku_baidu_list_status(NULL, &count, NULL, NULL);
        for (int i = 0; i < count; i++) {
            kuku_cloud_file_t file;
            if (kuku_baidu_list_get(i, &file) && kuku_baidu_is_jpeg(file.name))
                printf("BAIDU: image[%d] %s %llu B\r\n", i, file.name,
                       (unsigned long long)file.size);
        }
    } else if (strncmp(arg, "IMAGE", 5) == 0) {
        const char *p = arg + 5;
        while (*p == ' ') p++;
        if (!strncmp(p, "STATUS", 6)) {
            printf("BAIDU: image status=%d\r\n", kuku_baidu_image_status());
        } else {
            kuku_cloud_file_t file;
            int index = atoi(p);
            int rc = kuku_baidu_list_get(index, &file) ?
                kuku_baidu_image_request(&file) : -1;
            printf("BAIDU: image request index=%d rc=%d\r\n", index, rc);
        }
    } else if (strncmp(arg, "STATUS", 6) == 0 || arg[0] == 0) {
        char url[64], code[16];
        kuku_baidu_get_auth(url, sizeof(url), code, sizeof(code));
        char name[KUKU_MAX_NAME];
        int done = 0, total = 0;
        kuku_baidu_get_progress(name, sizeof(name), &done, &total);
        printf("BAIDU: state=%u wifi=%d code=%s url=%s last=%s %d/%d\r\n",
               g_kuku.bd_state, g_kuku.wifi_up, code, url, name, done, total);
    } else {
        printf("BAIDU: unknown sub '%s'\r\n", arg);
    }
}

static void cmd_stats(void) {
    kuku_rec_progress_t progress;
    kuku_rec_progress_get(&progress);
    printf("STATS: captured=%"PRIu64" written=%"PRIu64" session=%"PRIu32
           " segments=%"PRIu32" reason=%u heap=%u min=%u largest=%u\r\n",
           progress.captured_bytes, progress.written_bytes, progress.session,
           progress.segments, progress.reason, (unsigned)esp_get_free_heap_size(),
           (unsigned)esp_get_minimum_free_heap_size(),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    const esp_app_desc_t *app=esp_app_get_description();
    printf("IDENTITY: elf=");
    for (unsigned i=0;i<32;i++) printf("%02x",app->app_elf_sha256[i]);
    printf("\r\n");
    kuku_upload_diag_t upload;
    kuku_baidu_get_upload_diag(&upload);
    printf("UPLOAD: attempts=%"PRIu32" completed=%"PRIu32" failures=%"PRIu32
           " last_rc=%d last_ms=%"PRIu32" max_ms=%"PRIu32
           " phase=%u phase_ms=%"PRIu32" failure_rc=%d failure_phase=%u failure_ms=%"PRIu32"\r\n",
           upload.attempts, upload.completed, upload.failures, upload.last_rc,
           upload.last_ms, upload.max_ms, upload.phase, upload.phase_ms,
           upload.failure_rc, upload.failure_phase, upload.failure_ms);
    TaskStatus_t tasks[24];
    char names[24][configMAX_TASK_NAME_LEN];
    // Copy names while task deletion is suspended; snapshot name pointers may
    // otherwise outlive a short-lived upload/capture task's control block.
    vTaskSuspendAll();
    UBaseType_t count=uxTaskGetSystemState(tasks,24,NULL);
    for (UBaseType_t i=0;i<count;i++) strlcpy(names[i],tasks[i].pcTaskName,sizeof(names[i]));
    xTaskResumeAll();
    for (UBaseType_t i=0;i<count;i++)
        printf("STACK: %s free_min=%u\r\n",names[i],
               (unsigned)tasks[i].usStackHighWaterMark);
}

static void test_task(void *arg) {
    (void)arg;
    kuku_console_line_t input={0};
    char up[KUKU_CONSOLE_LINE_CAP];
    const char *line=input.text;
    printf("kuku-test: ready (REC<n>/LS/DUMP<name>/FREE/FILENAME/CLEAN SYNCED/"
           "WIFI<SET s|p|SCAN|LIST|DEL n|STA|INFO>/BAIDU<AUTH|UPLOAD|STATUS|LIST>)\r\n");
    for (;;) {
        int byte=fgetc(stdin);
        if (byte==EOF) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        kuku_line_result_t result=kuku_console_line_feed(&input,(unsigned char)byte);
        if (result==KUKU_LINE_REJECTED) printf("CONSOLE: invalid line discarded\r\n");
        if (result!=KUKU_LINE_READY) continue;
        // 大写副本只用于命令词匹配;WIFI SET 的 SSID/密码取原始 line(大小写敏感)。
        strlcpy(up, line, sizeof(up));
        for (char *p = up; *p; p++) *p = (char)toupper((unsigned char)*p);
        if (strncmp(up, "ENDURANCE", 9) == 0) {
            if (strncmp(up + 9, " ARM ", 5) == 0) {
                char url[192], token[65], extra[2];
                int n=sscanf(line+14,"%191s %64s %1s",url,token,extra);
                printf("ENDURANCE: arm rc=%d\r\n",n==2 ? kuku_endurance_arm(url,token) : -1);
            } else if (!strcmp(up+9," OFF")) {
                printf("ENDURANCE: off rc=%d\r\n",kuku_endurance_off());
            } else if (!strcmp(up+9," STATUS") || !up[9]) kuku_endurance_status();
            else printf("ENDURANCE: ARM <url> <token> / OFF / STATUS\r\n");
        } else if (strcmp(up, "STATS") == 0) {
            cmd_stats();
        } else if (strcmp(up, "STATE") == 0) {
            printf("STATE: page=%d rec=%d play=%d off=%d wifi=%d bd=%u bytes=%lu ms=%lu\r\n",
                   kuku_test_page(), g_kuku.recording, g_kuku.playing,
                   g_kuku.screen_off, g_kuku.wifi_up, g_kuku.bd_state,
                   (unsigned long)g_kuku.rec_bytes, (unsigned long)g_kuku.rec_ms);
        } else if (strncmp(up, "KEY ", 4) == 0) {
            char button[12], event[12] = "CLICK";
            int n = sscanf(up + 4, "%11s %11s", button, event);
            int b = n > 0 && strcmp(button, "UP") == 0 ? 0 :
                    n > 0 && strcmp(button, "OK") == 0 ? 1 :
                    n > 0 && strcmp(button, "DOWN") == 0 ? 2 :
                    n > 0 && strcmp(button, "CHORD") == 0 ? 3 : -1;
            if (b < 0 || (strcmp(event, "CLICK") && strcmp(event, "LONG")))
                printf("KEY: usage UP|OK|DOWN|CHORD [CLICK|LONG]\r\n");
            else printf("KEY: queued rc=%d\r\n", kuku_test_key(b, !strcmp(event, "LONG")));
        } else if (strncmp(up, "REC", 3) == 0) {
            cmd_rec(atoi(up + 3));
        } else if (strcmp(up, "LS") == 0) {
            cmd_ls();
        } else if (strncmp(up, "DUMP", 4) == 0) {
            const char *nm = line + 4;
            while (*nm == ' ') nm++;
            cmd_dump(nm);
        } else if (strncmp(up, "UI", 2) == 0) {
            int pg = atoi(up + 2);
            if (pg < 0 || pg > 10) {
                printf("UI: 0..10 (0主页1菜单2文件3WiFi4扫描5键盘6已存7删除8分享示例9关于10图片)\r\n");
            } else {
                if (bsp_lvgl_lock(500)) {
                    kuku_ui_goto((kuku_page_t)pg);
                    bsp_lvgl_unlock();
                    printf("UI: goto %d ok\r\n", pg);
                } else {
                    printf("UI: lvgl lock timeout\r\n");
                }
            }
        } else if (strncmp(up, "WIFI", 4) == 0) {
            const char *a = line + 4;              // 原始大小写
            while (*a == ' ') a++;
            cmd_wifi(a);
        } else if (strncmp(up, "BAIDU", 5) == 0) {
            const char *a = up + 5;
            while (*a == ' ') a++;
            cmd_baidu(a);
        } else if (strcmp(up, "FILENAME") == 0) {
            char name[KUKU_MAX_NAME] = {0};
            if (kuku_rec_preview_name(name, sizeof(name)))
                printf("FILENAME: %s\r\n", name);
            else
                printf("FILENAME: unavailable\r\n");
        } else if (strcmp(up, "CLEAN SYNCED") == 0) {
            int removed = kuku_rec_clean_synced();
            printf("CLEAN: synced=%d freeKB=%ld\r\n", removed, kuku_rec_free_kb());
        } else if (strcmp(up, "FREE") == 0) {
            printf("FREE: %ld KB\r\n", kuku_rec_free_kb());
        } else if (line[0]) {
            printf("kuku-test: unknown '%s'\r\n", line);
        }
    }
}

void kuku_test_start(void) {
    if (xTaskCreate(test_task, "kuku_test", 7168, NULL, 3, NULL) != pdPASS)
        ESP_LOGE(TAG, "test task create failed");
}
