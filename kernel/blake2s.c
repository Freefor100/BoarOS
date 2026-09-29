// SPDX-License-Identifier: GPL-2.0 OR MIT
/* Adapted from Linux lib/crypto/blake2s.c, commit
 * f4cdf7ca9a1fdcca413157df19753f388a5a224e.
 * Copyright (C) 2015-2019 Jason A. Donenfeld <Jason@zx2c4.com>.
 * All Rights Reserved. Portable unkeyed BLAKE2s-256 interface for BoarOS.
 */
#include <kernel/blake2s.h>
#include <kernel/random.h>
#include <string.h>
static const uint8_t blake2s_sigma[10][16] = {
	{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
	{ 14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3 },
	{ 11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4 },
	{ 7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8 },
	{ 9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13 },
	{ 2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9 },
	{ 12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11 },
	{ 13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10 },
	{ 6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5 },
	{ 10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0 },
};


static uint32_t ror(uint32_t x, unsigned n) { return (x >> n) | (x << (32-n)); }
static uint32_t load(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
void kernel_blake2s(const void *data, size_t size, uint8_t output[32])
{
    static const uint32_t iv[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                 0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    uint32_t h[8], v[16], m[16];
    uint8_t block[64];
    uint64_t count = 0;
    const uint8_t *p = data;
    memcpy(h, iv, sizeof(h)); h[0] ^= 0x01010020U;
    do {
        size_t n = size > 64 ? 64 : size;
        memset(block, 0, sizeof(block));
        if (n) memcpy(block, p, n);
        count += n;
        for (unsigned i=0;i<16;i++) m[i] = load(block + i*4);
        memcpy(v,h,32); memcpy(v+8,iv,32);
        v[12] ^= (uint32_t)count; v[13] ^= (uint32_t)(count>>32);
        if (size <= 64) v[14] = ~v[14];
#define G(r,i,a,b,c,d) do { \
 a += b + m[blake2s_sigma[r][2*i]]; d=ror(d^a,16); \
 c += d; b=ror(b^c,12); a += b + m[blake2s_sigma[r][2*i+1]]; \
 d=ror(d^a,8); c+=d; b=ror(b^c,7); } while(0)
        for(unsigned r=0;r<10;r++) {
            G(r,0,v[0],v[4],v[8],v[12]); G(r,1,v[1],v[5],v[9],v[13]);
            G(r,2,v[2],v[6],v[10],v[14]); G(r,3,v[3],v[7],v[11],v[15]);
            G(r,4,v[0],v[5],v[10],v[15]); G(r,5,v[1],v[6],v[11],v[12]);
            G(r,6,v[2],v[7],v[8],v[13]); G(r,7,v[3],v[4],v[9],v[14]);
        }
#undef G
        for(unsigned i=0;i<8;i++) h[i] ^= v[i] ^ v[i+8];
        if (p) p += n;
        size -= n;
    } while(size);
    for(unsigned i=0;i<32;i++) output[i]=(uint8_t)(h[i/4]>>(8*(i%4)));
    kernel_random_erase(h,sizeof(h)); kernel_random_erase(v,sizeof(v));
    kernel_random_erase(m,sizeof(m)); kernel_random_erase(block,sizeof(block));
}
