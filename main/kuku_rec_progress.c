#include "kuku_rec_progress.h"
#include <stdatomic.h>

// Independent single-writer counters: only 32-bit atomics are required on C3.
// Sequential consistency prevents torn snapshots across the 4 GiB boundary.
typedef struct { atomic_uint_least32_t sequence, low, high; } counter_t;
static counter_t s_capture, s_written;
static atomic_uint_least32_t s_session, s_segments, s_reason;
static atomic_uint_least32_t s_begin_epoch;
static void store(counter_t *c, uint64_t value) {
    atomic_fetch_add(&c->sequence, 1);
    atomic_store(&c->low, (uint32_t)value);
    atomic_store(&c->high, (uint32_t)(value >> 32));
    atomic_fetch_add(&c->sequence, 1);
}
static uint64_t load(counter_t *c) {
    uint32_t first, last, low, high;
    do {
        first = atomic_load(&c->sequence);
        low = atomic_load(&c->low);
        high = atomic_load(&c->high);
        last = atomic_load(&c->sequence);
    } while ((first & 1) || first != last);
    return ((uint64_t)high << 32) | low;
}
void kuku_rec_progress_begin(void) {
    atomic_fetch_add(&s_begin_epoch, 1);
    store(&s_capture, 0); store(&s_written, 0);
    atomic_store(&s_segments, 0);
    atomic_store(&s_reason, KUKU_REC_RUNNING);
    atomic_fetch_add(&s_session, 1);
    atomic_fetch_add(&s_begin_epoch, 1);
}
void kuku_rec_progress_capture(uint32_t bytes) {
    store(&s_capture, load(&s_capture) + bytes);
}
void kuku_rec_progress_write(uint64_t bytes, uint32_t segments) {
    store(&s_written, bytes); atomic_store(&s_segments, segments);
}
void kuku_rec_progress_end(kuku_rec_reason_t reason) { atomic_store(&s_reason, reason); }
void kuku_rec_progress_get(kuku_rec_progress_t *out) {
    uint32_t before, after;
    do {
    before = atomic_load(&s_begin_epoch);
    out->session = atomic_load(&s_session);
    out->captured_bytes = load(&s_capture);
    out->written_bytes = load(&s_written);
    out->segments = atomic_load(&s_segments);
    out->reason = atomic_load(&s_reason);
    after = atomic_load(&s_begin_epoch);
    } while ((before & 1) || before != after);
}
