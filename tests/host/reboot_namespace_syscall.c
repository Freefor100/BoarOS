#include "../../kernel/syscall/process.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static struct kernel_task task;
static struct kernel_credentials credentials;
static int capable=1,nested=1,copies,requests,namespace_calls;
const struct kernel_credentials *kernel_credentials_current(void){return &credentials;}
int kernel_credentials_capable(const struct kernel_credentials *c,unsigned cap)
{assert(c==&credentials&&cap==KERNEL_CAP_SYS_BOOT);return capable;}
int kernel_task_reboot_namespace(struct kernel_task *t,enum kernel_reboot_action a)
{assert(t==&task&&a>=KERNEL_REBOOT_RESTART&&a<=KERNEL_REBOOT_POWER_OFF);namespace_calls++;return nested;}
int __wrap_kernel_reboot_request(enum kernel_reboot_action a,const char *command)
{assert(!nested&&a==KERNEL_REBOOT_RESTART&&!strcmp(command,"machine"));requests++;return 0;}
enum kernel_task_status kernel_task_mm_borrow_mutable(struct kernel_task *t,struct kernel_mm **mm)
{assert(t==&task);*mm=NULL;return KERNEL_TASK_STATUS_OK;}
enum kernel_uaccess_status kernel_copy_string_from_user(struct kernel_mm *mm,char *out,uint64_t address,size_t size,size_t *length)
{assert(!mm&&size>=8);copies++;if(address==1)return KERNEL_UACCESS_STATUS_FAULT;strcpy(out,"machine");*length=7;return KERNEL_UACCESS_STATUS_OK;}
static struct kernel_syscall_result call(uint32_t magic,uint32_t cmd,uint64_t address)
{struct kernel_syscall_request r={.arguments={magic,672274793,cmd,address}};struct kernel_syscall_result d={0};assert(!syscall_handle_reboot(&task,&r,&d));return d;}
int main(void)
{
    struct kernel_syscall_result d;
    capable=0;d=call(0,KERNEL_REBOOT_CMD_RESTART2,1);assert(d.value==-KERNEL_EPERM&&!namespace_calls&&!copies&&!requests);
    capable=1;d=call(0,KERNEL_REBOOT_CMD_RESTART2,1);assert(d.value==-KERNEL_EINVAL&&!namespace_calls);
    d=call(0xfee1dead,0,1);assert(d.value==-KERNEL_EINVAL&&!namespace_calls);
    const uint32_t commands[]={KERNEL_REBOOT_CMD_RESTART,KERNEL_REBOOT_CMD_RESTART2,KERNEL_REBOOT_CMD_HALT,KERNEL_REBOOT_CMD_POWER_OFF};
    for(unsigned i=0;i<4;i++){d=call(0xfee1dead,commands[i],1);assert(!d.value&&d.action==KERNEL_SYSCALL_ACTION_EXIT&&!copies&&!requests);}
    nested=-KERNEL_ESRCH;d=call(0xfee1dead,KERNEL_REBOOT_CMD_RESTART2,1);assert(d.value==-KERNEL_ESRCH&&d.action==KERNEL_SYSCALL_ACTION_RETURN&&!copies&&!requests);
    nested=0;d=call(0xfee1dead,KERNEL_REBOOT_CMD_RESTART2,1);assert(d.value==-KERNEL_EFAULT&&copies==1&&!requests);
    d=call(0xfee1dead,KERNEL_REBOOT_CMD_RESTART2,2);assert(!d.value&&d.action==KERNEL_SYSCALL_ACTION_EXIT_GROUP&&copies==2&&requests==1);
    puts("PASS reboot syscall capability/magic/action order, nested no RESTART2 copy or global request, caller thread exit");return 0;
}
