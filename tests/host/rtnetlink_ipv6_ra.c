/* Pure production wire encoder regression; lifecycle/ingress is network_owner. */
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <kernel/socket.h>
#include <kernel/errno.h>
#include "../../net/rtnetlink.h"
static const unsigned char *attribute(const struct route_header *h,unsigned fixed,unsigned type,unsigned length)
{
    const unsigned char *p=(const void *)(h+1);
    for(size_t at=fixed;at+4<=h->length-sizeof(*h);) {
        uint16_t n,t;memcpy(&n,p+at,2);memcpy(&t,p+at+2,2);
        assert(n>=4 && at+n<=h->length-sizeof(*h));
        if(t==type){assert(n==length+4);return p+at+4;}
        at+=route_align(n);
    }
    return NULL;
}
static uint32_t word(const unsigned char *p){uint32_t value;assert(p);memcpy(&value,p,4);return value;}
int main(void)
{
    struct kernel_socket_interface s={.index=2,.flags=1,.link_up=1,.hardware_type=1};
    struct kernel_socket_ipv6_address *a=&s.ipv6_addresses[2];
    a->present=1;a->prefix=64;a->address[0]=0x20;a->address[1]=1;a->address[15]=9;
    a->valid=8000;a->preferred=4000;a->created=17;a->updated=29;
    unsigned char data[4096];
    struct {struct route_header h;unsigned char family;} request={{17,22,0x301,19,0},10};
    struct route_output out={.data=data,.capacity=sizeof(data)};
    assert(!route_encode(&out,&request,17,&s,1));
    struct route_header *h=(void *)data;
    assert(h->type==20 && ((unsigned char *)(h+1))[2]==0);
    const unsigned char *cache=attribute(h,8,6,16);
    assert(word(cache)==4000 && word(cache+4)==8000 && word(cache+8)==17 && word(cache+12)==29);
    a->flags=32;out.used=0;assert(!route_encode(&out,&request,17,&s,1));
    assert(((unsigned char *)(h+1))[2]==32); /* deprecated, not permanent */
    request.h.type=26;out.used=0;assert(!route_encode(&out,&request,17,&s,1));
    assert(h->type==24 && ((unsigned char *)(h+1))[1]==128);
    assert(((struct route_header *)(data+route_align(h->length)))->type==3); /* A-only: no /64 */
    s.ipv6_prefixes[0].present=1;s.ipv6_prefixes[0].prefix=57;
    s.ipv6_prefixes[0].address[0]=0x20;s.ipv6_prefixes[0].lifetime=123;
    s.ipv6_routers[0].present=1;s.ipv6_routers[0].preference=3;
    s.ipv6_routers[0].address[0]=0xfe;s.ipv6_routers[0].address[1]=0x80;
    s.ipv6_routers[0].lifetime=456;
    out.used=0;assert(!route_encode(&out,&request,17,&s,1));
    unsigned routes=0;
    for(size_t at=0;at<out.used;) {
        h=(void *)(data+at);unsigned char *info=(void *)(h+1);
        if(h->type==24 && info[5]==9) {
            cache=attribute(h,12,12,32);assert(cache);
            if(info[1]==57) {assert(word(cache+8)==12300);assert(word(attribute(h,12,6,4))==256);}
            else {assert(info[1]==0 && word(cache+8)==45600);assert(*attribute(h,12,20,1)==3);assert(word(attribute(h,12,6,4))==2048);}
            routes++;
        }
        at+=route_align(h->length);
    }
    assert(routes==2);
    s.ipv6_prefixes[0].lifetime=UINT32_MAX;
    s.ipv6_routers[0].lifetime=30000000;
    out.used=0;assert(!route_encode(&out,&request,17,&s,1));
    for(size_t at=0;at<out.used;) {
        h=(void *)(data+at);unsigned char *info=(void *)(h+1);
        if(h->type==24 && info[5]==9)assert(word(attribute(h,12,12,32)+8)==(info[1]?0:2147483647));
        at+=route_align(h->length);
    }
    out.used=0;out.capacity=16;assert(route_encode(&out,&request,17,&s,1)==-KERNEL_ENOBUFS);
    puts("rtnetlink IPv6 RA encoder passed");
}
