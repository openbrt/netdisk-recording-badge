// main/kuku_wav.h —— 44 字节 PCM WAV 头构造(纯函数,主机可测)。
#pragma once

#include <stddef.h>
#include <stdint.h>

#define KUKU_WAV_HDR 44

// 填写标准 44 字节 PCM WAV 头:data_bytes 为 PCM 数据字节数(不含头)。
// hdr 长度必须 >= KUKU_WAV_HDR。sample_rate/ch/bits 描述 PCM 格式。
void kuku_wav_fill(uint8_t *hdr, uint32_t sample_rate, uint16_t ch,
                   uint16_t bits, uint32_t data_bytes);

// 由 PCM 数据字节数推算时长(毫秒)。非法参数返回 0。
uint32_t kuku_wav_ms(uint32_t data_bytes, uint32_t sample_rate, uint16_t ch,
                     uint16_t bits);
