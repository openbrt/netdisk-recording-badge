#include "kuku_endurance.h"
#include "kuku_endurance_core.h"
#include "kuku_rec_progress.h"
#include "kuku_app.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "driver/usb_serial_jtag.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define URL_CAP 192
#define TOKEN_CAP 65
#define BATCH_N 6
#define QUEUE_N 32
typedef struct { bool on; char url[URL_CAP], token[TOKEN_CAP]; } setup_t;
typedef struct {
    uint64_t ms, captured, written;
    uint32_t boot, run, seq, session, segments, dropped, heap;
    int soc, mv, rssi, free_kb, backlight;
    uint8_t kind, stage, reason, cloud;
    bool host, rec, wifi, playing;
    int64_t utc;
} sample_t;
enum { SAMPLE, ARM, HOST_READY, DETACH, RECONNECT, REC_START, REC_STOP, OFF };
static const char *const kinds[] = { "sample", "test_armed", "host_ready",
    "usb_host_detached", "usb_reconnected", "recording_started", "recording_stopped", "test_off" };
static QueueHandle_t s_setup_queue, s_samples;
static setup_t s_config;
static atomic_bool s_enabled, s_configured;
static atomic_uint_least32_t s_stage, s_run, s_last_ack, s_dropped, s_last_seq;
static atomic_int s_http_error;
static uint32_t s_boot;
static TaskHandle_t s_sender_task, s_sampler_task;
static uint64_t now_ms(void) { return (uint64_t)esp_timer_get_time() / 1000; }

static void publish(unsigned kind, kuku_end_detector_t *d, uint32_t run, uint32_t *seq) {
    kuku_rec_progress_t p;
    kuku_rec_progress_get(&p);
    sample_t s = { .ms=now_ms(), .boot=s_boot, .run=run, .seq=++*seq,
        .session=p.session, .captured=p.captured_bytes, .written=p.written_bytes,
        .segments=p.segments, .reason=p.reason, .kind=kind, .stage=d->stage,
        .host=usb_serial_jtag_is_connected(), .rec=g_kuku.recording,
        .wifi=g_kuku.wifi_up, .playing=g_kuku.playing, .cloud=g_kuku.bd_state,
        .soc=bsp_battery_soc(), .mv=bsp_battery_mv(),
        .backlight=bsp_display_backlight_level(), .rssi=kuku_wifi_get_rssi(),
        .free_kb=(int)kuku_rec_free_kb(), .heap=esp_get_free_heap_size(),
        .utc=(int64_t)time(NULL), .dropped=atomic_load(&s_dropped) };
    atomic_store(&s_last_seq, *seq);
    if (xQueueSend(s_samples, &s, 0) != pdTRUE) atomic_fetch_add(&s_dropped, 1);
}

static void sampler(void *arg) {
    (void)arg;
    uint32_t seq=0, run=0;
    uint64_t next_sample=0;
    bool previous_rec=false;
    kuku_end_detector_t d;
    kuku_end_detector_init(&d, now_ms(), false);
    for (;;) {
        setup_t c;
        if (xQueueReceive(s_setup_queue, &c, pdMS_TO_TICKS(500)) == pdTRUE) {
            if (c.on) {
                // Endpoint becomes immutable for this boot, so old queued runs
                // cannot be sent to another collector or with another secret.
                if (!atomic_load(&s_configured)) {
                    s_config=c;
                    atomic_store(&s_configured, true);
                }
                run=esp_random(); if (!run) run=1;
                atomic_store(&s_run, run);
                kuku_end_detector_init(&d, now_ms(), usb_serial_jtag_is_connected());
                previous_rec=g_kuku.recording;
                atomic_store(&s_enabled, true);
                next_sample=now_ms()+10000;
                publish(ARM, &d, run, &seq);
            } else if (atomic_load(&s_enabled)) {
                publish(OFF, &d, run, &seq);
                atomic_store(&s_enabled, false);
                d.stage=KUKU_END_CLOSED;
            }
        }
        if (!atomic_load(&s_enabled)) continue;
        kuku_end_event_t event=kuku_end_detector_tick(&d, now_ms(), usb_serial_jtag_is_connected());
        atomic_store(&s_stage, d.stage);
        if (event) {
            unsigned kind=event==KUKU_END_HOST_READY ? HOST_READY :
                          event==KUKU_END_USB_DETACHED ? DETACH : RECONNECT;
            publish(kind, &d, run, &seq);
        }
        if (previous_rec != g_kuku.recording) {
            previous_rec=g_kuku.recording;
            publish(previous_rec ? REC_START : REC_STOP, &d, run, &seq);
        }
        if (now_ms() >= next_sample) {
            publish(SAMPLE, &d, run, &seq);
            next_sample=now_ms()+10000;
        }
    }
}

