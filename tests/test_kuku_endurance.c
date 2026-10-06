#include "kuku_endurance_core.h"
#include "kuku_rec_progress.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    kuku_end_detector_t d;
    kuku_end_detector_init(&d,0,false);
    assert(kuku_end_detector_tick(&d,20000,false)==KUKU_END_NO_EVENT);
    assert(d.stage==KUKU_END_WAIT_HOST); // Battery boot never fabricates unplug.
    kuku_end_detector_tick(&d,21000,true);
    assert(kuku_end_detector_tick(&d,30999,true)==KUKU_END_NO_EVENT);
    assert(kuku_end_detector_tick(&d,31000,true)==KUKU_END_HOST_READY);
    kuku_end_detector_tick(&d,32000,false);
    kuku_end_detector_tick(&d,36000,true); // Short interruptions bounce.
    kuku_end_detector_tick(&d,37000,false);
    assert(kuku_end_detector_tick(&d,41999,false)==KUKU_END_NO_EVENT);
    assert(kuku_end_detector_tick(&d,42000,false)==KUKU_END_USB_DETACHED);
    assert(kuku_end_detector_tick(&d,90000,false)==KUKU_END_NO_EVENT);
    assert(kuku_end_detector_tick(&d,90001,true)==KUKU_END_USB_RECONNECTED);
    assert(kuku_end_detector_tick(&d,100000,false)==KUKU_END_NO_EVENT);
    kuku_end_detector_init(&d,100000,true); // A new arm starts a fresh debounce.
    assert(kuku_end_detector_tick(&d,110000,true)==KUKU_END_HOST_READY);
    kuku_rec_progress_begin();
    kuku_rec_progress_capture(UINT32_MAX);
    kuku_rec_progress_capture(4096);
    kuku_rec_progress_write(4608000000ULL,2400);
    kuku_rec_progress_end(KUKU_REC_SPACE);
    kuku_rec_progress_t p;
    kuku_rec_progress_get(&p);
    assert(p.captured_bytes==4294971391ULL && p.written_bytes==4608000000ULL);
    assert(p.session==1 && p.segments==2400 && p.reason==KUKU_REC_SPACE);
    kuku_rec_progress_begin(); kuku_rec_progress_get(&p);
    assert(p.session==2 && !p.captured_bytes && !p.written_bytes && !p.segments && !p.reason);
    puts("Endurance detector and 64-bit PCM progress: PASS");
}
