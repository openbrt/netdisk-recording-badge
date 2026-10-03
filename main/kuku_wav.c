// main/kuku_wav.c —— 44 字节 PCM WAV 头构造。小端逐字节写入,不依赖打包结构体。
#include "kuku_wav.h"

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
}

void kuku_wav_fill(uint8_t *hdr, uint32_t sample_rate, uint16_t ch,
                   uint16_t bits, uint32_t data_bytes) {
    if (!hdr || sample_rate == 0 || ch == 0 || bits == 0) return;

    const uint16_t block_align = (uint16_t)(ch * (bits / 8));
    const uint32_t byte_rate = sample_rate * block_align;

    hdr[0] = 'R'; hdr[1] = 'I'; hdr[2] = 'F'; hdr[3] = 'F';
    put_u32(hdr + 4, 36 + data_bytes);                 // RIFF 块大小
    hdr[8] = 'W'; hdr[9] = 'A'; hdr[10] = 'V'; hdr[11] = 'E';

    hdr[12] = 'f'; hdr[13] = 'm'; hdr[14] = 't'; hdr[15] = ' ';
    put_u32(hdr + 16, 16);                             // fmt 块大小
    put_u16(hdr + 20, 1);                              // PCM
    put_u16(hdr + 22, ch);
    put_u32(hdr + 24, sample_rate);
    put_u32(hdr + 28, byte_rate);
    put_u16(hdr + 32, block_align);
    put_u16(hdr + 34, bits);

    hdr[36] = 'd'; hdr[37] = 'a'; hdr[38] = 't'; hdr[39] = 'a';
    put_u32(hdr + 40, data_bytes);
}

uint32_t kuku_wav_ms(uint32_t data_bytes, uint32_t sample_rate, uint16_t ch,
                     uint16_t bits) {
    if (sample_rate == 0 || ch == 0 || bits < 8) return 0;
    const uint64_t bytes_per_ms =
        (uint64_t)sample_rate * ch * (bits / 8) / 1000;
    if (bytes_per_ms == 0) return 0;
    return (uint32_t)((uint64_t)data_bytes / bytes_per_ms);
}