static int format_sample(char *out, size_t cap, const sample_t *s) {
    return snprintf(out, cap,
        "{\"boot\":%"PRIu32",\"run\":%"PRIu32",\"seq\":%"PRIu32
        ",\"ms\":%"PRIu64",\"utc\":%"PRId64",\"event\":\"%s\",\"stage\":%u,"
        "\"host\":%d,\"rec\":%d,\"wifi\":%d,\"playing\":%d,\"cloud\":%u,"
        "\"soc\":%d,\"mv\":%d,\"backlight\":%d,\"rssi\":%d,\"free_kb\":%d,"
        "\"heap\":%"PRIu32",\"session\":%"PRIu32",\"segments\":%"PRIu32
        ",\"captured\":%"PRIu64",\"written\":%"PRIu64",\"reason\":%u,\"dropped\":%"PRIu32"}",
        s->boot,s->run,s->seq,s->ms,s->utc,kinds[s->kind],s->stage,
        s->host,s->rec,s->wifi,s->playing,s->cloud,s->soc,s->mv,s->backlight,
        s->rssi,s->free_kb,s->heap,s->session,s->segments,s->captured,s->written,s->reason,s->dropped);
}

static void sender(void *arg) {
    (void)arg;
    static sample_t pending[BATCH_N];
    static char body[4096];
    unsigned n=0, failures=0;
    uint64_t oldest=0, retry=0;
    bool urgent=false;
    for (;;) {
        sample_t s;
        if (n<BATCH_N && xQueueReceive(s_samples,&s,pdMS_TO_TICKS(500)) == pdTRUE) {
            if (!n) oldest=now_ms();
            pending[n++]=s;
            urgent |= s.kind != SAMPLE;
        } else vTaskDelay(pdMS_TO_TICKS(100));
        if (!n || !atomic_load(&s_configured) || !g_kuku.wifi_up || now_ms()<retry) continue;
        // A Netdisk batch owns the network heap until its TLS clients close.
        // Sampling continues; queued events retain their original timestamps.
        if (g_kuku.bd_state==3) continue;
        if (atomic_load(&s_http_error)) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
        if (!urgent && n<BATCH_N && now_ms()-oldest<60000) continue;
        // ELF identity is binary in esp_app_desc_t; encode without exposing config.
        char elf[65];
        for (unsigned i=0;i<32;i++) snprintf(elf+i*2,3,"%02x",esp_app_get_description()->app_elf_sha256[i]);
        int used=snprintf(body,sizeof(body),"{\"protocol\":1,\"firmware\":\"%s\",\"samples\":[",elf);
        bool fits=used>0;
        for (unsigned i=0;fits && i<n;i++) {
            if (i) body[used++]=',';
            int count=format_sample(body+used,sizeof(body)-(size_t)used,&pending[i]);
            fits=count>0 && (size_t)count<sizeof(body)-(size_t)used-3;
            if (fits) used+=count;
        }
        if (!fits) { atomic_fetch_add(&s_dropped,n); n=0; urgent=false; continue; }
        memcpy(body+used,"]}",3); used+=2;
        esp_http_client_config_t cfg={ .url=s_config.url, .timeout_ms=2500,
            .disable_auto_redirect=true, .crt_bundle_attach=esp_crt_bundle_attach,
            .buffer_size=512, .buffer_size_tx=512 };
        esp_http_client_handle_t c=esp_http_client_init(&cfg);
        bool ok=false;
        int status=0;
        if (c) {
            esp_http_client_set_method(c,HTTP_METHOD_POST);
            esp_http_client_set_header(c,"Content-Type","application/json");
            esp_http_client_set_header(c,"X-Endurance-Token",s_config.token);
            esp_http_client_set_post_field(c,body,used);
            esp_err_t rc=esp_http_client_perform(c);
            status=esp_http_client_get_status_code(c);
            ok=rc==ESP_OK && status==204;
            esp_http_client_cleanup(c);
        }
        if (ok) {
            atomic_store(&s_last_ack,pending[n-1].seq);
            ESP_LOGI("endurance","ACK seq=%"PRIu32" count=%u",pending[n-1].seq,n);
            n=0; failures=0; urgent=false; retry=0;
        } else {
            if (status>=400 && status<500 && status!=408 && status!=429) {
                atomic_store(&s_http_error,status);
                atomic_store(&s_enabled,false);
                ESP_LOGE("endurance","collector rejected configuration/status=%d; reboot to configure",status);
            }
            if (failures<4) ++failures;
            retry=now_ms()+(5000U << failures);
            ESP_LOGW("endurance","collector unavailable; bounded retry");
        }
    }
}

