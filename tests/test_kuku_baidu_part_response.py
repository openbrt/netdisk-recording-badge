"""Exercise Baidu superfile2 success parsing and block-MD5 matching."""

from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class BaiduPartResponseTest(unittest.TestCase):
    def test_md5_only_success_response_and_block_lookup(self):
        source = (ROOT / "main/kuku_baidu.c").read_text()
        helpers = source[
            source.index("static int part_response_status("):
            source.index("// ---- 上传")
        ]
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { const char *valuestring; double valuedouble; int kind; } cJSON;
static cJSON root, number, string;
static char md5_value[33];
static cJSON *cJSON_Parse(const char *text) {
 if (!text || text[0] != '{') return NULL;
 root.valuestring = text;
 return &root;
}
static const cJSON *cJSON_GetObjectItem(const cJSON *item, const char *key) {
 const char *text = item->valuestring;
 if (!strcmp(key, "errno")) {
  const char *p = strstr(text, "\"errno\":");
  if (!p) return NULL;
  number = (cJSON){ .valuedouble = strtol(p + 8, NULL, 10), .kind = 1 };
  return &number;
 }
 if (!strcmp(key, "md5")) {
  const char *p = strstr(text, "\"md5\":\"");
  if (!p) return NULL;
  p += 7;
  const char *end = strchr(p, '"');
  size_t n = end ? (size_t)(end - p) : 0;
  if (n >= sizeof(md5_value)) n = sizeof(md5_value) - 1;
  memcpy(md5_value, p, n); md5_value[n] = 0;
  string = (cJSON){ .valuestring = md5_value, .kind = 2 };
  return &string;
 }
 return NULL;
}
static bool cJSON_IsNumber(const cJSON *item) { return item && item->kind == 1; }
static bool cJSON_IsString(const cJSON *item) { return item && item->kind == 2; }
static void cJSON_Delete(cJSON *item) { (void)item; }
static size_t test_strlcpy(char *dst, const char *src, size_t cap) {
 size_t n = strlen(src);
 if (cap) { size_t copy = n < cap - 1 ? n : cap - 1; memcpy(dst, src, copy); dst[copy] = 0; }
 return n;
}
#ifdef strlcpy
#undef strlcpy
#endif
#define strlcpy test_strlcpy
'''
        checks = r'''
int main(void) {
 char md5[33];
 const char *a = "9501ac997bfa9ca58bf4cb11ac0075d9";
 const char *b = "0e04ff8c524bb2a4c2ba8b048ae5d9cf";
 assert(part_response_status("not-json", md5) == -101);
 assert(part_response_status("{\"errno\":111}", md5) == 111);
 assert(part_response_status("{\"md5\":\"short\"}", md5) == -3);
 assert(part_response_status("{\"md5\":\"9501ac997bfa9ca58bf4cb11ac0075d9\",\"request_id\":1}", md5) == 0);
 assert(!strcmp(md5, a));
 assert(part_response_status("{\"errno\":0,\"md5\":\"0e04ff8c524bb2a4c2ba8b048ae5d9cf\"}", md5) == 0);
 assert(!strcmp(md5, b));
 char json[80], found[33];
 snprintf(json, sizeof(json), "[\"%s\",\"%s\"]", a, b);
 assert(block_md5_at(json, 0, found) && !strcmp(found, a));
 assert(block_md5_at(json, 1, found) && !strcmp(found, b));
 assert(!block_md5_at(json, 2, found));
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            test = path / "test.c"
            test.write_text(harness + helpers + checks)
            exe = path / "test"
            subprocess.run(
                ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(test), "-o", str(exe)],
                check=True,
            )
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    unittest.main()
