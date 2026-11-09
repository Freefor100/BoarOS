#include <kernel/bpf_vm.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#define I(c,d,s,o,k) { .code=c, .dst_reg=d, .src_reg=s, .off=o, .imm=k }
#define EXIT I(0x95,0,0,0,0)
#define N(a) (sizeof(a)/sizeof((a)[0]))
static unsigned checks;
static char logbuf[4096];
static size_t loglen;
static uint64_t map_value;
static uint64_t scratch_values[8];
static uint32_t map_flags, map_type=2;
static unsigned allocations, frees;
static int range_mode;
static uint64_t range_min, range_max;
static void *allocate(void *x,size_t n) { (void)x; allocations++; return calloc(1,n); }
static void release(void *x,void *p) { (void)x; frees++; free(p); }
static void logger(void *x,const char *s,size_t n)
{ (void)x; if(n<sizeof(logbuf)-loglen) { memcpy(logbuf+loglen,s,n);loglen+=n;logbuf[loglen]=0; } }
static int map(void *x,uint32_t pc,const struct kernel_bpf_insn *i,struct kernel_bpf_vm_map *m)
{ (void)x;(void)pc;
    if(i->imm==10){*m=(struct kernel_bpf_vm_map){.object=scratch_values,.value=scratch_values,.key_size=4,.value_size=sizeof(scratch_values),.type=2};return 0;}
    if(i->imm!=9)return -9;
    *m=(struct kernel_bpf_vm_map){.object=&map_value,.value=&map_value,.key_size=4,.value_size=8,.flags=map_flags,.type=map_type};return 0; }
