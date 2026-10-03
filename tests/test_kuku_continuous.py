"""Run the production rotating WAV writer over real files and compare every PCM byte."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ContinuousTest(unittest.TestCase):
    def test_rotation_continuity_network_loss_and_storage_stop(self):
        source=(ROOT/'main/kuku_rec.c').read_text()
        writer=source[source.index('static bool rec_segment_open('):source.index('\nint kuku_rec_start(')]
        harness=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include "kuku_wav.h"
#include "kuku_rec_filename.h"
#include "kuku_cloud_path.h"
#define REC_MOUNT_POINT "rec"
#define KUKU_MAX_NAME 32
#define KUKU_REC_CHUNK_BYTES 4096
#define KUKU_SAMPLE_RATE_HZ 16000
#define KUKU_REC_SEGMENT_SECONDS 60
#define CAPTURE_TASK_STACK 3072
#define REC_BUFFER_BYTES 8192
#define pdMS_TO_TICKS(ms) (ms)
#define pdPASS 1
#define portMAX_DELAY 0
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define TAG "test"
static struct { bool recording, wifi_up; uint32_t rec_bytes, rec_ms; } g_kuku;
static bool s_capture_done, s_capture_failed, s_rec_stop_req;
static uint32_t s_capture_bytes;
static int s_rec_result;
static void *s_rec_task, *s_pcm_stream, *s_catalog_mutex;
static char s_rec_file[32], status[100], names[10][32];
static unsigned index_name, notifications, deleted, mode;
static size_t received, desired, remaining;
static time_t test_epoch = 1790956770; // 2026-10-02 23:59:30 Beijing
static time_t fake_time(time_t *out) { if(out) *out=test_epoch; return test_epoch; }
#define time fake_time
#ifdef strlcpy
#undef strlcpy
#endif
#define strlcpy test_strlcpy
static size_t test_strlcpy(char *d,const char *s,size_t cap) {
 size_t len=strlen(s); if(cap) snprintf(d,cap,"%s",s); return len;
}
static bool choose_rec_name_at(char *out,size_t cap,bool consume,time_t epoch) {
 assert(consume); ++index_name;
 if(mode==2 && index_name==2) return false;
 bool ok=kuku_rec_filename_build(epoch,index_name,0,out,cap);
 if(ok) strcpy(names[index_name-1],out);
 return ok;
}
static void xSemaphoreTake(void *p,int t) { (void)p;(void)t; }
static void xSemaphoreGive(void *p) { (void)p; }
static void cache_invalidate(void) {}
static void capture_task(void *p) { (void)p; }
static int xTaskCreate(void (*fn)(void *),const char *name,int stack,void *arg,int pri,void *handle) {
 assert(fn==capture_task && stack==3072 && pri==12 && !arg && !handle); (void)name;
 s_capture_done=true; s_capture_bytes=(uint32_t)desired; return pdPASS;
}
static unsigned char pattern(size_t offset) { return (unsigned char)((offset/7 + offset)%251); }
static size_t xStreamBufferBytesAvailable(void *p) { (void)p; return remaining; }
static size_t xStreamBufferReceive(void *p,void *buf,size_t cap,int wait) {
 (void)p; assert(wait==100);
 if(mode==1 && s_rec_stop_req && remaining>REC_BUFFER_BYTES) remaining=REC_BUFFER_BYTES;
 size_t got=remaining<cap ? remaining:cap;
 for(size_t i=0;i<got;i++) ((unsigned char *)buf)[i]=pattern(received+i);
 received+=got; remaining-=got; return got;
}
static void vStreamBufferDelete(void *p) { (void)p;++deleted; }
static void vTaskDelay(int ms) { assert(ms>0); }
static void vTaskDelete(void *p) { assert(!p); }
static long kuku_rec_free_kb(void) { return mode==1 && received>=102400 ? 70:4096; }
static void kuku_baidu_on_recording_saved(void) { assert(g_kuku.recording); ++notifications; }
static void kuku_ui_set_status(const char *s) { strcpy(status,s); }
'''
        checks=r'''
static void reset(unsigned selected,size_t bytes) {
 assert(!mkdir("rec",0700));
 mode=selected; desired=remaining=bytes; received=0;
 index_name=notifications=deleted=0; status[0]=0;
 g_kuku.recording=true; g_kuku.wifi_up=true; // Writer rotates without ending an online session.
 g_kuku.rec_ms=(uint32_t)(bytes/32); s_rec_task=(void *)1;
 s_rec_stop_req=s_capture_done=s_capture_failed=false;
}
static size_t verify_files(void) {
 size_t offset=0;
 for(unsigned i=0;i<notifications;i++) {
  char path[80];snprintf(path,sizeof(path),"rec/%s",names[i]);
  FILE *f=fopen(path,"rb"); assert(f);
  uint8_t h[44];assert(fread(h,1,44,f)==44 && !memcmp(h,"RIFF",4));
  uint32_t data=h[40]|((uint32_t)h[41]<<8)|((uint32_t)h[42]<<16)|((uint32_t)h[43]<<24);
  assert(data<=1920000 && data>0 && !(data%2));
  if(i+1<notifications) assert(data==1920000);
  for(uint32_t n=0;n<data;n++) assert(fgetc(f)==pattern(offset++));
  assert(fgetc(f)==EOF);fclose(f);
 }
 return offset;
}
int main(void) {
 reset(0,16000*2*137); rec_task(NULL);
 assert(notifications==3 && deleted==1 && !g_kuku.recording && s_rec_result==0);
 assert(g_kuku.rec_bytes==desired && verify_files()==desired);
 assert(!strcmp(names[0],"20261002_235930.WAV"));
 assert(!strcmp(names[1],"20261003_000030.WAV"));
 assert(!strcmp(names[2],"20261003_000130.WAV"));
 char cloud[256];assert(kuku_cloud_recording_path(names[0],cloud,sizeof(cloud)));
 assert(strstr(cloud,"/2026-10-02/"));
 assert(kuku_cloud_recording_path(names[1],cloud,sizeof(cloud)) && strstr(cloud,"/2026-10-03/"));
 assert(!rename("rec","long-result"));
 reset(1,16000*2*137); rec_task(NULL);
 assert(s_rec_result==0 && notifications==1 && received<=102400+8192);
 assert(verify_files()==received && strstr(status,"暂存空间不足"));
 assert(!rename("rec","space-result"));
 reset(2,16000*2*137); rec_task(NULL);
 assert(s_rec_result==-1 && notifications==1 && !g_kuku.recording);
 assert(verify_files()==1920000 && strstr(status,"录音未完成"));
 puts("Continuous WAV rotation: byte continuity, midnight routing, network outage, storage stop and failure preservation PASS");
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp);(p/'test.c').write_text(harness+writer+checks)
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(ROOT/'main'),str(p/'test.c'),str(ROOT/'main/kuku_wav.c'),str(ROOT/'main/kuku_rec_filename.c'),str(ROOT/'main/kuku_cloud_path.c'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],cwd=p,check=True)


if __name__=='__main__':
    unittest.main()