int kuku_endurance_arm(const char *url,const char *token) {
    if (!s_setup_queue || !url || !token || strlen(url)>=URL_CAP ||
        strlen(token)<16 || strlen(token)>=TOKEN_CAP ||
        (strncmp(url,"http://",7) && strncmp(url,"https://",8))) return -1;
    for (const char *p=token;*p;p++)
        if (!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9'))) return -1;
    if (atomic_load(&s_enabled)) return -2;
    if (atomic_load(&s_http_error)) return -4;
    if (atomic_load(&s_configured) && (strcmp(url,s_config.url)||strcmp(token,s_config.token))) return -3;
    setup_t c={.on=true};
    snprintf(c.url,sizeof(c.url),"%s",url); snprintf(c.token,sizeof(c.token),"%s",token);
    return xQueueSend(s_setup_queue,&c,0)==pdTRUE ? 0 : -2;
}
int kuku_endurance_off(void) {
    setup_t c={0};
    return s_setup_queue && xQueueSend(s_setup_queue,&c,0)==pdTRUE ? 0 : -1;
}
void kuku_endurance_status(void) {
    printf("ENDURANCE: configured=%d enabled=%d stage=%"PRIu32" run=%"PRIu32
           " seq=%"PRIu32" ack=%"PRIu32" dropped=%"PRIu32" soc=%d mv=%d host=%d http_error=%d"
           " heap=%lu min_heap=%lu send_stack=%u sample_stack=%u\r\n",
           atomic_load(&s_configured),atomic_load(&s_enabled),atomic_load(&s_stage),atomic_load(&s_run),
           atomic_load(&s_last_seq),atomic_load(&s_last_ack),atomic_load(&s_dropped),
           bsp_battery_soc(),bsp_battery_mv(),usb_serial_jtag_is_connected(),atomic_load(&s_http_error),
           (unsigned long)esp_get_free_heap_size(),(unsigned long)esp_get_minimum_free_heap_size(),
           s_sender_task ? (unsigned)uxTaskGetStackHighWaterMark(s_sender_task) : 0,
           s_sampler_task ? (unsigned)uxTaskGetStackHighWaterMark(s_sampler_task) : 0);
}
void kuku_endurance_start(void) {
    s_boot=esp_random();
    s_setup_queue=xQueueCreate(2,sizeof(setup_t));
    s_samples=xQueueCreate(QUEUE_N,sizeof(sample_t));
    if (!s_setup_queue || !s_samples) goto failed;
    if (xTaskCreate(sender,"end_send",4096,NULL,1,&s_sender_task)!=pdPASS) goto failed;
    if (xTaskCreate(sampler,"end_sample",3072,NULL,2,&s_sampler_task)!=pdPASS) {
        vTaskDelete(s_sender_task); s_sender_task=NULL; goto failed;
    }
    return;
failed:
    if (s_setup_queue) vQueueDelete(s_setup_queue);
    if (s_samples) vQueueDelete(s_samples);
    s_setup_queue=NULL; s_samples=NULL;
    ESP_LOGE("endurance","telemetry unavailable (recording unaffected)");
}