static int context(void *x,int32_t o,uint32_t n,int w,struct kernel_bpf_vm_context_access *a)
{ (void)x; if(w || o!=0 || (n!=4 && !(range_mode && n==8))) return -13; *a=(struct kernel_bpf_vm_context_access){.min=range_min,.max=range_max};return 0; }
static int helper(void *x,uint32_t id,struct kernel_bpf_vm_helper *p)
{
    (void)x; memset(p,0,sizeof(*p));
    if(id==1) {p->args[0].kind=KERNEL_BPF_ARG_MAP;p->args[1].kind=KERNEL_BPF_ARG_MAP_KEY;p->map_arg=1;p->map_types=(1ull<<1)|(1ull<<2);p->result=KERNEL_BPF_RETURN_MAP_VALUE_OR_NULL;return 0;}
    if(id==1000) {
        p->args[0]=(struct kernel_bpf_vm_arg){.kind=KERNEL_BPF_ARG_MEM_WRITE,.fixed_size=8};
        p->args[1]=(struct kernel_bpf_vm_arg){.kind=KERNEL_BPF_ARG_MEM_READ,.fixed_size=8};
        return 0;
    }
    if(id==7) return 0;
    if(id==3) {p->args[0].kind=KERNEL_BPF_ARG_MEM_WRITE;p->args[0].size_arg=2;p->args[1].kind=KERNEL_BPF_ARG_SCALAR;return 0;}
    if(id==2) {p->args[0].kind=KERNEL_BPF_ARG_MEM_READ;p->args[0].size_arg=2;p->args[1].kind=KERNEL_BPF_ARG_SCALAR;return 0;}
    if(id==131) {p->args[0].kind=KERNEL_BPF_ARG_MAP;p->args[1].kind=KERNEL_BPF_ARG_CONST;p->args[2].kind=KERNEL_BPF_ARG_SCALAR;p->map_arg=1;p->map_types=1ull<<27;p->size_arg=2;p->result=KERNEL_BPF_RETURN_RINGBUF_OR_NULL;return 0;}
    if(id==132) {p->args[0].kind=KERNEL_BPF_ARG_RINGBUF;p->args[1].kind=KERNEL_BPF_ARG_SCALAR;p->releases_ringbuf=1;return 0;}
    return id==99 ? -13 : -2;
}
static uint64_t run_helper(void*x,uint32_t id,const uint64_t args[5])
{
    (void)x;
    if(id==1)return(uintptr_t)&map_value;
    if(id==1000) {
        uint64_t value;
        memcpy(&value,(void *)(uintptr_t)args[1],sizeof(value));
        value++;
        memcpy((void *)(uintptr_t)args[0],&value,sizeof(value));
    }
    return 0;
}
static struct kernel_bpf_vm_env env={.program_type=1,.alloc=allocate,.free=release,.map=map,.helper=helper,.context=context,.log=logger};
static struct kernel_bpf_vm_workspace workspace;
static void verify(const char *name,const struct kernel_bpf_insn *p,unsigned n,int valid)
{loglen=0;int r=kernel_bpf_verify(p,n,&env);checks++;if((r==0)!=valid){fprintf(stderr,"%s: verify=%d wanted %d\n%s",name,r,valid,logbuf);abort();}}
static uint64_t run(const struct kernel_bpf_insn*p,unsigned n)
{uint32_t ctx=17;struct kernel_bpf_vm_run r={.context=&ctx,.helper=run_helper};uint64_t result=0;assert(!kernel_bpf_vm_execute(p,n,&env,&r,&workspace,&result));return result;}
static void basic(void)
{
    struct kernel_bpf_insn width[]={I(0xb7,0,0,0,-1),I(0x04,0,0,0,1),EXIT};verify("ALU32 zero extension",width,N(width),1);assert(run(width,N(width))==0);
    struct kernel_bpf_insn zero[]={I(0xb7,0,0,0,23),I(0xb7,2,0,0,0),I(0x3f,0,2,0,0),EXIT};verify("register divide zero",zero,N(zero),1);assert(run(zero,N(zero))==0);
    struct kernel_bpf_insn uninit[]={I(0xbf,0,2,0,0),EXIT};verify("uninitialized register",uninit,N(uninit),0);
    struct kernel_bpf_insn stack[]={I(0x79,0,10,-8,0),EXIT};verify("uninitialized stack",stack,N(stack),0);
    struct kernel_bpf_insn store[]={I(0x7a,10,0,-8,42),I(0x79,0,10,-8,0),EXIT};verify("stack read write",store,N(store),1);assert(run(store,N(store))==42);
    store[0].off=-513;verify("stack underflow",store,N(store),0);
    struct kernel_bpf_insn loop[]={I(0xb7,0,0,0,0),I(0x07,0,0,0,1),I(0xa5,0,0,-2,100),EXIT};verify("bounded loop",loop,N(loop),1);assert(run(loop,N(loop))==100);
    struct kernel_bpf_insn forever[]={I(0xb7,0,0,0,0),I(0x05,0,0,-1,0),EXIT};verify("nontermination",forever,N(forever),0);
    struct kernel_bpf_insn call[]={I(0x85,0,1,0,1),EXIT,I(0x7a,10,0,-8,31),I(0x79,0,10,-8,0),EXIT};verify("local call frame",call,N(call),1);assert(run(call,N(call))==31);
    call[0].imm=-1;verify("recursion",call,N(call),0);
    struct kernel_bpf_insn badctx[]={I(0x79,0,1,0,0),EXIT};verify("context field width",badctx,N(badctx),0);
    struct kernel_bpf_insn merge[]={I(0x61,0,1,0,0),I(0x15,0,0,1,0),I(0x7a,10,0,-8,1),I(0x79,0,10,-8,0),EXIT};verify("one branch uninitialized",merge,N(merge),0);
}
static void maps(void)
{
    struct kernel_bpf_insn p[]={I(0x18,1,1,0,9),I(0,0,0,0,0),I(0xbf,2,10,0,0),I(0x07,2,0,0,-4),I(0x62,10,0,-4,0),I(0x85,0,0,0,1),I(0x15,0,0,1,0),I(0x79,0,0,0,0),EXIT};
    map_value=55;verify("map null refinement",p,N(p),1);assert(run(p,N(p))==55);
    p[6].code=0x05;p[6].off=0;verify("nullable map dereference",p,N(p),0);p[6].code=0x15;p[6].off=1;p[7].off=8;verify("map bounds",p,N(p),0);
    struct kernel_bpf_insn wrong[]={I(0xb7,1,0,0,1),I(0xb7,2,0,0,0),I(0x85,0,0,0,1),EXIT};verify("map helper type",wrong,N(wrong),0);
    struct kernel_bpf_insn atomic[]={I(0x7a,10,0,-8,7),I(0xb7,1,0,0,3),I(0xdb,10,1,-8,1),I(0x79,0,10,-8,0),I(0x0f,0,1,0,0),EXIT};verify("atomic fetch add",atomic,N(atomic),1);assert(run(atomic,N(atomic))==17);
}
static void immutable_maps(void)
{
    /* Original bpftrace masks a CPU index with frozen rodata, then indexes
     * its separate mutable loss-counter array. */
    struct kernel_bpf_insn p[]={I(0x18,6,2,0,9),I(0,0,0,0,0),I(0x79,6,6,0,0),
        I(0x85,0,0,0,7),I(0x5f,6,0,0,0),I(0x67,6,0,0,3),
        I(0x18,1,2,0,10),I(0,0,0,0,0),I(0x0f,1,6,0,0),I(0x79,0,1,0,0),EXIT};
    map_value=7;map_flags=KERNEL_BPF_MAP_READONLY;
    verify("unfrozen rodata cannot supply constant index",p,N(p),0);
    map_flags|=KERNEL_BPF_MAP_CONSTANT_VALUE;
    verify("immutable mask bounds variable map pointer",p,N(p),1);
    scratch_values[0]=73;assert(run(p,N(p))==73);
    map_value=8;verify("immutable mask still rejects out-of-bounds index",p,N(p),0);
    p[9]=(struct kernel_bpf_insn)I(0x7a,1,0,0,1);map_value=7;
    verify("bounded variable pointer store",p,N(p),1);
    map_flags=0;
}

