#pragma once
#include <stdint.h>
typedef enum { KUKU_REC_RUNNING, KUKU_REC_MANUAL, KUKU_REC_NETWORK,
               KUKU_REC_SPACE, KUKU_REC_CAPTURE, KUKU_REC_STORAGE } kuku_rec_reason_t;
typedef struct {
    uint64_t captured_bytes, written_bytes;
    uint32_t session, segments;
    kuku_rec_reason_t reason;
} kuku_rec_progress_t;
void kuku_rec_progress_begin(void);
void kuku_rec_progress_capture(uint32_t bytes);
void kuku_rec_progress_write(uint64_t bytes, uint32_t segments);
void kuku_rec_progress_end(kuku_rec_reason_t reason);
void kuku_rec_progress_get(kuku_rec_progress_t *out);
