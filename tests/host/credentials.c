/* Production credential publication and syscall decoding; the fixture replaces
 * only allocation, user-copy and scheduler ownership. */
#include <kernel/credentials.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/task_state.h>
#include <kernel/uaccess.h>
#include <kernel/wait_internal.h>
#include <kernel/vfs.h>
#include "../../kernel/syscall/private.h"
#include "../../kernel/exec_internal.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum kernel_syscall_status syscall_handle_credentials(struct kernel_task *,const struct kernel_syscall_request *,struct kernel_syscall_result *);
enum kernel_syscall_status syscall_handle_capset(struct kernel_task *,const struct kernel_syscall_request *,struct kernel_syscall_result *);
enum kernel_syscall_status syscall_handle_prctl(struct kernel_task *,const struct kernel_syscall_request *,struct kernel_syscall_result *);
struct kernel_raw_lock kernel_wait_domain;
_Thread_local uintptr_t sync_test_irq=1;
static _Thread_local struct kernel_cpu cpu={.initialized=KERNEL_CPU_INITIALIZED};
static _Thread_local struct kernel_task *running;
static struct kernel_task tasks[2];
static struct kernel_heap heap;
static unsigned allocated,fail_next,changed;
static pthread_barrier_t *allocation_barrier;
static int dumpable=1;
static uintptr_t user_fault;
void *sync_test_cpu(void){return &cpu;}
void *sync_test_current_task(void){return running;}
void kernel_cpu_flush_notifications(void){assert(!cpu.outbound);}
struct kernel_task *kernel_task_current(void){return running;}
struct kernel_task *kernel_task_find_by_tid(kernel_pid_t tid)
{for(unsigned i=0;i<2;i++)if(tasks[i].tid==tid){tasks[i].storage_borrows++;return &tasks[i];}return 0;}
void kernel_task_lookup_put(struct kernel_task *t){assert(t->storage_borrows);t->storage_borrows--;}
void kernel_credentials_changed_locked(struct kernel_task *t,const struct kernel_credentials *old,const struct kernel_credentials *replacement)
{(void)t;assert(cpu.raw_locks);if(old->euid!=replacement->euid||old->fsuid!=replacement->fsuid)dumpable=0;changed++;}
int kernel_credentials_dumpable_get(struct kernel_task *t){(void)t;return dumpable;}
void kernel_credentials_dumpable_set(struct kernel_task *t,unsigned value){(void)t;dumpable=(int)value;}
enum kernel_heap_status kernel_heap_allocate(struct kernel_heap *h,size_t size,void **out)
{assert(h==&heap&&!cpu.raw_locks);if(fail_next&&!--fail_next)return KERNEL_HEAP_STATUS_EMPTY;*out=malloc(size);assert(*out);__atomic_add_fetch(&allocated,1,__ATOMIC_RELAXED);if(allocation_barrier)pthread_barrier_wait(allocation_barrier);return KERNEL_HEAP_STATUS_OK;}
enum kernel_heap_status kernel_heap_allocate_zeroed(struct kernel_heap *h,size_t n,size_t size,void **out)
{enum kernel_heap_status status=kernel_heap_allocate(h,n*size,out);if(!status)memset(*out,0,n*size);return status;}
void kernel_heap_release(struct kernel_heap *h,void *p){assert(h==&heap&&!cpu.raw_locks&&p);free(p);assert(__atomic_fetch_sub(&allocated,1,__ATOMIC_RELAXED));}
enum kernel_task_status kernel_task_mm_borrow_mutable(struct kernel_task *t,struct kernel_mm **out)
{(void)t;*out=(struct kernel_mm *)(uintptr_t)1;return KERNEL_TASK_STATUS_OK;}
static enum kernel_uaccess_status copy_user(void *dest,const void *src,size_t len,size_t *done,int output)
{
    uintptr_t address=(uintptr_t)(output?dest:src);size_t n=len;
    assert(!cpu.raw_locks);
    if(!address||address==UINTPTR_MAX)n=0;
    else if(user_fault&&address<user_fault&&len>user_fault-address)n=user_fault-address;
    else if(user_fault&&address>=user_fault)n=0;
    if(n)memcpy(dest,src,n);
    *done=n;
    return n==len?KERNEL_UACCESS_STATUS_OK:KERNEL_UACCESS_STATUS_FAULT;
}
enum kernel_uaccess_status kernel_copy_from_user(struct kernel_mm *mm,void *dest,uint64_t src,size_t len,size_t *done)
{(void)mm;return copy_user(dest,(void *)(uintptr_t)src,len,done,0);}
enum kernel_uaccess_status kernel_copy_to_user(struct kernel_mm *mm,uint64_t dest,const void *src,size_t len,size_t *done)
{(void)mm;return copy_user((void *)(uintptr_t)dest,src,len,done,1);}
static long call(unsigned nr,uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e)
{
    struct kernel_syscall_request req={.number=nr,.arguments={a,b,c,d,e}};
    struct kernel_syscall_result result;
    enum kernel_syscall_status status=nr==90?syscall_handle_capget(running,&req,&result):
        nr==91?syscall_handle_capset(running,&req,&result):nr==167?syscall_handle_prctl(running,&req,&result):syscall_handle_credentials(running,&req,&result);
    assert(status==KERNEL_SYSCALL_STATUS_OK&&result.action==KERNEL_SYSCALL_ACTION_RETURN);return result.value;
}
#define CALL(n,a,b,c) call(n,(uint64_t)(uintptr_t)(a),(uint64_t)(uintptr_t)(b),(uint64_t)(uintptr_t)(c),0,0)
static void reset(void)
{
    for(unsigned i=0;i<2;i++){kernel_credentials_put(tasks[i].credentials);tasks[i]=(struct kernel_task){.tid=(kernel_pid_t)(i+10),.credentials=kernel_credentials_initial()};tasks[i].files.heap=&heap;}
    running=&tasks[0];user_fault=fail_next=changed=0;dumpable=1;
    assert(!allocated);
}
static void ids(void)
{
    reset();const struct kernel_credentials *root=running->credentials;
    assert(CALL(174,0,0,0)==0&&CALL(175,0,0,0)==0);
    assert(CALL(146,-1,0,0)==-KERNEL_EINVAL);
    assert(CALL(147,-1,-1,-1)==0&&running->credentials==root);
    assert(CALL(151,1001,0,0)==0&&running->credentials->fsuid==1001);
    assert(!(running->credentials->effective&(UINT64_C(1)<<KERNEL_CAP_DAC_OVERRIDE)));
    assert(CALL(151,0,0,0)==1001&&running->credentials->effective==KERNEL_CAP_VALID_MASK);
    assert(CALL(147,1000,1001,1002)==0);
    assert(!running->credentials->effective&&!running->credentials->permitted&&running->credentials->account->uid==1000);
    assert(CALL(147,1001,1002,1000)==0);
    assert(CALL(145,1000,-1,0)==-KERNEL_EPERM); /* saved ID is not permitted as reuid's real ID. */
    assert(CALL(146,1000,0,0)==0&&running->credentials->euid==1000);
    assert(CALL(146,0,0,0)==-KERNEL_EPERM);
    assert(CALL(151,777,0,0)==1000&&running->credentials->fsuid==1000);
    uint32_t result[3]={99,99,99};user_fault=(uintptr_t)&result[1];
    assert(CALL(148,&result[0],&result[1],&result[2])==-KERNEL_EFAULT&&result[0]==1001&&result[1]==99);
    user_fault=0;
    reset();assert(CALL(149,100,101,102)==0&&CALL(144,102,0,0)==0);
    assert(running->credentials->gid==102&&running->credentials->sgid==102);
    assert(CALL(143,300,301,0)==0&&running->credentials->sgid==301);
    assert(CALL(152,302,0,0)==301&&running->credentials->fsgid==302);
    reset();assert(CALL(167,8,1,0)==0&&CALL(146,1000,0,0)==0);
    assert(!running->credentials->effective&&running->credentials->permitted==KERNEL_CAP_VALID_MASK);
    reset();assert(CALL(167,28,KERNEL_SECBIT_NO_SETUID_FIXUP,0)==0&&CALL(146,1000,0,0)==0);
    assert(running->credentials->effective==KERNEL_CAP_VALID_MASK);
}
static void groups(void)
{
    reset();uint32_t input[]={9,2,9,7},output[4]={0};
    assert(CALL(159,4,input,0)==0&&CALL(158,0,0,0)==4);
    assert(CALL(158,3,output,0)==-KERNEL_EINVAL&&CALL(158,4,output,0)==4);
    assert(output[0]==2&&output[1]==7&&output[2]==9&&output[3]==9);
    assert(kernel_credentials_in_group(running->credentials,7)&&!kernel_credentials_in_group(running->credentials,3));
    const struct kernel_credentials *old=running->credentials;input[0]=UINT32_MAX;user_fault=(uintptr_t)&input[1];
    assert(CALL(159,4,input,0)==-KERNEL_EINVAL&&running->credentials==old);user_fault=0;
    assert(CALL(159,65537,0,0)==-KERNEL_EINVAL);
    fail_next=1;assert(CALL(159,0,0,0)==-KERNEL_ENOMEM&&running->credentials==old);
    assert(CALL(146,1000,0,0)==0&&CALL(159,-1,0,0)==-KERNEL_EPERM);
    reset();assert(CALL(159,0,-1,0)==0&&CALL(158,0,0,0)==0);
}
struct cap_header{uint32_t version;int32_t pid;};
struct cap_data{uint32_t e,p,i;};
static void caps(void)
{
    reset();struct cap_header header={0x20080522,0};struct cap_data data[2]={{0}};
    assert(CALL(90,&header,data,0)==0&&data[0].e==UINT32_MAX&&data[1].e==511);
    header.version=0;assert(CALL(91,&header,0,0)==-KERNEL_EINVAL&&header.version==0x20080522);
    header.pid=-1;assert(CALL(91,&header,0,0)==-KERNEL_EPERM);header.pid=0;
    assert(CALL(91,&header,0,0)==-KERNEL_EFAULT);
    data[0].i=UINT32_C(1)<<10;assert(CALL(91,&header,data,0)==0);
    assert(CALL(167,47,2,10)==0&&running->credentials->ambient==(UINT64_C(1)<<10));
    assert(CALL(167,24,10,0)==0&&!(running->credentials->bounding&(UINT64_C(1)<<10)));
    assert(running->credentials->effective&(UINT64_C(1)<<10));
    data[0].p&=~(UINT32_C(1)<<10);data[0].e=data[0].p;
    assert(CALL(91,&header,data,0)==0&&!running->credentials->ambient);
    data[0].p|=UINT32_C(1)<<10;assert(CALL(91,&header,data,0)==-KERNEL_EPERM);
    reset();header.version=0x19980330;data[0]=(struct cap_data){1,1,0};
    assert(CALL(91,&header,data,0)==0&&running->credentials->effective==1);
    assert(CALL(167,24,99,0)==-KERNEL_EPERM); /* permission precedes cap number */
    assert(CALL(167,28,0,0)==-KERNEL_EPERM);
    assert(CALL(167,28,0x300,0)==0&&CALL(167,28,0,0)==-KERNEL_EPERM);
    assert(CALL(167,38,1,0)==0&&CALL(167,39,0,0)==1);
    assert(call(167,39,0,1,0,0)==-KERNEL_EINVAL);
    reset();assert(CALL(167,28,0x20,0)==0&&CALL(167,8,1,0)==-KERNEL_EPERM);
}
static void ownership(void)
{
    reset();assert(CALL(147,100,0,0)==0);
    const struct kernel_credentials *held=kernel_credentials_snapshot(running);
    struct kernel_uid_account *account;
    {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);account=kernel_uid_account_get_locked(held->account);account->pending++;}
    tasks[1].credentials=kernel_credentials_get(held);
    assert(CALL(147,101,0,0)==0&&held->uid==100&&tasks[1].credentials==held);
    kernel_credentials_put(held);
    assert(account->pending==1&&account->uid==100);
    struct kernel_uid_account *found=kernel_uid_account_get(&heap,100);assert(found==account);kernel_uid_account_put(found);
    {KERNEL_RAW_SCOPE(g,&kernel_wait_domain);account->pending--;}
    kernel_uid_account_put(account);
    const struct kernel_credentials *old=running->credentials;fail_next=1;
    assert(CALL(147,102,0,0)==-KERNEL_ENOMEM&&running->credentials==old);
    fail_next=2;assert(CALL(147,102,0,0)==-KERNEL_ENOMEM&&running->credentials==old);
    const struct kernel_credentials *before=kernel_credentials_override(tasks[1].credentials);
    assert(kernel_credentials_current()->uid==100);kernel_credentials_override(before);
    assert(kernel_credentials_current()->uid==101);
    reset();assert(!allocated);
}
static pthread_barrier_t rendezvous;
static struct kernel_uid_account *raced[2];
static void *account_race(void *opaque)
{
    uintptr_t index=(uintptr_t)opaque;
    raced[index]=kernel_uid_account_get(&heap,12345);
    assert(raced[index]);return 0;
}
static void *snapshot_race(void *unused)
{
    (void)unused;
    const struct kernel_credentials *held=kernel_credentials_snapshot(&tasks[0]);
    assert(held->uid==100);
    pthread_barrier_wait(&rendezvous);
    pthread_barrier_wait(&rendezvous);
    assert(held->uid==100&&held->account->uid==100);
    const struct kernel_credentials *now=kernel_credentials_snapshot(&tasks[0]);
    assert(now->uid==101);kernel_credentials_put(now);kernel_credentials_put(held);
    return 0;
}
static void concurrent_publication(void)
{
    reset();pthread_t workers[2];pthread_barrier_init(&rendezvous,0,2);
    allocation_barrier=&rendezvous;
    assert(!pthread_create(&workers[0],0,account_race,(void *)0));
    assert(!pthread_create(&workers[1],0,account_race,(void *)1));
    assert(!pthread_join(workers[0],0)&&!pthread_join(workers[1],0));
    allocation_barrier=0;assert(raced[0]==raced[1]&&raced[0]->references==2);
    kernel_uid_account_put(raced[0]);kernel_uid_account_put(raced[1]);assert(!allocated);
    assert(CALL(147,100,0,0)==0);
    assert(!pthread_create(&workers[0],0,snapshot_race,0));
    pthread_barrier_wait(&rendezvous);
    assert(CALL(147,101,0,0)==0);
    pthread_barrier_wait(&rendezvous);
    assert(!pthread_join(workers[0],0));pthread_barrier_destroy(&rendezvous);reset();
}
static void little_word(uint8_t *p,uint32_t value)
{for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(value>>(8*i));}
static void filecap(struct kernel_vfs_exec_metadata *file,unsigned revision,uint64_t p,uint64_t i,int effective)
{
    file->filecap_present=1;file->filecap_size=revision==1?12:revision==2?20:24;
    memset(file->filecap,0,sizeof(file->filecap));little_word(file->filecap,(revision<<24)|(unsigned)effective);
    little_word(file->filecap+4,(uint32_t)p);little_word(file->filecap+8,(uint32_t)i);
    if(revision>1){little_word(file->filecap+12,(uint32_t)(p>>32));little_word(file->filecap+16,(uint32_t)(i>>32));}
}
static void exec_rules(void)
{
    struct kernel_credentials old=*kernel_credentials_initial(),next;
    struct kernel_vfs_exec_metadata file={.mode=0100755,.readable=1};
    unsigned secure,dump,nnp=0;
    old.uid=old.euid=old.suid=old.fsuid=1000;old.permitted=old.effective=0;
    for(unsigned rev=1;rev<=3;rev++){
        filecap(&file,rev,UINT64_C(1)<<10,0,1);next=old;
        assert(!kernel_exec_credentials_prepare(&next,&old,&file,0,nnp,&secure,&dump));
        assert(next.permitted==(UINT64_C(1)<<10)&&next.effective==next.permitted&&secure&&!dump);
    }
    filecap(&file,3,1,0,1);little_word(file.filecap+20,1000);next=old;
    assert(!kernel_exec_credentials_prepare(&next,&old,&file,0,nnp,&secure,&dump)&&!next.permitted&&!secure&&dump);
    filecap(&file,2,1,0,1);file.filecap_size=19;next=old;
    assert(kernel_exec_credentials_prepare(&next,&old,&file,0,nnp,&secure,&dump)==-KERNEL_EINVAL);
    filecap(&file,4,0,0,0);next=old;
    assert(kernel_exec_credentials_prepare(&next,&old,&file,0,nnp,&secure,&dump)==-KERNEL_EINVAL);
    filecap(&file,2,1,0,1);old.bounding=0;next=old;
    assert(kernel_exec_credentials_prepare(&next,&old,&file,0,nnp,&secure,&dump)==-KERNEL_EPERM);
    old.inheritable=1;filecap(&file,2,1,1,1);next=old;
    assert(!kernel_exec_credentials_prepare(&next,&old,&file,0,nnp,&secure,&dump)&&next.permitted==1);
    filecap(&file,2,UINT64_C(1)<<63,0,1);next=old;
    assert(!kernel_exec_credentials_prepare(&next,&old,&file,0,nnp,&secure,&dump)&&!next.permitted&&secure);
    old.bounding=KERNEL_CAP_VALID_MASK;old.permitted=old.inheritable=old.ambient=1;
    file.filecap_present=0;next=old;
    assert(!kernel_exec_credentials_prepare(&next,&old,&file,0,nnp,&secure,&dump)&&next.effective==1&&!secure&&dump);
    filecap(&file,2,0,0,0);next=old;
    assert(!kernel_exec_credentials_prepare(&next,&old,&file,0,nnp,&secure,&dump)&&!next.ambient&&!next.permitted&&!secure);
    filecap(&file,2,2,0,1);nnp=1;next=old;
    assert(!kernel_exec_credentials_prepare(&next,&old,&file,0,nnp,&secure,&dump)&&!next.permitted&&secure&&dump);
    nnp=0;file.filecap_present=0;file.mode=0104755;file.uid=0;next=old;
    assert(!kernel_exec_credentials_prepare(&next,&old,&file,0,nnp,&secure,&dump)&&!next.euid&&next.permitted==KERNEL_CAP_VALID_MASK&&secure&&!dump);
    filecap(&file,2,2,0,1);next=old;
    assert(!kernel_exec_credentials_prepare(&next,&old,&file,0,nnp,&secure,&dump)&&!next.euid&&next.permitted==2);
    file.filecap_present=0;file.nosuid=1;next=old;
    assert(!kernel_exec_credentials_prepare(&next,&old,&file,0,nnp,&secure,&dump)&&next.euid==1000&&next.ambient==1&&!secure);
    file.nosuid=0;next=old;
    assert(!kernel_exec_credentials_prepare(&next,&old,&file,1,nnp,&secure,&dump)&&next.euid==1000&&next.permitted==1&&!next.ambient&&secure&&dump);
    old=*kernel_credentials_initial();old.securebits=KERNEL_SECBIT_NOROOT;next=old;
    assert(!kernel_exec_credentials_prepare(&next,&old,&file,0,nnp,&secure,&dump)&&!next.permitted&&!next.effective);
    old.securebits=KERNEL_SECBIT_KEEP_CAPS|KERNEL_SECBIT_KEEP_CAPS_LOCKED;next=old;
    assert(!kernel_exec_credentials_prepare(&next,&old,&file,0,nnp,&secure,&dump)&&next.securebits==KERNEL_SECBIT_KEEP_CAPS_LOCKED);
    file.mode=0100755;file.readable=0;next=old;
    assert(!kernel_exec_credentials_prepare(&next,&old,&file,0,nnp,&secure,&dump)&&!secure&&!dump);
    file.readable=1;old.fsuid=2000;next=old;
    assert(!kernel_exec_credentials_prepare(&next,&old,&file,0,nnp,&secure,&dump)&&!secure&&!dump&&next.fsuid==0);
    old=*kernel_credentials_initial();old.uid=1000;old.euid=1001;old.fsuid=1001;old.permitted=old.effective=0;
    file.mode=0104755;file.uid=1000;next=old;
    assert(!kernel_exec_credentials_prepare(&next,&old,&file,0,nnp,&secure,&dump)&&next.euid==1000&&secure&&!dump);
    old=*kernel_credentials_initial();old.fsgid=2000;file.mode=0102755;file.gid=2000;next=old;
    assert(!kernel_exec_credentials_prepare(&next,&old,&file,0,nnp,&secure,&dump)&&next.egid==2000&&!next.ambient&&secure&&!dump);
}
static void exec_security_lifetime(void)
{
    reset();
    fail_next=1;
    assert(!kernel_exec_security_create(&heap,1));
    struct kernel_exec_security *image=kernel_exec_security_create(&heap,1);
    assert(image && kernel_exec_security_dumpable(image)==1);
    struct kernel_exec_security *thread=kernel_exec_security_get(image);
    struct kernel_exec_security *child=kernel_exec_security_create(&heap,kernel_exec_security_dumpable(image));
    assert(child);
    kernel_exec_security_set_dumpable(thread,0);
    assert(!kernel_exec_security_dumpable(image) && kernel_exec_security_dumpable(child)==1);
    /* 页表及执行者退出不影响独立任务引用，fork 的状态继续独立。 */
    kernel_exec_security_put(image);
    assert(!kernel_exec_security_dumpable(thread));
    kernel_exec_security_set_dumpable(thread,1);
    kernel_exec_security_put(thread);
    kernel_exec_security_put(child);
    assert(!allocated);
}
int main(void)
{
    kernel_raw_lock_init(&kernel_wait_domain,KERNEL_RAW_RANK_SCHEDULER);
    ids();groups();caps();ownership();concurrent_publication();exec_rules();exec_security_lifetime();reset();
    puts("credentials: IDs, groups, capability/prctl, fault prefixes, account ownership OK");return 0;
}