static void arithmetic(void)
{
    struct kernel_bpf_insn p[]={I(0xb7,0,0,0,0),I(0xb7,2,0,0,0),I(0x0f,0,2,0,0),EXIT};
    struct arithmetic_case {uint8_t op;int16_t off;int32_t a,b;uint64_t want;} cases[]={
        {0x0f,0,-1,1,0},{0x1f,0,2,5,UINT64_MAX-2},{0x2f,0,-3,4,(uint64_t)-12},
        {0x3f,0,-1,2,UINT64_MAX/2},{0x9f,0,27,0,27},{0x3f,1,-27,4,(uint64_t)-6},
        {0x9f,1,-27,4,(uint64_t)-3},{0x3c,1,INT32_MIN,-1,(uint32_t)INT32_MIN},
        {0x9c,1,INT32_MIN,-1,0},{0x6f,0,1,65,2},{0x7f,0,-1,63,1},
        {0xcf,0,-4,1,(uint64_t)-2},{0xcc,0,-4,1,(uint32_t)-2},
        {0x4f,0,10,5,15},{0x5f,0,10,5,0},{0xaf,0,10,5,15}
    };
    for(unsigned j=0;j<N(cases);j++) {
        p[0].imm=cases[j].a;p[1].imm=cases[j].b;p[2].code=cases[j].op;p[2].off=cases[j].off;
        verify("ALU semantics",p,N(p),1);assert(run(p,N(p))==cases[j].want);
    }
    struct kernel_bpf_insn endian[]={I(0x18,0,0,0,0x55667788),I(0,0,0,0,0x11223344),I(0xdc,0,0,0,64),EXIT};
    verify("64-bit endian",endian,N(endian),1);assert(run(endian,N(endian))==0x8877665544332211ull);
    struct kernel_bpf_insn sx[]={I(0xb7,2,0,0,0x80),I(0xbf,0,2,8,0),EXIT};
    verify("MOVSX byte",sx,N(sx),1);assert(run(sx,N(sx))==(uint64_t)-128);
    sx[1].code=0xbc;verify("MOVSX32 byte",sx,N(sx),1);assert(run(sx,N(sx))==(uint32_t)-128);
    struct kernel_bpf_insn memsx[]={I(0x72,10,0,-1,0x80),I(0x91,0,10,-1,0),EXIT};
    verify("sign-extending byte load",memsx,N(memsx),1);assert(run(memsx,N(memsx))==(uint64_t)-128);
    struct kernel_bpf_insn shift[]={I(0xb7,0,0,0,1),I(0x67,0,0,0,64),EXIT};verify("invalid immediate shift",shift,N(shift),0);
    struct kernel_bpf_insn zero[]={I(0xb7,0,0,0,1),I(0x37,0,0,0,0),EXIT};verify("invalid immediate divide zero",zero,N(zero),0);
}
static void pointer_safety(void)
{
    struct kernel_bpf_insn partial[]={I(0x7b,10,1,-8,0),I(0x72,10,0,-8,0),I(0x79,0,10,-8,0),EXIT};
    verify("partial pointer overwrite cannot leak",partial,N(partial),0);
    struct kernel_bpf_insn atom[]={I(0x7b,10,1,-8,0),I(0xb7,2,0,0,1),I(0xdb,10,2,-8,1),I(0xbf,0,2,0,0),EXIT};
    verify("atomic pointer spill cannot leak",atom,N(atom),0);
    struct kernel_bpf_insn leak[]={I(0x7b,10,1,-8,0),I(0xbf,1,10,0,0),I(0x07,1,0,0,-8),I(0xb7,2,0,0,8),I(0x85,0,0,0,2),EXIT};
    verify("helper read pointer cannot leak",leak,N(leak),0);
    struct kernel_bpf_insn spill[]={I(0x7b,10,1,-8,0),I(0x79,2,10,-8,0),I(0x61,0,2,0,0),EXIT};
    verify("full pointer spill reload",spill,N(spill),1);assert(run(spill,N(spill))==17);
    struct kernel_bpf_insn escape[]={I(0xbf,1,10,0,0),I(0x07,1,0,0,-8),I(0x85,0,1,0,2),I(0xb7,0,0,0,0),EXIT,I(0x7b,1,10,0,0),I(0xb7,0,0,0,0),EXIT};
    verify("callee stack pointer escape",escape,N(escape),0);
    struct kernel_bpf_insn dead[]={I(0x85,0,1,0,1),EXIT,I(0xbf,0,10,0,0),EXIT};verify("return callee frame pointer",dead,N(dead),0);
    struct kernel_bpf_insn caller[]={I(0x7a,10,0,-8,7),I(0xbf,1,10,0,0),I(0x07,1,0,0,-8),I(0x85,0,1,0,2),I(0x79,0,10,-8,0),EXIT,I(0x7a,1,0,0,19),I(0xb7,0,0,0,0),EXIT};
    verify("callee writes caller stack",caller,N(caller),1);assert(run(caller,N(caller))==19);
    struct kernel_bpf_insn longjump[]={I(0x06,0,0,0,1),I(0xb7,0,0,0,1),I(0xb7,0,0,0,2),EXIT};verify("JMP32 long jump",longjump,N(longjump),1);assert(run(longjump,N(longjump))==2);
    struct kernel_bpf_insn into[]={I(0x05,0,0,1,0),I(0x18,0,0,0,0),I(0,0,0,0,0),EXIT};verify("jump into LDDW tail",into,N(into),0);
    struct kernel_bpf_insn global[]={I(0x18,2,2,0,9),I(0,0,0,0,0),I(0x79,0,2,0,0),EXIT};map_value=91;
    verify("global map-data relocation",global,N(global),1);assert(run(global,N(global))==91);
    global[1].imm=8;verify("global relocation bounds",global,N(global),0);global[1].imm=0;
    map_flags=KERNEL_BPF_MAP_WRITEONLY;verify("write-only map read",global,N(global),0);map_flags=0;
    struct kernel_bpf_insn ro[]={I(0x18,2,2,0,9),I(0,0,0,0,0),I(0x7a,2,0,0,1),I(0xb7,0,0,0,0),EXIT};
    map_flags=KERNEL_BPF_MAP_READONLY;verify("read-only map write",ro,N(ro),0);map_flags=0;
    struct kernel_bpf_insn helperbad[]={I(0x85,0,0,0,4321),EXIT};verify("unknown helper",helperbad,N(helperbad),0);assert(strstr(logbuf,"unknown func 4321"));
    helperbad[0].imm=99;verify("forbidden helper",helperbad,N(helperbad),0);assert(strstr(logbuf,"program of this type cannot use helper"));
}
static void reservations(void)
{
    map_type=27;
    struct kernel_bpf_insn leak[]={I(0x18,1,1,0,9),I(0,0,0,0,0),I(0xb7,2,0,0,8),I(0xb7,3,0,0,0),I(0x85,0,0,0,131),I(0xb7,0,0,0,0),EXIT};
    verify("unreleased reservation",leak,N(leak),0);
    struct kernel_bpf_insn p[]={I(0x18,1,1,0,9),I(0,0,0,0,0),I(0xb7,2,0,0,8),I(0xb7,3,0,0,0),I(0x85,0,0,0,131),I(0x15,0,0,5,0),I(0xbf,6,0,0,0),I(0x7a,0,0,0,1),I(0xbf,1,0,0,0),I(0xb7,2,0,0,0),I(0x85,0,0,0,132),I(0xb7,0,0,0,0),EXIT};
    verify("balanced nullable reservation",p,N(p),1);
    p[11]=(struct kernel_bpf_insn)I(0x79,0,6,0,0);verify("use after ring release",p,N(p),0);
    p[11]=(struct kernel_bpf_insn)I(0xb7,0,0,0,0);p[7].off=8;verify("reservation bounds",p,N(p),0);
    map_type=2;verify("ring helper wrong map type",leak,N(leak),0);
}

