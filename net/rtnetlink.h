/* Read-only rtnetlink wire encoder. The caller owns a coherent interface
 * snapshot and bounded response storage; no protocol objects escape it. */
#ifndef BOAROS_RTNETLINK_H
#define BOAROS_RTNETLINK_H
struct route_header { uint32_t length; uint16_t type, flags; uint32_t sequence, port; };
struct route_output { unsigned char *data; size_t used, capacity; uint32_t port; };
static size_t route_align(size_t n) { return (n+3U)&~(size_t)3U; }
static void *route_append(struct route_output *out,size_t n)
{
    size_t aligned=route_align(n);
    if(aligned>out->capacity-out->used)return NULL;
    void *p=out->data+out->used;__builtin_memset(p,0,aligned);out->used+=aligned;return p;
}
static struct route_header *route_message(struct route_output *out,uint16_t type,
    uint16_t flags,uint32_t sequence,const void *payload,size_t size)
{
    struct route_header *h=route_append(out,sizeof(*h)+size);if(!h)return NULL;
    *h=(struct route_header){sizeof(*h)+(uint32_t)size,type,flags,sequence,out->port};
    if(size)__builtin_memcpy(h+1,payload,size);
    return h;
}
static int route_attribute(struct route_output *out,struct route_header *h,
    uint16_t type,const void *data,size_t size)
{
    uint16_t *a=route_append(out,size+4);if(!a)return -KERNEL_ENOBUFS;
    a[0]=(uint16_t)(size+4);a[1]=type;__builtin_memcpy(a+2,data,size);
    h->length=(uint32_t)(out->used-((unsigned char *)h-out->data));return 0;
}
static int route_error(struct route_output *out,const struct route_header *request,
    const void *data,size_t size,int error)
{
    size_t echo=error?size:0;
    struct route_header *h=route_message(out,2,0,request->sequence,NULL,0);
    if(!h)return -KERNEL_ENOBUFS;
    unsigned char *p=route_append(out,4+sizeof(*request)+echo);if(!p)return -KERNEL_ENOBUFS;
    __builtin_memcpy(p,&error,4);__builtin_memcpy(p+4,request,sizeof(*request));
    if(echo)__builtin_memcpy(p+4+sizeof(*request),data,echo);
    h->length=(uint32_t)(sizeof(*h)+4+sizeof(*request)+echo);return 0;
}
static int route_link(struct route_output *out,const struct route_header *request,
    const struct kernel_socket_interface *s,int dump)
{
    struct { uint8_t family,pad; uint16_t type; int32_t index; uint32_t flags,change; } info=
        {0,0,s->hardware_type,(int32_t)s->index,s->flags|(s->link_up?0x10000U:0),0};
    struct route_header *h=route_message(out,16,dump?2:0,request->sequence,&info,sizeof(info));
    if(!h)return -KERNEL_ENOBUFS;
    size_t name=0;while(name<sizeof(s->name) && s->name[name])name++;
    if(name==sizeof(s->name))return -KERNEL_EINVAL;
    uint8_t state=(s->flags&8)?0:((s->flags&0x40)?6:2);
    uint8_t broadcast[6];__builtin_memset(broadcast,s->hardware_type==772?0:255,6);
    if(route_attribute(out,h,3,s->name,name+1) || route_attribute(out,h,4,&s->mtu,4) ||
       route_attribute(out,h,1,s->mac,6) || route_attribute(out,h,2,broadcast,6) ||
       route_attribute(out,h,16,&state,1) || route_attribute(out,h,13,(uint32_t[]){0},4) ||
       route_attribute(out,h,6,"noqueue",8))return -KERNEL_ENOBUFS;
    return 0;
}
static unsigned route_prefix(uint32_t mask)
{
    const uint8_t *p=(const uint8_t *)&mask;unsigned bits=0;
    for(unsigned i=0;i<4;i++)for(unsigned b=128;b;b>>=1) {if(!(p[i]&b))return bits;bits++;}
    return bits;
}
static int route_address(struct route_output *out,const struct route_header *request,
    const struct kernel_socket_interface *s,unsigned family,int slot)
{
    if(family==10 && (slot<0?!s->ipv6_present:!s->ipv6_addresses[slot].present))return 0;
    if(family==2 && !s->address)return 0;
    uint8_t address[16]={0};size_t size=family==10?16:4;
    if(family==10)__builtin_memcpy(address,slot<0?s->ipv6_address:s->ipv6_addresses[slot].address,16);else __builtin_memcpy(address,&s->address,4);
    struct { uint8_t family,prefix,flags,scope; uint32_t index; } info=
        {family,family==10?(slot<0?(s->hardware_type==772?128:64):s->ipv6_addresses[slot].prefix):route_prefix(s->netmask),
         family==10?(slot<0?s->ipv6_flags:s->ipv6_addresses[slot].flags):128,s->hardware_type==772?254:(family==10 && slot<0?253:0),s->index};
    struct route_header *h=route_message(out,20,2,request->sequence,&info,sizeof(info));
    if(!h)return -KERNEL_ENOBUFS;
    if(route_attribute(out,h,1,address,size) || route_attribute(out,h,2,address,size))return -KERNEL_ENOBUFS;
    if(family==10 && slot>=0) {
        const struct kernel_socket_ipv6_address *a=&s->ipv6_addresses[slot];
        uint32_t cacheinfo[4]={a->preferred,a->valid,a->created,a->updated};
        if(route_attribute(out,h,6,cacheinfo,sizeof(cacheinfo)))return -KERNEL_ENOBUFS;
    }
    if(family==2) {
        size_t n=0;while(n<15 && s->name[n])n++;
        if(route_attribute(out,h,3,s->name,n+1))return -KERNEL_ENOBUFS;
        if(s->hardware_type!=772) { uint32_t broadcast=s->address|~s->netmask;
            if(route_attribute(out,h,4,&broadcast,4))return -KERNEL_ENOBUFS; }
    }
    return 0;
}
static int route_entry(struct route_output *out,const struct route_header *request,
    const struct kernel_socket_interface *s,unsigned kind)
{
    /* Existing IPv4 direct-subnet selection and one configured default gateway. */
    if(!s->address)return 0;
    if(kind==1 && !s->default_route)return 0;
    unsigned local=kind==2 || (kind==0 && s->hardware_type==772);
    if(kind==2 && s->hardware_type==772)return 0;
    uint32_t table=local?255:254;
    struct { uint8_t family,dst,src,tos,table,protocol,scope,type; uint32_t flags; } info=
        {2,kind==1?0:kind==2?32:route_prefix(s->netmask),0,0,table,2,local?254:kind==1?0:253,local?2:1,0};
    uint32_t destination=kind==2?s->address:s->address&s->netmask;
    struct route_header *h=route_message(out,24,2,request->sequence,&info,sizeof(info));
    if(!h)return -KERNEL_ENOBUFS;
    if(route_attribute(out,h,15,&table,4) || route_attribute(out,h,4,&s->index,4))return -KERNEL_ENOBUFS;
    if(kind==1) {if(route_attribute(out,h,5,&s->gateway,4))return -KERNEL_ENOBUFS;}
    else if(route_attribute(out,h,1,&destination,4) || route_attribute(out,h,7,&s->address,4))return -KERNEL_ENOBUFS;
    return 0;
}
static int route_ipv6(struct route_output *out,const struct route_header *request,
    const struct kernel_socket_interface *s)
{
    if(!s->ipv6_present || (s->ipv6_flags&(64|8)))return 0;
    uint8_t info[12]={10,128,0,0,255,2,0,2};
    uint8_t destination[16];__builtin_memcpy(destination,s->ipv6_address,16);
    struct route_header *h=route_message(out,24,2,request->sequence,info,sizeof(info));
    if(!h)return -KERNEL_ENOBUFS;
    if(route_attribute(out,h,15,(uint32_t[]){255},4) ||
        route_attribute(out,h,1,destination,16) || route_attribute(out,h,4,&s->index,4))return -KERNEL_ENOBUFS;
    if(s->hardware_type==772 || !(s->flags&1))return 0;
    info[1]=64;info[4]=254;info[7]=1;
    __builtin_memset(destination+8,0,8);
    h=route_message(out,24,2,request->sequence,info,sizeof(info));
    if(!h)return -KERNEL_ENOBUFS;
    return route_attribute(out,h,15,(uint32_t[]){254},4) ||
        route_attribute(out,h,1,destination,16) || route_attribute(out,h,4,&s->index,4)?-KERNEL_ENOBUFS:0;
}
static int route_static_ipv6(struct route_output *out,const struct route_header *request,
    const struct kernel_socket_interface *s)
{
    for(unsigned i=0;i<4;i++) {
        if(!s->ipv6_addresses[i].present || (s->ipv6_addresses[i].flags&(64|8)))continue;
        for(unsigned local=0;local<2;local++) {
            if(!local && (!(s->flags&1) || !(s->ipv6_addresses[i].flags&128)))continue;
            if(!local && i && s->ipv6_addresses[0].flags && !(s->ipv6_addresses[0].flags&(64|8)) &&
               s->ipv6_addresses[0].prefix==s->ipv6_addresses[i].prefix) {
                unsigned bits=s->ipv6_addresses[i].prefix,bytes=bits/8,tail=bits%8;
                if(!__builtin_memcmp(s->ipv6_addresses[0].address,s->ipv6_addresses[i].address,bytes) &&
                   (!tail || !((s->ipv6_addresses[0].address[bytes]^s->ipv6_addresses[i].address[bytes])&(255U<<(8-tail)))))continue;
            }
            uint8_t prefix=local?128:s->ipv6_addresses[i].prefix;
            uint8_t info[12]={10,prefix,0,0,local?255:254,2,0,local?2:1};
            uint8_t destination[16];__builtin_memcpy(destination,s->ipv6_addresses[i].address,16);
            for(unsigned byte=0;byte<16;byte++) {
                unsigned bits=prefix>byte*8?prefix-byte*8:0;
                if(bits<8)destination[byte]&=bits?(uint8_t)(255U<<(8-bits)):0;
            }
            struct route_header *h=route_message(out,24,2,request->sequence,info,sizeof(info));
            if(!h || route_attribute(out,h,15,(uint32_t[]){local?255:254},4) ||
                route_attribute(out,h,1,destination,16) || route_attribute(out,h,4,&s->index,4) ||
                route_attribute(out,h,6,(uint32_t[]){local?0:256},4))return -KERNEL_ENOBUFS;
        }
    }
    uint8_t zero[16]={0};
    if(!__builtin_memcmp(s->ipv6_gateway,zero,16) || !(s->flags&1))return 0;
    uint8_t info[12]={10,0,0,0,254,3,0,1};
    struct route_header *h=route_message(out,24,2,request->sequence,info,sizeof(info));
    return !h || route_attribute(out,h,15,(uint32_t[]){254},4) ||
        route_attribute(out,h,5,s->ipv6_gateway,16) || route_attribute(out,h,4,&s->index,4) ||
        route_attribute(out,h,6,(uint32_t[]){1024},4)?-KERNEL_ENOBUFS:0;
}
/* RA on-link prefixes are independent of SLAAC addresses: an A-only PIO
 * creates an address/local route, never an implied connected /64. */
