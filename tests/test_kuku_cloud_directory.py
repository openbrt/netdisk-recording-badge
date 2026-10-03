"""Exercise production cloud directory creation against controlled API outcomes."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class DirectoryTest(unittest.TestCase):
    def test_creation_conflicts_and_retry_paths(self):
        source = (ROOT / 'main/kuku_baidu.c').read_text()
        helpers = source[source.index('static int ensure_cloud_dir('):source.index('// 三步上传单个文件')]
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "kuku_cloud_path.h"
#define HTTP_METHOD_GET 0
#define HTTP_METHOD_POST 1
static const char *s_access = "test-token";
typedef struct { int type, valueint; double valuedouble; const char *valuestring; } cJSON;
static cJSON root, err, isdir, path, list;
static int scenario, calls, fail_at;
static char current_path[256];
static void url_encode(const char *s, char *out, size_t cap) {
 assert(strlen(s) < cap); strcpy(out, s);
}
static int http_req(const char *url, int method, const char *body, int len,
 const char *ctype, char *resp, size_t cap, int timeout) {
 ++calls; assert(timeout == 15000 && cap >= 2);
 if (calls == fail_at) return -1;
 err = (cJSON){.type=1}; isdir = (cJSON){.type=1,.valueint=1};
 path = (cJSON){.type=2,.valuestring=current_path}; list = (cJSON){.type=3};
 if (method == HTTP_METHOD_POST) {
  assert(strstr(url, "method=create") && !strstr(url, "method=list"));
  assert(ctype && len == (int)strlen(body));
  assert(strstr(body, "&isdir=1&rtype=0"));
  const char *end = strstr(body, "&isdir=");
  assert(end && !strncmp(body, "path=", 5));
  size_t n = (size_t)(end - body - 5);
  memcpy(current_path, body+5, n); current_path[n]=0;
  if (scenario == 1 || scenario == 2 || scenario == 6 || scenario == 8) err.valuedouble=-8;
  if (scenario == 3) err.valuedouble=111;
  if (scenario == 4) path.valuestring="/unexpected/renamed";
  if (scenario == 7) isdir.valueint=0;
 } else {
  assert(strstr(url, "method=list") && strstr(url, current_path));
  assert(!body && !len && !ctype);
  if (scenario == 2) err.valuedouble=-9;
  if (scenario == 6) err.valuedouble=111;
  // The live service ignores limit without start. A populated directory then
  // exceeds the fixed response buffer and leaves an incomplete JSON document.
  if (scenario == 8 && !strstr(url, "&start=0&limit=1")) {
   memset(resp, '!', cap-1); resp[cap-1]=0; return 0;
  }
 }
 strcpy(resp, scenario == 5 ? "!" : "x"); return 0;
}
static cJSON *cJSON_Parse(const char *s) { return *s=='!' ? NULL : &root; }
static cJSON *cJSON_GetObjectItem(const cJSON *r, const char *key) {
 if (!r) return NULL;
 if (!strcmp(key,"errno")) return &err;
 if (!strcmp(key,"isdir")) return &isdir;
 if (!strcmp(key,"path")) return &path;
 if (!strcmp(key,"list")) return &list;
 return NULL;
}
static bool cJSON_IsNumber(const cJSON *j) { return j && j->type==1; }
static bool cJSON_IsString(const cJSON *j) { return j && j->type==2; }
static bool cJSON_IsArray(const cJSON *j) { return j && j->type==3; }
static void cJSON_Delete(cJSON *j) { (void)j; }
'''
        checks = r'''
int main(void) {
 assert(prepare_recording_directory("20261003_120000.WAV")==0 && calls==2);
 assert(!strcmp(current_path,KUKU_CLOUD_ROOT "/2026-10-03"));
 calls=0; scenario=1;
 assert(prepare_recording_directory("REC0012.WAV")==0 && calls==4);
 assert(!strcmp(current_path,KUKU_CLOUD_ROOT "/undated"));
 calls=0; scenario=2;
 assert(prepare_recording_directory("20261003_120000.WAV")==-1 && calls==2);
 calls=0; scenario=3;
 assert(prepare_recording_directory("20261003_120000.WAV")==-111 && calls==1);
 calls=0; scenario=6;
 assert(prepare_recording_directory("20261003_120000.WAV")==-111 && calls==2);
 for (int s=4;s<=7;s++) {
  if(s==6) continue;
  scenario=s; calls=0;
  assert(prepare_recording_directory("20261003_120000.WAV")==-1 && calls==1);
 }
 scenario=0; calls=0; fail_at=2;
 assert(prepare_recording_directory("20261003_120000.WAV")==-1 && calls==2);
 calls=0; fail_at=0;
 assert(prepare_recording_directory("20261003_120000.WAV")==0 && calls==2);
 calls=0;
 assert(prepare_recording_directory("../escape.WAV")==-1 && calls==0);
 scenario=8; calls=0;
 assert(prepare_recording_directory("20261003_120000.WAV")==0 && calls==4);
 puts("Cloud directory creation, conflicts, expiry and retry: PASS");
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            cfile = Path(tmp) / 'directory.c'
            binary = Path(tmp) / 'directory'
            cfile.write_text(harness + helpers + checks)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-I'+str(ROOT/'main'), str(cfile),
                            str(ROOT/'main/kuku_cloud_path.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
