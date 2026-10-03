#include "kuku_cloud_path.h"

#include <stdio.h>
#include <string.h>

static bool component_valid(const char *name) {
    if (!name || !name[0] || !strcmp(name, ".") || !strcmp(name, "..")) return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
        if (*p < 32 || *p == 127 || *p == '/' || *p == '\\') return false;
    return true;
}

static bool inside_root(const char *dir) {
    size_t base = sizeof(KUKU_CLOUD_ROOT) - 1;
    if (!dir || strncmp(dir, KUKU_CLOUD_ROOT, base)) return false;
    if (!dir[base]) return true;
    if (dir[base] != '/' || strlen(dir) >= KUKU_CLOUD_PATH_MAX) return false;
    for (const char *p = dir + base + 1; ; ) {
        const char *end = strchr(p, '/');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (!len || (len == 1 && *p == '.') ||
            (len == 2 && p[0] == '.' && p[1] == '.')) return false;
        for (size_t i = 0; i < len; ++i)
            if ((unsigned char)p[i] < 32 || p[i] == 127 || p[i] == '\\') return false;
        if (!end) return true;
        p = end + 1;
    }
}

bool kuku_cloud_child_path(const char *dir, const char *name, char *out, size_t cap) {
    if (!out || !cap) return false;
    out[0] = 0;
    if (!inside_root(dir) || !component_valid(name)) return false;
    int n = snprintf(out, cap, "%s/%s", dir, name);
    if (n > 0 && (size_t)n < cap && n < KUKU_CLOUD_PATH_MAX) return true;
    out[0] = 0;
    return false;
}

bool kuku_cloud_parent_path(const char *dir, char *out, size_t cap) {
    if (!out || !cap) return false;
    out[0] = 0;
    if (!inside_root(dir) || !strcmp(dir, KUKU_CLOUD_ROOT)) return false;
    const char *slash = strrchr(dir, '/');
    size_t len = (size_t)(slash - dir);
    if (len >= cap) return false;
    memcpy(out, dir, len);
    out[len] = 0;
    return true;
}

static int digits(const char *s, size_t n) {
    int result = 0;
    for (size_t i = 0; i < n; ++i) {
        if (s[i] < '0' || s[i] > '9') return -1;
        result = result * 10 + s[i] - '0';
    }
    return result;
}

bool kuku_cloud_recording_path(const char *name, char *out, size_t cap) {
    if (!out || !cap) return false;
    out[0] = 0;
    if (!component_valid(name)) return false;
    size_t len = strlen(name);
    if (len >= 32 || len < 8 || strcmp(name + len - 4, ".WAV")) return false;
    char day[11] = KUKU_CLOUD_UNDATED;
    if (len >= 19 && name[8] == '_') {
        int year = digits(name, 4), month = digits(name + 4, 2), date = digits(name + 6, 2);
        int hour = digits(name + 9, 2), minute = digits(name + 11, 2), second = digits(name + 13, 2);
        static const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
        bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
        bool suffix = len == 19 || (len >= 22 && len <= 23 && name[15] == '_' &&
                      digits(name + 16, len - 20) >= 0);
        if (!suffix || year < 2020 || year > 9999 || month < 1 || month > 12 ||
            date < 1 || date > days[month - 1] + (month == 2 && leap) ||
            hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59)
            return false;
        snprintf(day, sizeof(day), "%.4s-%.2s-%.2s", name, name + 4, name + 6);
    } else {
        if (strncmp(name, "REC", 3) || len > 17) return false;
        for (size_t i = 3; i < len - 4; ++i)
            if (name[i] < '0' || name[i] > '9') return false;
    }
    char dir[KUKU_CLOUD_PATH_MAX];
    return kuku_cloud_child_path(KUKU_CLOUD_ROOT, day, dir, sizeof(dir)) &&
           kuku_cloud_child_path(dir, name, out, cap);
}