static int route_ra_ipv6(struct route_output *out,const struct route_header *request,
    const struct kernel_socket_interface *s)
{
    if(!(s->flags&1) || !s->link_up)return 0;
    for(unsigned kind=0;kind<2;kind++)for(unsigned i=0;i<(kind?3U:5U);i++) {
        if(kind?!s->ipv6_routers[i].present:!s->ipv6_prefixes[i].present)continue;
        uint8_t prefix=kind?0:s->ipv6_prefixes[i].prefix;
        uint8_t info[12]={10,prefix,0,0,254,9,0,1};
        const uint8_t *address=kind?s->ipv6_routers[i].address:s->ipv6_prefixes[i].address;
        uint32_t lifetime=kind?s->ipv6_routers[i].lifetime:s->ipv6_prefixes[i].lifetime;
        struct route_header *h=route_message(out,24,2,request->sequence,info,sizeof(info));
        if(!h || route_attribute(out,h,15,(uint32_t[]){254},4) ||
           route_attribute(out,h,kind?5:1,address,16) || route_attribute(out,h,4,&s->index,4) ||
           route_attribute(out,h,6,(uint32_t[]){kind?2048:256},4))return -KERNEL_ENOBUFS;
        /* Linux IPv6 dumps expose remaining USER_HZ (100) ticks through
         * RTA_CACHEINFO, clamped to signed INT_MAX; zero means no expiry. */
        uint32_t expires=lifetime==UINT32_MAX?0:
            lifetime>21474836U?2147483647U:lifetime*100U;
        uint32_t cacheinfo[8]={0,0,expires,0,0,0,0,0};
        if(route_attribute(out,h,12,cacheinfo,sizeof(cacheinfo)))return -KERNEL_ENOBUFS;
        if(kind && route_attribute(out,h,20,&s->ipv6_routers[i].preference,1))return -KERNEL_ENOBUFS;
    }
    return 0;
}
/* Output lookups only: this is the existing lwIP route selection, not a FIB.
 * Keep snapshot order (ip4_route uses first matching netif, not longest prefix).
 * No mutable route/cache object or extra work on the packet path is needed. */
