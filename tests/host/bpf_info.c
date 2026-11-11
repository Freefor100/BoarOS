#include "../../kernel/bpf/private.h"
#include <kernel/credentials.h>
#include <kernel/errno.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int privileged=1;
static const struct kernel_credentials credentials;
const struct kernel_credentials *kernel_credentials_current(void){return &credentials;}
int kernel_credentials_capable(const struct kernel_credentials *c,unsigned cap){(void)c;(void)cap;return privileged;}
int bpf_user_copy(struct kernel_mm *mm,uint64_t a,void *p,size_t n,int from)
{(void)mm;if(n && a<4096)return -KERNEL_EFAULT;if(from)memcpy(p,(void *)(uintptr_t)a,n);else memcpy((void *)(uintptr_t)a,p,n);return 0;}
const struct bpf_map_spec *bpf_map_spec(const struct bpf_map *m){return (const void *)m;}
static void tags(void)
{
    static const struct {unsigned count;const char *hex;} samples[]={
        {2,"b11459a0e11ca14c"},{8,"f33d9a86ab8e18e9"},{9,"ed1283c7020d4816"},{17,"93bc5b6f11915486"}
    };
    struct kernel_bpf_insn insns[17];
    for(unsigned i=0;i<17;i++)insns[i]=(struct kernel_bpf_insn){.code=0xb7,.imm=1};
    for(unsigned n=0;n<sizeof(samples)/sizeof(samples[0]);n++){
        unsigned count=samples[n].count;insns[count-1]=(struct kernel_bpf_insn){.code=0x95};
        struct kernel_bpf_program p={.instructions=insns,.count=count};bpf_program_calculate_tag(&p);
        char hex[17];for(unsigned i=0;i<8;i++)sprintf(hex+i*2,"%02x",p.tag[i]);
        assert(!strcmp(hex,samples[n].hex));insns[count-1]=(struct kernel_bpf_insn){.code=0xb7,.imm=1};
    }
    insns[0]=(struct kernel_bpf_insn){.code=0x18,.src_reg=1,.imm=3};insns[1]=(struct kernel_bpf_insn){0};
    struct kernel_bpf_program p={.instructions=insns,.count=2};bpf_program_calculate_tag(&p);
    unsigned char tag[8];memcpy(tag,p.tag,8);insns[0].imm=55;bpf_program_calculate_tag(&p);assert(!memcmp(tag,p.tag,8));
}
static void information(void)
{
    struct bpf_map_spec spec={2,4,8,10,0};struct bpf_map_object map={.object={.kind=BPF_OBJECT_MAP,.id=41},.storage=(void *)&spec};
    struct bpf_map_information mi={0};uint32_t size=sizeof(mi);
    assert(!bpf_object_information(NULL,&map.object,(uintptr_t)&mi,&size));
    assert(size==sizeof(mi) && mi.id==41 && mi.max_entries==10 && mi.key_size==4);
    struct bpf_map_object *maps[]={&map};struct kernel_bpf_insn code[3]={{.code=0x18,.src_reg=1,.imm=7},{0},{.code=0x95}};
    struct bpf_program_binding binding={.pc=0,.map=&map};uint32_t funcs[2]={0,1},lines[4]={0,1,2,3};
    struct kernel_bpf_program p={.object={.kind=BPF_OBJECT_PROGRAM,.id=9,.uid=42,.name="tracer"},
        .type=5,.count=3,.instructions=code,.bindings=&binding,.binding_count=1,.maps=maps,.map_count=1,
        .loaded_at=123,.run_time=456,.run_count=7,.gpl=1,.function_info=funcs,.function_count=1,
        .line_info=lines,.line_count=1};
    bpf_program_calculate_tag(&p);uint32_t ids[2]={0},fout[2],lout[4];struct kernel_bpf_insn translated[3];
    struct bpf_program_information pi={.map_count=2,.map_ids=(uintptr_t)ids,
        .translated_length=sizeof(translated),.translated=(uintptr_t)translated,.funcs=(uintptr_t)fout,.func_count=1,
        .func_record_size=8,.lines=(uintptr_t)lout,.line_count=1,.line_record_size=16};
    size=sizeof(pi);assert(!bpf_object_information(NULL,&p.object,(uintptr_t)&pi,&size));
    assert(size==BPF_END(struct bpf_program_information,recursion_misses) && pi.id==9 && pi.uid==42 && pi.flags==1);
    assert(ids[0]==41 && !ids[1] && pi.map_count==1 && pi.translated_length==24 && translated[0].imm==41);
    assert(pi.run_time==456 && pi.run_count==7 && !pi.jited_length && !memcmp(fout,funcs,8) && !memcmp(lout,lines,16));
    assert(!memcmp(pi.tag,p.tag,8));
    pi=(struct bpf_program_information){.func_count=1};size=sizeof(pi);
    assert(bpf_object_information(NULL,&p.object,(uintptr_t)&pi,&size)==-KERNEL_EINVAL);
    pi=(struct bpf_program_information){.map_count=1,.map_ids=1};size=sizeof(pi);
    assert(bpf_object_information(NULL,&p.object,(uintptr_t)&pi,&size)==-KERNEL_EFAULT);
    pi=(struct bpf_program_information){0};size=sizeof(pi);privileged=0;
    assert(!bpf_object_information(NULL,&p.object,(uintptr_t)&pi,&size) && !pi.translated_length);privileged=1;
    unsigned char data[]={1,2,3,4},copy[3]={0};char name[5]={0};
    struct bpf_btf btf={.object={.kind=BPF_OBJECT_BTF,.id=8,.name="vmlinux"},.data=data,.size=4,.kernel=1};
    struct bpf_btf_information bi={.data=(uintptr_t)copy,.size=3,.name=(uintptr_t)name,.name_length=5};size=sizeof(bi);
    assert(bpf_object_information(NULL,&btf.object,(uintptr_t)&bi,&size)==-KERNEL_ENOSPC);
    assert(bi.size==4 && bi.name_length==7 && bi.kernel && !memcmp(copy,data,3) && !strcmp(name,"vmli"));
    struct bpf_perf event={.event=1};struct bpf_link link={.object={.kind=BPF_OBJECT_LINK,.id=11},.event=&event,.program=&p,.type=7,.cookie=1234};
    char tpname[16]={0};struct bpf_link_information li={.payload={0,(uintptr_t)tpname,sizeof(tpname)}};size=sizeof(li);
    assert(!bpf_object_information(NULL,&link.object,(uintptr_t)&li,&size));
    assert(size==48 && li.program_id==9 && li.payload[0]==5 && li.payload[3]==1234 && !strcmp(tpname,"sys_enter"));
}
int main(void){tags();information();puts("BPF INFO HOST PASS SHA256 nested buffers lengths metadata");}
