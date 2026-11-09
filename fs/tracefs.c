#include "bpf_fs.h"
#include <kernel/bpf.h>
#include <kernel/trace_events.h>
#include <string.h>

static char *text(char *out,const char *in)
{size_t n=strlen(in);memcpy(out,in,n);return out+n;}
static char *number(char *out,size_t n)
{char digits[24];unsigned count=0;do{digits[count++]=(char)('0'+n%10);n/=10;}while(n);while(count)*out++=digits[--count];return out;}
static char *field(char *p,const char *name,size_t offset,size_t size,int sign)
{
    p=text(p,"\tfield:");p=text(p,name);p=text(p,";\toffset:");p=number(p,offset);
    p=text(p,";\tsize:");p=number(p,size);p=text(p,";\tsigned:");p=number(p,sign);return text(p,";\n");
}
static size_t format(char *p,int enter)
{
    char *start=p;p=text(p,enter?"name: sys_enter\nID: ":"name: sys_exit\nID: ");
    p=number(p,enter?KERNEL_TRACE_SYS_ENTER:KERNEL_TRACE_SYS_EXIT);p=text(p,"\nformat:\n");
#define COMMON(member,name,sign) p=field(p,name,offsetof(struct kernel_trace_entry,member),sizeof(((struct kernel_trace_entry *)0)->member),sign)
    COMMON(type,"unsigned short common_type",0);COMMON(flags,"unsigned char common_flags",0);
    COMMON(preempt_count,"unsigned char common_preempt_count",0);COMMON(pid,"int common_pid",1);
#undef COMMON
    p=text(p,"\n");p=field(p,"long id",offsetof(struct kernel_trace_sys_enter,id),sizeof(long),1);
    if(enter)p=field(p,"unsigned long args[6]",offsetof(struct kernel_trace_sys_enter,args),sizeof(((struct kernel_trace_sys_enter *)0)->args),0);
    else p=field(p,"long ret",offsetof(struct kernel_trace_sys_exit,ret),sizeof(long),1);
    p=text(p,enter?"\nprint fmt: \"NR %ld (%lx, %lx, %lx, %lx, %lx, %lx)\", REC->id, REC->args[0], REC->args[1], REC->args[2], REC->args[3], REC->args[4], REC->args[5]\n":"\nprint fmt: \"NR %ld = %ld\", REC->id, REC->ret\n");
    return (size_t)(p-start);
}
int kernel_tracefs_mount(struct kernel_heap *heap,uint64_t flags,struct kernel_vfs_mount **out)
{
    char enter[2048],exit[2048],enter_id[24],exit_id[24];
    size_t en=format(enter,1),ex=format(exit,0);
    char *e=number(enter_id,KERNEL_TRACE_SYS_ENTER);*e++='\n';
    char *x=number(exit_id,KERNEL_TRACE_SYS_EXIT);*x++='\n';
    static const char events[]="raw_syscalls:sys_enter\nraw_syscalls:sys_exit\n";
#define DIR(p,n) {p,n,0,0,KERNEL_VFS_S_IFDIR|0555}
#define FILE(p,n,d,s) {p,n,d,s,KERNEL_VFS_S_IFREG|0444}
    const struct bpf_fs_entry entries[]={
        FILE(1,"available_events",events,sizeof(events)-1), /* 2 */
        DIR(1,"events"),DIR(3,"raw_syscalls"), /* 3,4 */
        DIR(4,"sys_enter"),FILE(5,"id",enter_id,(size_t)(e-enter_id)),FILE(5,"format",enter,en),
        DIR(4,"sys_exit"),FILE(8,"id",exit_id,(size_t)(x-exit_id)),FILE(8,"format",exit,ex),
    };
#undef DIR
#undef FILE
    return bpf_metadata_mount(heap,flags,0x74726163,entries,sizeof(entries)/sizeof(entries[0]),out);
}
