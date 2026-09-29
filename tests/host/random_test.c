#include <kernel/random.h>
#include <kernel/blake2s.h>
#include <kernel/errno.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
int main(void) {
    unsigned char seed[32], first[64], next[64], predicted[64];
    unsigned char nonce[12];
    unsigned char zero_key[32]={0}, zero_nonce[12]={0}, initial[64], second[64];
    kernel_random_chacha20_block(zero_key,zero_nonce,0,initial);
    kernel_random_chacha20_block(initial,zero_nonce,0,second);
    assert(!kernel_random_available() && !kernel_random_ready());
    assert(kernel_random_fill(first,64)==KERNEL_RANDOM_STATUS_OK);
    assert(kernel_random_fill(next,64)==KERNEL_RANDOM_STATUS_OK);
    assert(memcmp(first,next,64)!=0);
    assert(!memcmp(first,initial+32,32));
    assert(!memcmp(next,second+32,32));
    kernel_random_chacha20_block(first,zero_nonce,0,predicted);
    assert(memcmp(predicted+32,next,32)!=0);
    for(unsigned i=0;i<32;i++) seed[i]=(unsigned char)i;
    for(unsigned i=0;i<12;i++) nonce[i]=seed[(i<4?0:i<8?12:20)+(i%4)] ^
        (unsigned char)((i<4?0xa5a5a5a5u:i<8?0x3c6ef372u:0x9e3779b9u)>>(8*(i%4)));
    assert(kernel_random_initialize(seed,32)==KERNEL_RANDOM_STATUS_OK);
    assert(kernel_random_fill(first,64)==KERNEL_RANDOM_STATUS_OK);
    assert(kernel_random_fill(next,64)==KERNEL_RANDOM_STATUS_OK);
    kernel_random_chacha20_block(first,nonce,2,predicted);
    assert(memcmp(predicted,next,64)!=0 && "public output must not disclose next key");
    assert(!kernel_random_ready());
    assert(kernel_random_wait_ready(1) == -KERNEL_EAGAIN);
    assert(kernel_random_wait_ready(0) == -KERNEL_EINTR);
    for(unsigned i=0;i<100;i++) kernel_random_mix(seed,32,0);
    assert(!kernel_random_ready());
    kernel_random_mix(seed,31,1); assert(!kernel_random_ready());
    kernel_random_mix(seed,1,1); assert(kernel_random_ready());
    kernel_random_mix(seed,32,0); assert(kernel_random_ready());
    assert(kernel_random_initialize(0,0)==KERNEL_RANDOM_STATUS_UNAVAILABLE);
    assert(kernel_random_ready());
    static const unsigned char abc[32] = {
        0x50,0x8c,0x5e,0x8c,0x32,0x7c,0x14,0xe2,0xe1,0xa7,0x2b,0xa3,0x4e,0xeb,0x45,0x2f,
        0x37,0x45,0x8b,0x20,0x9e,0xd6,0x3a,0x29,0x4d,0x99,0x9b,0x4c,0x86,0x67,0x59,0x82};
    kernel_blake2s("abc",3,next); assert(!memcmp(abc,next,32));
    puts("random FKE, BLAKE2s vector, credit and wait tests passed");
}
