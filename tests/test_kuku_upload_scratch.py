"""Execute the production upload phases over a real file, with HTTP fixtures."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class UploadScratchTest(unittest.TestCase):
    def test_upload_phases_and_failures(self):
        source = (ROOT / 'main/kuku_baidu.c').read_text()
        worker = source[source.index('static int upload_file('):source.index('// 上传一轮:')]
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define KUKU_WAV_HEADER_BYTES 44
#define KUKU_BD_BLOCK (4*1024*1024)
#define HTTP_METHOD_POST 1
#define TAG "test"
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
static const char s_access[]="test-token";
static unsigned phase, marked, mode, diag_phase;
static void upload_diag_phase(unsigned value) { diag_phase=value; }
typedef struct { const char *valuestring; int valuedouble; unsigned type; } cJSON;
static cJSON root, upid={"upload-id",0,1}, rtype={NULL,1,2}, error={NULL,0,2};
#ifdef strlcpy
#undef strlcpy
#endif
#define strlcpy test_strlcpy
static size_t test_strlcpy(char *out,const char *s,size_t cap) {
 size_t n=strlen(s); if(cap) snprintf(out,cap,"%s",s); return n;
}
static bool kuku_cloud_recording_path(const char *name,char *out,size_t cap) {
 return snprintf(out,cap,"/apps/test/2026-10-05/%s",name)<(int)cap;
}
static void url_encode(const char *in,char *out,size_t cap) { assert(strlen(in)<cap); strcpy(out,in); }
static int md5_block_list(const char *path,long size,char *out,size_t cap) {
 assert(diag_phase==2);
 assert(strstr(path,"test.WAV") && size==12345 && cap>=37);
 strcpy(out,"[\"0123456789abcdef0123456789abcdef\"]"); return 0;
}
static int http_req(const char *url,int method,const char *body,int len,
 const char *ctype,char *resp,size_t cap,int timeout) {
 assert(method==1 && len==(int)strlen(body) && !strcmp(ctype,"application/x-www-form-urlencoded"));
 assert(timeout==15000 && cap==1024 && strstr(body,"size=12345"));
 if(phase==0) { assert(diag_phase==3); assert(strstr(url,"method=precreate") && strstr(body,"autoinit=1")); phase=1; }
 else { assert(diag_phase==5); assert(strstr(url,"method=create") && strstr(body,"uploadid=upload-id")); phase=3; }
 assert(strstr(body,"block_list=[\"0123456789abcdef0123456789abcdef\"]"));
 if(mode==1 && phase==1) return -1;
 strcpy(resp,phase==1?"precreate":"create"); return 0;
}
static cJSON *cJSON_Parse(const char *s) { assert(!strcmp(s,"precreate") || !strcmp(s,"create")); return &root; }
static const cJSON *cJSON_GetObjectItem(cJSON *r,const char *key) {
 assert(r==&root); if(!strcmp(key,"errno")) return &error;
 if(!strcmp(key,"uploadid")) return &upid;
 if(!strcmp(key,"return_type")) { rtype.valuedouble=mode==2?2:1; return &rtype; }
 return NULL;
}
static bool cJSON_IsString(const cJSON *v) { return v && v->type==1; }
static bool cJSON_IsNumber(const cJSON *v) { return v && v->type==2; }
static void cJSON_Delete(cJSON *r) { assert(r==&root); }
static int http_upload_part(const char *url,FILE *f,long size,char *resp,size_t cap) {
 assert(diag_phase==4);
 assert(phase==1 && size==12345 && cap==512 && strstr(url,"partseq=0"));
 unsigned char bytes[12345]; assert(fread(bytes,1,sizeof(bytes),f)==sizeof(bytes));
 for(unsigned i=0;i<sizeof(bytes);i++) assert(bytes[i]==i%251);
 strcpy(resp,"part"); phase=2; return 0;
}
static int part_response_status(const char *s,char md5[33]) {
 assert(!strcmp(s,"part")); strcpy(md5,mode==3?"bad":"0123456789abcdef0123456789abcdef"); return 0;
}
static bool block_md5_at(const char *s,int index,char out[33]) {
 assert(index==0 && strlen(s)==36); strcpy(out,"0123456789abcdef0123456789abcdef"); return true;
}
static int kuku_rec_mark_uploaded(const char *name) { assert(diag_phase==6 && !strcmp(name,"test.WAV") && phase==3); marked++; return 0; }
'''
        checks = r'''
int main(void) {
 FILE *f=fopen("/rec/test.WAV","wb"); assert(f);
 for(unsigned i=0;i<12345;i++) fputc(i%251,f);
 assert(!fclose(f));
 for(mode=0;mode<4;mode++) {
  phase=marked=0; int rc=upload_file("test.WAV");
  if(mode==1) assert(rc==-1 && phase==1 && !marked);
  else if(mode==3) assert(rc==-3 && phase==2 && !marked);
  else assert(rc==0 && phase==3 && marked==1);
 }
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory); (path/'rec').mkdir()
            # Replace only the mount prefix so the real FILE operations stay private.
            (path/'test.c').write_text(harness+worker.replace('"/rec/%s"','"rec/%s"')+checks.replace('"/rec/test.WAV"','"rec/test.WAV"'))
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(path/'test.c'),'-o',str(path/'test')],check=True)
            subprocess.run([str(path/'test')],check=True,cwd=path)

if __name__=='__main__': unittest.main()
