// tests/test_kuku_wav.c —— 主机测试:WAV 头字段与时长换算。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "kuku_wav.h"

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

int main(void) {
    uint8_t h[KUKU_WAV_HDR];

    // 16k/mono/16bit,一分钟数据。
    const uint32_t data = 16000UL * 2 * 60;
    kuku_wav_fill(h, 16000, 1, 16, data);

    assert(memcmp(h, "RIFF", 4) == 0);
    assert(memcmp(h + 8, "WAVE", 4) == 0);
    assert(memcmp(h + 12, "fmt ", 4) == 0);
    assert(memcmp(h + 36, "data", 4) == 0);
    assert(rd32(h + 4) == 36 + data);
    assert(rd32(h + 16) == 16);       // fmt 块长
    assert(rd16(h + 20) == 1);        // PCM
    assert(rd16(h + 22) == 1);        // 单声道
    assert(rd32(h + 24) == 16000);
    assert(rd32(h + 28) == 32000);    // byte rate
    assert(rd16(h + 32) == 2);        // block align
    assert(rd16(h + 34) == 16);
    assert(rd32(h + 40) == data);

    // 空数据。
    kuku_wav_fill(h, 16000, 1, 16, 0);
    assert(rd32(h + 4) == 36);
    assert(rd32(h + 40) == 0);

    // 时长换算:16000*2 B/s → 32000 B = 1000 ms。
    assert(kuku_wav_ms(32000, 16000, 1, 16) == 1000);
    assert(kuku_wav_ms(1600, 16000, 1, 16) == 50);
    assert(kuku_wav_ms(0, 16000, 1, 16) == 0);
    assert(kuku_wav_ms(32000, 0, 1, 16) == 0);      // 非法采样率
    assert(kuku_wav_ms(32000, 16000, 0, 16) == 0);  // 非法声道

    printf("test_kuku_wav: PASS\n");
    return 0;
}
