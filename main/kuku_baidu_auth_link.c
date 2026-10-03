#include "kuku_baidu_auth_link.h"
#include <stdio.h>

bool kuku_baidu_auth_link(const char *code, char *url, size_t cap) {
    if (!url || !cap) return false;
    url[0] = 0;
    if (!code || !code[0]) return false;
    size_t n = 0;
    for (; n < 16 && code[n]; ++n) {
        unsigned char c = (unsigned char)code[n];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9'))) return false;
    }
    if (n == 16) return false;
    int written = snprintf(url, cap,
        "https://openapi.baidu.com/device?display=page&code=%s", code);
    if (written < 0 || (size_t)written >= cap) {
        url[0] = 0;
        return false;
    }
    return true;
}