static int route_lookup(struct route_output *out,const struct route_header *h,
    const unsigned char *p,size_t size,const struct kernel_socket_interface *ifs,size_t count)
{
    uint32_t dst=0,src=0,oif=0,flags=0;int error=0;
    if(p[0]!=2)return route_error(out,h,p,size,-KERNEL_EOPNOTSUPP);
    __builtin_memcpy(&flags,p+8,4);
    if((p[1] && p[1]!=32) || (p[2] && p[2]!=32))error=-KERNEL_EINVAL;
    /* Input routing, notification and FIB-match need state this kernel lacks. */
    else if(p[3] || p[4] || p[5] || p[6] || p[7] || (flags&~0x1000U))error=-KERNEL_EOPNOTSUPP;
    for(size_t at=12;!error && at<size;) {
        uint16_t length,type;
        if(size-at<4){error=-KERNEL_EINVAL;break;}
        __builtin_memcpy(&length,p+at,2);__builtin_memcpy(&type,p+at+2,2);
        if(length<4 || length>size-at){error=-KERNEL_EINVAL;break;}
        uint32_t value=0;
        if(type==1 || type==2 || type==3 || type==4 || type==16 || type==25) {
            if(length!=8){error=-KERNEL_EINVAL;break;}
            __builtin_memcpy(&value,p+at+4,4);
            if(type==1)dst=value;
            else if(type==2)src=value;
            else if(type==4)oif=value;
            else if(type!=25 && value)error=-KERNEL_EOPNOTSUPP;
            /* UID cannot affect selection: there are no per-user rules. */
        } else error=-KERNEL_EOPNOTSUPP;
        size_t step=route_align(length);
        if(step>size-at) {if(length!=size-at)error=-KERNEL_EINVAL;break;}
        at+=step;
    }
    if(error)return route_error(out,h,p,size,error);
    const uint8_t *source_bytes=(const uint8_t *)&src;
    if(src && (!source_bytes[0] || source_bytes[0]>=224))return route_error(out,h,p,size,-KERNEL_EINVAL);
    const struct kernel_socket_interface *selected=NULL,*fallback=NULL,*loop=NULL,*bound=NULL;
    int source_ok=!src;
    for(size_t i=0;i<count;i++) {
        const struct kernel_socket_interface *s=&ifs[i];
        if(s->index==oif)bound=s;
        if(!(s->flags&1) || !s->link_up || !s->address)continue;
        if(s->address==src || (s->hardware_type==772 && (src&s->netmask)==(s->address&s->netmask)))source_ok=1;
        if(s->hardware_type==772)loop=s;
    }
    if(oif && !bound)return route_error(out,h,p,size,-KERNEL_ENODEV);
    if(!source_ok)return route_error(out,h,p,size,-KERNEL_ENETUNREACH);
    if(bound && (!(bound->flags&1) || !bound->link_up || !bound->address))
        return route_error(out,h,p,size,-KERNEL_ENETUNREACH);
    if(!dst)dst=src?src:loop?loop->address:0;
    const uint8_t *bytes=(const uint8_t *)&dst;
    if(!dst || bytes[0]>=224)return route_error(out,h,p,size,-KERNEL_EOPNOTSUPP);
    int local=0,gateway=0;
    for(size_t i=0;i<count;i++) {
        const struct kernel_socket_interface *s=&ifs[i];
        if(!(s->flags&1) || !s->link_up || !s->address)continue;
        if(dst==s->address || (s->hardware_type==772 && (dst&s->netmask)==(s->address&s->netmask))) {
            selected=s;local=1;break;
        }
        if(oif && s!=bound)continue;
        if(!selected && (dst&s->netmask)==(s->address&s->netmask))selected=s;
        if(s->default_route)fallback=s;
    }
    if(!selected && bytes[0]!=127){selected=fallback;gateway=!!selected;}
    /* A bound device still needs its configured gateway for off-link IPv4:
     * etharp_output cannot synthesize an on-link route as Linux can. */
    if(!selected && bound && bound->gateway && bytes[0]!=127){selected=bound;gateway=1;}
    if(!selected)return route_error(out,h,p,size,-KERNEL_ENETUNREACH);
    if(!local && selected->hardware_type!=772 && dst==(selected->address|~selected->netmask))
        return route_error(out,h,p,size,-KERNEL_EOPNOTSUPP);
    if(!local && source_bytes[0]==127 && selected->hardware_type!=772)
        return route_error(out,h,p,size,-KERNEL_EINVAL);
    uint32_t index=local && loop?loop->index:selected->index;
    uint32_t preferred=src?src:selected->address,table=(local && (flags&0x1000U))?255:254;
    unsigned char info[12]={2,32,src?32:0,0,table,0,0,local?2:1,0,2,0,local?128:0};
    struct route_header *reply=route_message(out,24,0,h->sequence,info,sizeof(info));
    if(!reply)return -KERNEL_ENOBUFS;
    if(route_attribute(out,reply,15,&table,4) || route_attribute(out,reply,1,&dst,4) ||
       route_attribute(out,reply,4,&index,4) ||
       (src?route_attribute(out,reply,2,&src,4):route_attribute(out,reply,7,&preferred,4)) ||
       (gateway && route_attribute(out,reply,5,&selected->gateway,4)))return -KERNEL_ENOBUFS;
    return (h->flags&4)?route_error(out,h,p,size,0):0;
}
static int route_request(struct route_output *out,const struct route_header *h,
    const unsigned char *payload,size_t size,const struct kernel_socket_interface *interfaces,size_t count)
{
    if(!(h->flags&1))return 0;
    if(h->type<16)return (h->flags&4)?route_error(out,h,payload,size,0):0;
    int dump=(h->flags&0x300)==0x300,error=0;
    size_t minimum=h->type==18?16:h->type==22?8:h->type==26?12:0;
    /* Legacy dump callers (including iproute2) send only rtgenmsg. */
    if(h->type!=18 && h->type!=22 && h->type!=26)error=-KERNEL_EOPNOTSUPP;
    else if(size<(dump?1:minimum))error=-KERNEL_EINVAL;
    else if(payload[0]!=0 && payload[0]!=2 && payload[0]!=10 && !(h->type==18 && payload[0]==17))error=-KERNEL_EAFNOSUPPORT;
    else if(!dump && h->type==22)error=-KERNEL_EOPNOTSUPP;
    if(error)return route_error(out,h,payload,size,error);
    if(!dump && h->type==26)return route_lookup(out,h,payload,size,interfaces,count);
    int32_t index=0;char name[16]={0};
    if(size>=minimum && minimum) {
        if(h->type==18)__builtin_memcpy(&index,payload+4,4);
        size_t offset=minimum;
        while(offset<size) {
            if(size-offset<4)return route_error(out,h,payload,size,-KERNEL_EINVAL);
            uint16_t length,type;__builtin_memcpy(&length,payload+offset,2);__builtin_memcpy(&type,payload+offset+2,2);
            if(length<4 || length>size-offset)return route_error(out,h,payload,size,-KERNEL_EINVAL);
            if(h->type==18 && type==3) {
                if(length<=4 || length>20 || payload[offset+length-1])return route_error(out,h,payload,size,-KERNEL_EINVAL);
                __builtin_memcpy(name,payload+offset+4,length-4);
            }
            offset+=route_align(length);
        }
    }
    if(!dump && index<=0 && !name[0])return route_error(out,h,payload,size,-KERNEL_EINVAL);
    int found=0;
    for(size_t i=0;i<count;i++) {
        const struct kernel_socket_interface *s=&interfaces[i];
        if(!dump && ((index>0 && s->index!=(uint32_t)index) || (name[0] && __builtin_strcmp(name,s->name))))continue;
        found=1;
        if(h->type==18)error=route_link(out,h,s,dump);
        else if(h->type==22) {
            if(payload[0]!=10)error=route_address(out,h,s,2,-1);
            if(!error && payload[0]!=2) {
                error=route_address(out,h,s,10,-1);
                for(unsigned slot=0;!error && slot<4;slot++)error=route_address(out,h,s,10,(int)slot);
            }
        } else {
            if(payload[0]!=10) {
                error=route_entry(out,h,s,0);if(!error)error=route_entry(out,h,s,1);
                if(!error)error=route_entry(out,h,s,2);
            }
            if(!error && payload[0]!=2) {
                error=route_ipv6(out,h,s);
                if(!error)error=route_static_ipv6(out,h,s);
                if(!error)error=route_ra_ipv6(out,h,s);
            }
        }
        if(error)return error;
    }
    if(!dump && !found)return route_error(out,h,payload,size,-KERNEL_ENODEV);
    if(dump)return route_message(out,3,2,h->sequence,(int32_t[]){0},4)?0:-KERNEL_ENOBUFS;
    return (h->flags&4)?route_error(out,h,payload,size,0):0;
}
static int route_encode(struct route_output *out,const void *data,size_t size,
    const struct kernel_socket_interface *interfaces,size_t count)
{
    size_t offset=0;
    while(size-offset>=sizeof(struct route_header)) {
        struct route_header h;__builtin_memcpy(&h,(const unsigned char *)data+offset,sizeof(h));
        if(h.length<sizeof(h) || h.length>size-offset)break; /* Linux drops malformed tail. */
        int error=route_request(out,&h,(const unsigned char *)data+offset+sizeof(h),h.length-sizeof(h),interfaces,count);
        if(error)return error;
        size_t step=route_align(h.length);if(step>size-offset)break;offset+=step;
    }
    return 0;
}
#endif
