#ifndef ESP_PLATFORM
#define _POSIX_C_SOURCE 200809L
#endif

#include "kuku_rec_filename.h"

#include <stdio.h>

#define KUKU_VALID_EPOCH_MIN ((time_t)1600000000)
#define KUKU_CST_OFFSET_SEC  (8 * 60 * 60)

static bool finish_name(int written, char *out, size_t cap) {
    if (written > 0 && (size_t)written < cap) return true;
    if (cap > 0) out[0] = 0;
    return false;
}

bool kuku_rec_filename_build(time_t epoch, uint32_t sequence, unsigned duplicate,
                             char *out, size_t cap) {
    if (!out || cap == 0) return false;

    if (epoch >= KUKU_VALID_EPOCH_MIN) {
        time_t cst = epoch + KUKU_CST_OFFSET_SEC;
        struct tm tm = {0};
        if (gmtime_r(&cst, &tm)) {
            int written;
            if (duplicate == 0) {
                written = snprintf(out, cap, "%04d%02d%02d_%02d%02d%02d.WAV",
                                   tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                                   tm.tm_hour, tm.tm_min, tm.tm_sec);
            } else {
                written = snprintf(out, cap,
                                   "%04d%02d%02d_%02d%02d%02d_%02u.WAV",
                                   tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                                   tm.tm_hour, tm.tm_min, tm.tm_sec, duplicate);
            }
            return finish_name(written, out, cap);
        }
    }

    return finish_name(snprintf(out, cap, "REC%08lu.WAV", (unsigned long)sequence),
                       out, cap);
}
