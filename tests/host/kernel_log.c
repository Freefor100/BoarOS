#include <kernel/log.h>
#include <kernel/errno.h>
#include <kernel/sync.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static char scratch[KERNEL_LOG_RECORD_MAX], output[32768];
static int copy_fault, copy_overwrite, block_mode;
void kernel_wait_queue_init(struct kernel_wait_queue *q) { memset(q,0,sizeof(*q));q->initialized=KERNEL_WAIT_QUEUE_INITIALIZED; }
enum kernel_scheduler_status kernel_wait_queue_wake_all(struct kernel_wait_queue *q){(void)q;return KERNEL_SCHEDULER_STATUS_OK;}
void kernel_rwlock_init(struct kernel_rwlock *l,uint32_t rank,uintptr_t key){memset(l,0,sizeof(*l));l->rank=rank;l->key=key;}
void kernel_rwlock_write(struct kernel_rwlock *l,struct kernel_lock_guard *g){assert(!g->lock);g->lock=l;}
void kernel_lock_release(struct kernel_lock_guard *g){assert(g->lock);g->lock=0;}
static void message(const char *s){while(*s)kernel_log_putc(6,*s++);}
enum kernel_scheduler_status kernel_scheduler_block_current(struct kernel_wait_queue *q,uint64_t d,int i,enum kernel_wait_wake_reason *reason){(void)q;(void)d;assert(i);if(block_mode){message("wake\n");*reason=KERNEL_WAIT_WOKEN;}else *reason=KERNEL_WAIT_SIGNALLED;return KERNEL_SCHEDULER_STATUS_OK;}
static int copy(void *c,size_t offset,const char *data,size_t n){(void)c;if(copy_fault)return 1;memcpy(output+offset,data,n);if(copy_overwrite){copy_overwrite=0;for(unsigned i=0;i<3000;i++)message("overwrite\n");}return 0;}
static int action(int a,int n){return kernel_log_action(a,n,1,copy,0,scratch);}
int main(void){
 assert(action(10,0)==16384);assert(action(11,0)==-KERNEL_EINVAL);
 assert(kernel_log_action(2,1,0,copy,0,scratch)==-KERNEL_EPERM);
 assert(kernel_log_action(3,0,0,copy,0,scratch)==0);
 assert(action(8,0)==-KERNEL_EINVAL);assert(action(8,9)==-KERNEL_EINVAL);
 message("one\n");message("two\n");assert(action(3,7)==7 && !memcmp(output,"<6>two\n",7));
 assert(action(3,6)==0);assert(action(2,4)==4 && !memcmp(output,"<6>o",4));assert(action(9,0)==10);
 assert(action(2,100)==10);assert(action(9,0)==0);assert(action(2,10)==-KERNEL_EINTR);
 block_mode=1;assert(action(2,10)==8 && !memcmp(output,"<6>wake\n",8));block_mode=0;
 assert(action(4,100)==22);assert(action(3,100)==0);message("fault\n");copy_fault=1;assert(action(3,100)==-KERNEL_EFAULT);
 assert(action(2,100)==-KERNEL_EFAULT);copy_fault=0;assert(action(9,0)==0);assert(action(5,0)==0);
 assert(action(8,1)==0);assert(!kernel_log_putc(6,'x'));assert(kernel_log_putc(0,'!'));kernel_log_putc(0,'\n');assert(action(6,0)==0);assert(action(7,0)==0);assert(action(8,8)==0);assert(kernel_log_putc(6,'\n'));
 copy_overwrite=1;int n=action(3,sizeof(output));assert(n>0 && n<=16384);n=action(3,sizeof(output));assert(n>0 && n<=16384);assert(memmem(output,n,"overwrite",9));
 puts("PASS: kernel log ring, whole records, overwrite during copy, consumption/fault, clear, wake/signal and permissions");return 0;
}

#include "wait_boundary.h"
HOST_WAIT_BOUNDARY(, kernel_scheduler_block_current)
