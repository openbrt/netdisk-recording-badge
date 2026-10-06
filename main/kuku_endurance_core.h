#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { KUKU_END_WAIT_HOST, KUKU_END_ARMED, KUKU_END_DETACHED,
               KUKU_END_CLOSED } kuku_end_stage_t;
typedef enum { KUKU_END_NO_EVENT, KUKU_END_HOST_READY,
               KUKU_END_USB_DETACHED, KUKU_END_USB_RECONNECTED } kuku_end_event_t;
typedef struct {
    kuku_end_stage_t stage;
    bool previous_host;
    uint64_t edge_ms;
} kuku_end_detector_t;
void kuku_end_detector_init(kuku_end_detector_t *d, uint64_t now_ms, bool host);
kuku_end_event_t kuku_end_detector_tick(kuku_end_detector_t *d,
                                      uint64_t now_ms, bool host);
