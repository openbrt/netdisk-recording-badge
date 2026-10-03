"""Render production file-list updates into widget stubs to catch ghost rows."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class FileListTest(unittest.TestCase):
    def test_saved_network_short_lists_and_selection(self):
        source = (ROOT / 'main/kuku_ui.c').read_text()
        refresh = source[source.index('void kuku_ui_saved_refresh(void)'):source.index('void kuku_ui_saved_move(int delta)')]
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#define SAVED_ROWS 6
#define KUKU_PAGE_WIFI_SAVED 6
#define LV_OBJ_FLAG_HIDDEN 1
enum { COLOR_ACCENT2, COLOR_PANEL, COLOR_TEXT, COLOR_DIM };
typedef struct { bool hidden; int color; char text[160]; } lv_obj_t;
static lv_obj_t rows[SAVED_ROWS], names[SAVED_ROWS], state;
static lv_obj_t *s_saved_row[SAVED_ROWS], *s_saved_name[SAVED_ROWS];
static lv_obj_t *s_saved_state=&state;
static int s_page=KUKU_PAGE_WIFI_SAVED, s_saved_sel, total_networks;
static int lv_color_hex(int c) { return c; }
static void lv_obj_set_style_text_color(lv_obj_t *o,int c,int s) { (void)o;(void)c;(void)s; }
static void lv_obj_set_style_bg_color(lv_obj_t *o,int c,int s) { o->color=c;(void)s; }
static void lv_obj_add_flag(lv_obj_t *o,int flag) { assert(flag==1);o->hidden=true; }
static void lv_obj_remove_flag(lv_obj_t *o,int flag) { assert(flag==1);o->hidden=false; }
static void lv_label_set_text(lv_obj_t *o,const char *text) { snprintf(o->text,sizeof(o->text),"%s",text); }
static void lv_label_set_text_fmt(lv_obj_t *o,const char *fmt,...) {
 va_list ap;va_start(ap,fmt);vsnprintf(o->text,sizeof(o->text),fmt,ap);va_end(ap);
}
static int kuku_wifi_saved_count(void) { return total_networks; }
static void kuku_wifi_saved_get(int idx,char *out,size_t cap) {
 assert(idx>=0 && idx<total_networks);
 snprintf(out,cap,"network-%d",idx);
}
'''
        checks = r'''
static void check_render(int total,int selected) {
 total_networks=total;s_saved_sel=selected;kuku_ui_saved_refresh();
 int visible=0,active=0;
 char expected[33];snprintf(expected,sizeof(expected),"network-%d",s_saved_sel);
 for(int r=0;r<SAVED_ROWS;r++) if(!rows[r].hidden) {
  visible++;assert(names[r].text[0]);
  if(rows[r].color==COLOR_ACCENT2) { active++;assert(!strcmp(names[r].text,expected)); }
 }
 assert(visible==(total<SAVED_ROWS?total:SAVED_ROWS));
 assert(active==(total?1:0));
 if(total>0 && total<=SAVED_ROWS) {
  assert(!rows[0].hidden && !strcmp(names[0].text,"network-0"));
  for(int r=total;r<SAVED_ROWS;r++) assert(rows[r].hidden);
 }
}
int main(void) {
 for(int r=0;r<SAVED_ROWS;r++) { s_saved_row[r]=&rows[r];s_saved_name[r]=&names[r]; }
 for(int total=0;total<=10;total++)
  for(int sel=0;sel<(total?total:1);sel++) check_render(total,sel);
 /* Refresh after the remembered list shrinks must clamp and hide old rows. */
 check_render(10,9);check_render(2,9);assert(s_saved_sel==1);
 check_render(0,1);assert(s_saved_sel==0);
 puts("Production saved networks: empty, short, scrolling and shrinking lists PASS");
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            cfile = Path(tmp) / 'saved_networks.c'
            binary = Path(tmp) / 'saved_networks'
            cfile.write_text(harness + refresh + checks)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            str(cfile), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_successful_browse_stays_stable_and_errors_retry(self):
        source = (ROOT / 'main/kuku_ui.c').read_text()
        timer = source[source.index('static void ui_timer_cb(lv_timer_t *t)'):source.index('// 供 main.c 在主页建好后启动定时器')]
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>
typedef int lv_timer_t;
enum { KUKU_PAGE_HOME, KUKU_PAGE_BD_RESET, KUKU_PAGE_WIFI,
       KUKU_PAGE_WIFI_SCAN, KUKU_PAGE_IMAGE, KUKU_PAGE_FILES };
enum { COLOR_ACCENT, COLOR_GREEN };
static int s_page=KUKU_PAGE_FILES, s_home_status, fake_status=2, fake_page=0;
static uint32_t s_toast_until_ms, s_list_next_request_ms=60000;
static int requests, refreshes;
static int selection=5;
static bool s_files_list_mode=true;
static char s_toast[1];
static struct { int bd_state, rec_ms; bool playing, recording, wifi_up; } g_kuku={.bd_state=2, .wifi_up=true};
static int64_t clock_us;
static int64_t esp_timer_get_time(void) { return clock_us; }
static void status_bar_refresh(void) {}
static int lv_color_hex(int x) { return x; }
static void lv_obj_set_style_text_color(int o,int c,int p) { (void)o;(void)c;(void)p; }
static void lv_label_set_text(int o,const char *s) { (void)o;(void)s; }
static void lv_label_set_text_fmt(int o,const char *s,...) { (void)o;(void)s; }
static void reset_refresh(void) {}
static void kuku_ui_wifi_refresh(void) {}
static void scan_refresh(void) {}
static void kuku_ui_image_refresh(void) {}
static void kuku_ui_goto(int page) { (void)page; }
static void kuku_baidu_auth_start(void) {}
static void kuku_ui_baidu_refresh(void) {}
static void kuku_baidu_list_status(int *page,int *count,int *status,bool *more) {
 if(page) *page=fake_page;
 if(count) *count=10;
 if(status) *status=fake_status;
 if(more) *more=true;
}
static int kuku_baidu_list_request(int page) {
 assert(page>=0);requests++;fake_status=1;selection=0;return 0;
}
static void kuku_ui_files_refresh(void) { refreshes++; }
'''
        checks = r'''
int main(void) {
 /* A minute's polling of the real timer must not clear a loaded selection. */
 for(int ms=0;ms<=70000;ms+=500) {
  clock_us=(int64_t)ms*1000;ui_timer_cb(0);
  assert(requests==0 && selection==5 && fake_status==2);
 }
 assert(refreshes==141);
 /* An unread page is requested immediately; an in-flight read is not duplicated. */
 fake_status=0;clock_us=71000000;ui_timer_cb(0);assert(requests==1);
 ui_timer_cb(0);assert(requests==1);
 /* Failed reads retain their 15s retry backoff on the current page. */
 fake_status=-1;fake_page=3;s_list_next_request_ms=86000;
 clock_us=85000000;ui_timer_cb(0);assert(requests==1);
 clock_us=86000000;ui_timer_cb(0);assert(requests==2 && s_list_next_request_ms==101000);
 /* An upload owns the network: no new list request while it is active. */
 fake_status=-1;g_kuku.bd_state=3;clock_us=102000000;
 ui_timer_cb(0);assert(requests==2);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            cfile = Path(tmp) / 'list_timer.c'
            binary = Path(tmp) / 'list_timer'
            cfile.write_text(harness + timer + checks)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            str(cfile), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_short_and_scrolling_lists(self):
        source = (ROOT / 'main/kuku_ui.c').read_text()
        refresh = source[source.index('void kuku_ui_files_refresh(void)'):source.index('int kuku_ui_files_selected(')]
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#define FILES_ROWS 6
#define KUKU_MAX_NAME 32
#define KUKU_PAGE_FILES 2
#define LV_OBJ_FLAG_HIDDEN 1
#define COLOR_ACCENT 1
#define COLOR_DIM 2
#define COLOR_ACCENT2 3
#define COLOR_PANEL 4
#define COLOR_TEXT 5
typedef struct { bool hidden; char text[160]; } lv_obj_t;
typedef struct { char name[96]; bool is_dir; time_t mtime; } kuku_cloud_file_t;
static lv_obj_t rows[FILES_ROWS], names[FILES_ROWS], info[FILES_ROWS], state;
static lv_obj_t *s_files_row[FILES_ROWS], *s_files_name[FILES_ROWS], *s_files_info[FILES_ROWS];
static lv_obj_t *s_files_state=&state;
static bool s_files_list_mode=true;
static int s_page=KUKU_PAGE_FILES, s_files_sel, locals, clouds;
static struct { bool wifi_up; int bd_state; } g_kuku={true,2};
static int lv_color_hex(int c) { return c; }
static void lv_obj_set_style_text_color(lv_obj_t *o,int c,int selector) { (void)o;(void)c;(void)selector; }
static void lv_obj_set_style_bg_color(lv_obj_t *o,int c,int selector) { (void)o;(void)c;(void)selector; }
static void lv_obj_add_flag(lv_obj_t *o,int flag) { assert(flag==1);o->hidden=true; }
static void lv_obj_remove_flag(lv_obj_t *o,int flag) { assert(flag==1);o->hidden=false; }
static void lv_label_set_text(lv_obj_t *o,const char *text) { snprintf(o->text,sizeof(o->text),"%s",text); }
static void lv_label_set_text_fmt(lv_obj_t *o,const char *fmt,...) {
 va_list ap;va_start(ap,fmt);vsnprintf(o->text,sizeof(o->text),fmt,ap);va_end(ap);
}
static void kuku_baidu_list_status(int *page,int *count,int *status,bool *more) {
 *page=0;*count=clouds;*status=2;*more=false;
}
static int pending_count(void) { return locals; }
static int files_total(void) { return locals+clouds; }
static bool pending_at(int idx,char *out,size_t cap) {
 assert(idx>=0 && idx<locals);
 snprintf(out,cap,"local-%d.WAV",idx);return true;
}
static void kuku_baidu_get_progress(char *name,size_t cap,int *done,int *total) {
 assert(cap>0);*name=0;*done=0;*total=0;
}
static bool kuku_baidu_list_get(int idx,kuku_cloud_file_t *file) {
 assert(idx>=0 && idx<clouds);
 snprintf(file->name,sizeof(file->name),"cloud-%d",idx);file->is_dir=true;return true;
}
'''
        checks = r'''
int main(void) {
 for(int r=0;r<FILES_ROWS;r++) {
  s_files_row[r]=&rows[r];s_files_name[r]=&names[r];s_files_info[r]=&info[r];
 }
 for(int total=0;total<=12;total++) for(int local=0;local<=total && local<=3;local++) {
  locals=local;clouds=total-local;
  for(int selected=0;selected<(total?total:1);selected++) {
   memset(rows,0,sizeof(rows));memset(names,0,sizeof(names));memset(info,0,sizeof(info));
   s_files_sel=selected;kuku_ui_files_refresh();
   int visible=0;
   for(int r=0;r<FILES_ROWS;r++) if(!rows[r].hidden) {
    ++visible;assert(names[r].text[0]);
    assert(!strncmp(names[r].text,"local-",6) || !strncmp(names[r].text,"cloud-",6));
   }
   assert(visible==(total<FILES_ROWS?total:FILES_ROWS));
   if(total==1 && !local) {
    assert(!rows[0].hidden && !strcmp(names[0].text,"cloud-0"));
    for(int r=1;r<FILES_ROWS;r++) assert(rows[r].hidden);
   }
  }
 }
 puts("Production file list: short lists, mixed pending/cloud files and scrolling PASS");
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            cfile = Path(tmp) / 'file_list.c'
            binary = Path(tmp) / 'file_list'
            cfile.write_text(harness + refresh + checks)
            subprocess.run(['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L',
                            '-Wall', '-Wextra', '-Werror', str(cfile), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
