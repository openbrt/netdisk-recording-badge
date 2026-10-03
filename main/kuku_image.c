#include "../managed_components/lvgl__lvgl/src/libs/tjpgd/tjpgd.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// LVGL's tiled JPEG decoder does not reliably transform each MCU tile. Render
// once into a screen-sized BMP so the display decoder can read whole scanlines.
#define IMAGE_EDGE 232
#define JPEG_POOL_BYTES 4096
#define MAX_MCU_ROWS 16

typedef struct {
    FILE *src;
    FILE *dst;
    uint8_t *stripe;
    int width;
    int height;
    int pitch;
    int src_width;
    int src_height;
    int stripe_top;
    int stripe_y_first;
    int stripe_y_last;
    bool io_error;
} image_render_t;

static void put16(uint8_t *p, unsigned v) {
    p[0] = v & 255;
    p[1] = (v >> 8) & 255;
}

static void put32(uint8_t *p, unsigned v) {
    put16(p, v);
    put16(p + 2, v >> 16);
}

static size_t jpeg_input(JDEC *jd, uint8_t *buf, size_t count) {
    image_render_t *r = jd->device;
    if (buf) return fread(buf, 1, count, r->src);
    return fseek(r->src, (long)count, SEEK_CUR) == 0 ? count : 0;
}

static bool flush_stripe(image_render_t *r) {
    if (r->stripe_top < 0) return true;
    for (int y = r->stripe_y_first; y <= r->stripe_y_last; y++) {
        long offset = 54L + (long)(r->height - 1 - y) * r->pitch;
        if (fseek(r->dst, offset, SEEK_SET) != 0 ||
            fwrite(r->stripe + (size_t)(y - r->stripe_y_first) * r->pitch,
                   1, r->pitch, r->dst) != (size_t)r->pitch) {
            r->io_error = true;
            return false;
        }
    }
    return true;
}

static int jpeg_output(JDEC *jd, void *pixels, JRECT *rect) {
    image_render_t *r = jd->device;
    if (r->stripe_top != rect->top) {
        if (!flush_stripe(r)) return 0;
        r->stripe_top = rect->top;
        r->stripe_y_first = -1;
        r->stripe_y_last = -1;
        for (int y = 0; y < r->height; y++) {
            int source_y = (int)(((int64_t)(2 * y + 1) * r->src_height) /
                                 (2 * r->height));
            if (source_y >= rect->top && source_y <= rect->bottom) {
                if (r->stripe_y_first < 0) r->stripe_y_first = y;
                r->stripe_y_last = y;
            }
        }
        if (r->stripe_y_first >= 0) {
            if (r->stripe_y_last - r->stripe_y_first >= MAX_MCU_ROWS) return 0;
            memset(r->stripe, 0, (size_t)(r->stripe_y_last - r->stripe_y_first + 1) * r->pitch);
        }
    }
    if (r->stripe_y_first < 0) return 1;
    int rect_width = rect->right - rect->left + 1;
    const uint8_t *src = pixels; // TJpgDec's RGB888 output is B, G, R.
    for (int y = r->stripe_y_first; y <= r->stripe_y_last; y++) {
        int source_y = (int)(((int64_t)(2 * y + 1) * r->src_height) /
                             (2 * r->height));
        for (int x = 0; x < r->width; x++) {
            int source_x = (int)(((int64_t)(2 * x + 1) * r->src_width) /
                                 (2 * r->width));
            if (source_x < rect->left || source_x > rect->right) continue;
            size_t src_offset = ((size_t)(source_y - rect->top) * rect_width +
                                 source_x - rect->left) * 3;
            memcpy(r->stripe + (size_t)(y - r->stripe_y_first) * r->pitch + x * 3,
                   src + src_offset, 3);
        }
    }
    return 1;
}

int kuku_image_render_bmp(const char *jpeg_path, const char *bmp_path, int edge) {
    if (!jpeg_path || !bmp_path || edge < 1 || edge > IMAGE_EDGE) return -1;
    image_render_t r = {.stripe_top = -1};
    r.src = fopen(jpeg_path, "rb");
    if (!r.src) return -1;
    uint8_t *pool = malloc(JPEG_POOL_BYTES);
    JDEC *jd = malloc(sizeof(*jd));
    int result = -1;
    if (!pool || !jd || jd_prepare(jd, jpeg_input, pool, JPEG_POOL_BYTES, &r) != JDR_OK)
        goto done;
    r.src_width = jd->width;
    r.src_height = jd->height;
    if (!r.src_width || !r.src_height || r.src_width > 4096 || r.src_height > 4096)
        goto done;
    if (r.src_width >= r.src_height) {
        r.width = r.src_width < edge ? r.src_width : edge;
        r.height = (int)((int64_t)r.src_height * r.width / r.src_width);
    } else {
        r.height = r.src_height < edge ? r.src_height : edge;
        r.width = (int)((int64_t)r.src_width * r.height / r.src_height);
    }
    if (r.width < 1) r.width = 1;
    if (r.height < 1) r.height = 1;
    r.pitch = (r.width * 3 + 3) & ~3;
    r.stripe = malloc((size_t)MAX_MCU_ROWS * r.pitch);
    r.dst = r.stripe ? fopen(bmp_path, "wb+") : NULL;
    if (!r.dst) goto done;
    uint8_t header[54] = {0};
    header[0] = 'B'; header[1] = 'M';
    put32(header + 2, 54 + r.pitch * r.height);
    put32(header + 10, 54);
    put32(header + 14, 40);
    put32(header + 18, r.width);
    put32(header + 22, r.height);
    put16(header + 26, 1);
    put16(header + 28, 24);
    put32(header + 34, r.pitch * r.height);
    if (fwrite(header, 1, sizeof(header), r.dst) != sizeof(header)) goto done;
    if (jd_decomp(jd, jpeg_output, 0) != JDR_OK || !flush_stripe(&r) || r.io_error)
        goto done;
    if (fflush(r.dst) != 0) goto done;
    result = 0;
done:
    if (r.dst) fclose(r.dst);
    if (r.src) fclose(r.src);
    free(r.stripe);
    free(jd);
    free(pool);
    if (result != 0) remove(bmp_path);
    return result;
}