static int packet_context(void*x,int32_t off,uint32_t n,int write,struct kernel_bpf_vm_context_access*a)
{
    (void)x;if(write||n!=8||(off!=0&&off!=8))return -13;
    *a=(struct kernel_bpf_vm_context_access){.kind=off?KERNEL_BPF_CONTEXT_PACKET_END:KERNEL_BPF_CONTEXT_PACKET};return 0;
}
static void packet_bounds(void)
{
    env.context=packet_context;
    struct kernel_bpf_insn p[]={I(0x79,2,1,0,0),I(0x79,3,1,8,0),I(0xbf,4,2,0,0),I(0x07,4,0,0,4),I(0xb7,0,0,0,0),I(0x2d,4,3,1,0),I(0x61,0,2,0,0),EXIT};
    verify("packet end guard",p,N(p),1);
    unsigned char bytes[4]={1,2,3,4};uint64_t ctx[2]={(uintptr_t)bytes,(uintptr_t)(bytes+4)};
    struct kernel_bpf_vm_run r={.context=ctx};uint64_t result;
    assert(!kernel_bpf_vm_execute(p,N(p),&env,&r,&workspace,&result));assert(result==0x04030201);
    ctx[1]=(uintptr_t)(bytes+3);assert(!kernel_bpf_vm_execute(p,N(p),&env,&r,&workspace,&result));assert(!result);
    p[3].imm=3;verify("packet guard too short",p,N(p),0);
    p[3].imm=4;p[6].off=-1;verify("packet negative offset",p,N(p),0);
    env.context=context;
}
static void frames_and_helpers(void)
{
    struct kernel_bpf_insn frames[34];
    for(unsigned j=0;j<17;j++){frames[2*j]=(struct kernel_bpf_insn)I(0x85,0,1,0,1);frames[2*j+1]=(struct kernel_bpf_insn)EXIT;}
    frames[30]=(struct kernel_bpf_insn)I(0xb7,0,0,0,77);
    verify("sixteen call frames",frames,32,1);assert(run(frames,32)==77);
    frames[30]=(struct kernel_bpf_insn)I(0x85,0,1,0,1);frames[32]=(struct kernel_bpf_insn)I(0xb7,0,0,0,77);
    verify("seventeen call frames rejected",frames,34,0);
    struct kernel_bpf_insn init[]={I(0xbf,1,10,0,0),I(0x07,1,0,0,-8),I(0xb7,2,0,0,8),I(0x85,0,0,0,3),I(0x79,0,10,-8,0),EXIT};
    verify("helper initializes destination",init,N(init),1);
    init[2].imm=4;verify("helper cannot initialize extra bytes",init,N(init),0);
    struct kernel_bpf_insn source[]={I(0x62,10,0,-8,0),I(0xbf,1,10,0,0),I(0x07,1,0,0,-8),I(0xb7,2,0,0,8),I(0x85,0,0,0,2),EXIT};
    verify("helper rejects uninitialized source",source,N(source),0);
    struct kernel_bpf_insn bounded[]={I(0x61,1,1,0,0),I(0x54,1,0,0,15),I(0xb7,0,0,0,0),I(0x1d,0,1,2,0),I(0x07,0,0,0,1),I(0xa5,0,0,-3,16),EXIT};
    verify("unknown bound with finite induction",bounded,N(bounded),1);assert(run(bounded,N(bounded))==1);
    struct kernel_bpf_insn alias[]={I(0x18,1,1,0,9),I(0,0,0,0,0),I(0x62,10,0,-4,0),I(0xbf,2,10,0,0),I(0x07,2,0,0,-4),I(0x85,0,0,0,1),I(0x7b,10,0,-16,0),I(0x15,0,0,2,0),I(0x79,6,10,-16,0),I(0x79,0,6,0,0),EXIT};
    map_value=66;verify("nullable spilled aliases refine together",alias,N(alias),1);assert(run(alias,N(alias))==66);
}
static void helper_output_order(void)
{
    struct kernel_bpf_insn p[]={
        I(0xbf,1,10,0,0),I(0x07,1,0,0,-8),I(0xbf,2,1,0,0),
        I(0x85,0,0,0,1000),I(0x79,0,10,-8,0),EXIT
    };
    verify("helper output cannot initialize overlapping input",p,N(p),0);
    struct kernel_bpf_insn initialized[]={
        I(0x7a,10,0,-8,73),I(0xbf,1,10,0,0),I(0x07,1,0,0,-8),
        I(0xbf,2,1,0,0),I(0x85,0,0,0,1000),I(0x79,0,10,-8,0),EXIT
    };
    verify("helper accepts initialized overlapping input",initialized,N(initialized),1);
    assert(run(initialized,N(initialized))==74);
    initialized[0]=(struct kernel_bpf_insn)I(0x62,10,0,-8,73);
    verify("helper rejects partially initialized overlapping input",initialized,N(initialized),0);
    initialized[0]=(struct kernel_bpf_insn)I(0x7b,10,1,-8,0);
    verify("helper output cannot erase input pointer spill",initialized,N(initialized),0);
}
static const unsigned char packet_bytes[]={0x45,0x01,0xab,0xcd,0x11,0x22,0x33,0x44};
static unsigned packet_reads, ancillary_reads;
static int packet(void*x,int64_t offset,uint32_t size,uint32_t*out)
{
    (void)x;packet_reads++;if(offset<0||(uint64_t)offset+size>sizeof(packet_bytes))return -1;
    *out=0;for(uint32_t j=0;j<size;j++)*out=(*out<<8)|packet_bytes[offset+j];return 0;
}
static uint32_t packet_length(void*x){(void)x;return sizeof(packet_bytes);}
static int ancillary(void*x,uint32_t selector,uint32_t a,uint32_t b,uint32_t*out)
{(void)x;ancillary_reads++;if(selector==8)*out=5;else if(selector==12)*out=a+b;else return -1;return 0;}
static uint64_t classic_run(const struct kernel_bpf_classic_insn*p,uint32_t n,int valid)
{
    uint32_t count=0;loglen=0;int e=kernel_bpf_classic_convert(p,n,0,0,&count,&env);checks++;
    if(!valid){assert(e);return 0;}if(e){fprintf(stderr,"classic sizing %d\n",e);abort();}
    struct kernel_bpf_insn*code=calloc(count,sizeof(*code));assert(code);
    e=kernel_bpf_classic_convert(p,n,code,count,&count,&env);if(e){fprintf(stderr,"classic conversion %d\n%s",e,logbuf);abort();}
    struct kernel_bpf_vm_env ce=env;ce.flags|=KERNEL_BPF_VM_CLASSIC;
    struct kernel_bpf_vm_run run={.packet=packet,.packet_length=packet_length,.ancillary=ancillary};uint64_t value;
    assert(!kernel_bpf_vm_execute(code,count,&ce,&run,&workspace,&value));free(code);return value;
}
#define C(c,t,f,v) { .code=c, .jt=t, .jf=f, .k=v }
static void classic(void)
{
    const struct kernel_bpf_classic_insn word[]={C(0x20,0,0,2),C(0x16,0,0,0)};assert(classic_run(word,N(word),1)==0xabcd1122);
    const struct kernel_bpf_classic_insn oob[]={C(0x20,0,0,6),C(0x06,0,0,99)};assert(classic_run(oob,N(oob),1)==0);
    const struct kernel_bpf_classic_insn len[]={C(0x80,0,0,0),C(0x16,0,0,0)};assert(classic_run(len,N(len),1)==8);
    const struct kernel_bpf_classic_insn msh[]={C(0x00,0,0,99),C(0xb1,0,0,0),C(0x02,0,0,0),C(0x87,0,0,0),C(0x61,0,0,0),C(0x0c,0,0,0),C(0x16,0,0,0)};assert(classic_run(msh,N(msh),1)==119);
    const struct kernel_bpf_classic_insn ind[]={C(0x01,0,0,2),C(0x48,0,0,0),C(0x16,0,0,0)};assert(classic_run(ind,N(ind),1)==0xabcd);
    const struct kernel_bpf_classic_insn jump[]={C(0x00,0,0,UINT32_MAX),C(0x15,1,0,UINT32_MAX),C(0x06,0,0,3),C(0x06,0,0,7)};assert(classic_run(jump,N(jump),1)==7);
    const struct kernel_bpf_classic_insn div[]={C(0x00,0,0,99),C(0x3c,0,0,0),C(0x06,0,0,33)};assert(classic_run(div,N(div),1)==0);
    const struct kernel_bpf_classic_insn mod[]={C(0x00,0,0,99),C(0x9c,0,0,0),C(0x06,0,0,33)};assert(classic_run(mod,N(mod),1)==0);
    const struct kernel_bpf_classic_insn anc[]={C(0x20,0,0,(uint32_t)(-4096+8)),C(0x16,0,0,0)};assert(classic_run(anc,N(anc),1)==5);assert(ancillary_reads);
    const struct kernel_bpf_classic_insn attr[]={C(0x00,0,0,5),C(0x01,0,0,7),C(0x20,0,0,(uint32_t)(-4096+12)),C(0x16,0,0,0)};assert(classic_run(attr,N(attr),1)==12);
    const struct kernel_bpf_classic_insn xor[]={C(0x00,0,0,5),C(0x01,0,0,7),C(0x20,0,0,(uint32_t)(-4096+40)),C(0x16,0,0,0)};assert(classic_run(xor,N(xor),1)==2);
    const struct kernel_bpf_classic_insn badanc[]={C(0x20,0,0,(uint32_t)(-4096+1)),C(0x16,0,0,0)};classic_run(badanc,N(badanc),0);
    const struct kernel_bpf_classic_insn badmem[]={C(0x60,0,0,0),C(0x16,0,0,0)};classic_run(badmem,N(badmem),0);
    const struct kernel_bpf_classic_insn badmerge[]={C(0x80,0,0,0),C(0x15,1,0,0),C(0x02,0,0,0),C(0x60,0,0,0),C(0x16,0,0,0)};classic_run(badmerge,N(badmerge),0);
    const struct kernel_bpf_classic_insn badshift[]={C(0x64,0,0,32),C(0x16,0,0,0)};classic_run(badshift,N(badshift),0);
    const struct kernel_bpf_classic_insn badjump[]={C(0x05,0,0,UINT32_MAX),C(0x16,0,0,0)};classic_run(badjump,N(badjump),0);
    const struct kernel_bpf_classic_insn badend[]={C(0x00,0,0,1)};classic_run(badend,N(badend),0);
    const struct kernel_bpf_classic_insn neg[]={C(0x00,0,0,1),C(0x84,0,0,1234),C(0x16,0,0,0)};assert(classic_run(neg,N(neg),1)==UINT32_MAX);
    struct kernel_bpf_classic_insn *largest=calloc(KERNEL_BPF_CLASSIC_MAX_INSNS+1,sizeof(*largest));assert(largest);
    for(unsigned j=0;j<KERNEL_BPF_CLASSIC_MAX_INSNS;j++)largest[j]=(struct kernel_bpf_classic_insn)C(0x05,0,0,0);
    largest[KERNEL_BPF_CLASSIC_MAX_INSNS-1]=(struct kernel_bpf_classic_insn)C(0x06,0,0,99);
    assert(classic_run(largest,KERNEL_BPF_CLASSIC_MAX_INSNS,1)==99);
    classic_run(largest,KERNEL_BPF_CLASSIC_MAX_INSNS+1,0);free(largest);
    assert(packet_reads);
}

