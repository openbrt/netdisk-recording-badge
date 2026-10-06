#include "kuku_endurance_core.h"
void kuku_end_detector_init(kuku_end_detector_t *d, uint64_t now_ms, bool host) {
    *d = (kuku_end_detector_t){ .stage = KUKU_END_WAIT_HOST,
                              .previous_host = host, .edge_ms = now_ms };
}
kuku_end_event_t kuku_end_detector_tick(kuku_end_detector_t *d,
                                      uint64_t now_ms, bool host) {
    if (host != d->previous_host || now_ms < d->edge_ms) {
        d->edge_ms = now_ms;
        d->previous_host = host;
    }
    uint64_t duration = now_ms - d->edge_ms;
    if (d->stage == KUKU_END_WAIT_HOST && host && duration >= 10000) {
        d->stage = KUKU_END_ARMED;
        return KUKU_END_HOST_READY;
    }
    if (d->stage == KUKU_END_ARMED && !host && duration >= 5000) {
        d->stage = KUKU_END_DETACHED;
        return KUKU_END_USB_DETACHED;
    }
    // Stop accepting battery samples as soon as a host is seen again.
    if (d->stage == KUKU_END_DETACHED && host) {
        d->stage = KUKU_END_CLOSED;
        return KUKU_END_USB_RECONNECTED;
    }
    return KUKU_END_NO_EVENT;
}