/* A pruned branch may contain invalid memory only when no concrete input in
 * the declared context interval can reach it. Check sign-boundary intervals
 * and both outcomes independently; conservative rejections are permitted. */
static void range_pruning(void)
{
    const uint64_t bases[]={0,1,0x7ffffffd,0x80000000,0xfffffffc,
        0x7ffffffffffffffcull,0x8000000000000000ull,UINT64_MAX-3};
    const int32_t constants[]={INT32_MIN,-2,-1,0,1,2,INT32_MAX};
    const uint8_t ops[]={0x10,0x20,0x30,0x40,0x50,0x60,0x70,0xa0,0xb0,0xc0,0xd0};
    range_mode=1;
    for(unsigned w=0;w<2;w++)for(unsigned b=0;b<N(bases);b++) {
        if(!w&&bases[b]>UINT32_MAX)continue;
        range_min=bases[b];range_max=bases[b]+3;
        for(unsigned j=0;j<N(constants);j++)for(unsigned op=0;op<N(ops);op++)for(unsigned bad=0;bad<2;bad++) {
            struct kernel_bpf_insn p[]={I(0x79,0,1,0,0),I(0x15,0,0,2,0),I(0xb7,0,0,0,0),EXIT,I(0x79,0,10,-8,0),EXIT};
            p[0].code=w?0x79:0x61;p[1].code=ops[op]|(w?5:6);p[1].imm=constants[j];
            if(!bad){p[1].off=1;p[2]=p[4];p[4]=(struct kernel_bpf_insn)I(0xb7,0,0,0,0);}
            int reachable=0;
            for(uint64_t x=range_min;;x++) {
                uint64_t y=w?(uint64_t)(int64_t)constants[j]:(uint32_t)constants[j];
                int64_t sx=w?(int64_t)x:(int32_t)x, sy=w?(int64_t)y:(int32_t)y;
                int cond=0;
                switch(ops[op]) {
                case 0x10:cond=x==y;break;case 0x20:cond=x>y;break;case 0x30:cond=x>=y;break;
                case 0x40:cond=(x&y)!=0;break;case 0x50:cond=x!=y;break;
                case 0x60:cond=sx>sy;break;case 0x70:cond=sx>=sy;break;
                case 0xa0:cond=x<y;break;case 0xb0:cond=x<=y;break;
                case 0xc0:cond=sx<sy;break;case 0xd0:cond=sx<=sy;break;
                }
                reachable|=cond==(int)bad;
                if(x==range_max)break;
            }
            loglen=0;int result=kernel_bpf_verify(p,N(p),&env);
            checks++;if(reachable&&!result){fprintf(stderr,"unsound prune width=%u min=%llx op=%x k=%d bad=%u\n",w,(unsigned long long)range_min,ops[op],constants[j],bad);abort();}
        }
    }
    range_mode=0;range_min=range_max=0;
}
static const struct kernel_bpf_insn concurrent_program[]={I(0x18,2,2,0,9),I(0,0,0,0,0),I(0xb7,1,0,0,1),I(0xdb,2,1,0,0),I(0xb7,0,0,0,0),EXIT};
static void *atomic_thread(void*arg)
{
    (void)arg;struct kernel_bpf_vm_workspace *w=calloc(1,sizeof(*w));assert(w);
    struct kernel_bpf_vm_run r={0};uint64_t result;
    for(unsigned j=0;j<10000;j++)assert(!kernel_bpf_vm_execute(concurrent_program,N(concurrent_program),&env,&r,w,&result));
    free(w);return 0;
}
static void atomics(void)
{
    struct kernel_bpf_insn cmp[]={I(0x7a,10,0,-8,7),I(0xb7,0,0,0,7),I(0xb7,1,0,0,19),I(0xdb,10,1,-8,0xf1),I(0x79,2,10,-8,0),I(0x0f,0,2,0,0),EXIT};
    verify("CMPXCHG success",cmp,N(cmp),1);assert(run(cmp,N(cmp))==26);
    cmp[1].imm=6;verify("CMPXCHG failure",cmp,N(cmp),1);assert(run(cmp,N(cmp))==14);
    struct kernel_bpf_insn xchg[]={I(0x7a,10,0,-8,7),I(0xb7,1,0,0,19),I(0xdb,10,1,-8,0xe1),I(0x79,0,10,-8,0),I(0x0f,0,1,0,0),EXIT};
    verify("XCHG",xchg,N(xchg),1);assert(run(xchg,N(xchg))==26);
    xchg[2].off=-7;verify("misaligned atomic",xchg,N(xchg),0);
    verify("concurrent map atomic",concurrent_program,N(concurrent_program),1);
    map_value=0;pthread_t threads[4];
    for(unsigned j=0;j<4;j++)assert(!pthread_create(&threads[j],0,atomic_thread,0));
    for(unsigned j=0;j<4;j++)assert(!pthread_join(threads[j],0));
    assert(map_value==40000);
}
int main(int argc,char **argv)
{
    if(argc==2) {
        FILE *file=fopen(argv[1],"rb");assert(file);
        assert(!fseek(file,0,SEEK_END));long bytes=ftell(file);
        assert(bytes>0&&bytes%8==0);rewind(file);
        struct kernel_bpf_insn *code=malloc((size_t)bytes);assert(code);
        assert(fread(code,1,(size_t)bytes,file)==(size_t)bytes);fclose(file);
        verify("LLVM v4 bounded local-call output",code,(unsigned)bytes/8,1);
        assert(run(code,(unsigned)bytes/8)==0x8d000000);
        free(code);assert(allocations==frees);
        puts("bpf-vm: actual LLVM v4 program verified and executed");return 0;
    }
    basic();arithmetic();maps();immutable_maps();pointer_safety();reservations();packet_bounds();frames_and_helpers();helper_output_order();classic();range_pruning();atomics();
    assert(allocations==frees);
    printf("bpf-vm: %u behavioral checks passed; verifier allocations balanced\n",checks);
    return 0;
}
