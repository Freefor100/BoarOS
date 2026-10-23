#include "signal_pending.h"
#include <kernel/task_state.h>
#include <kernel/credentials.h>
#include <kernel/heap.h>
#include <kernel/errno.h>
#include <kernel/wait_internal.h>
#include <string.h>

_Static_assert(sizeof(struct kernel_signal_wait_info)==128,"native siginfo_t");
struct kernel_signal_queue {
    struct kernel_heap *heap;
    uint64_t inline_pending,native_count;
    struct { struct signal_queue_record *head,*tail; uint64_t count; } slots[64];
};
uint64_t kernel_signal_pending_count_locked(const struct kernel_task *task)
{ return kernel_credentials_task_locked(task)->account->pending; }
static void uncharge(struct kernel_uid_account *account,uint64_t count)
{ if(!account || account->pending<count)__builtin_trap();account->pending-=count; }
static void retire_inline_account(struct kernel_signal_pending_state *s,struct signal_pending_prepared *p)
{
    if(s->inline_charged || !s->inline_account)return;
    for(unsigned i=0;i<2;i++)if(!p->retired_accounts[i]) {
        p->retired_accounts[i]=s->inline_account;s->inline_account=0;return;
    }
    __builtin_trap();
}
static struct kernel_signal_pending_state *state(struct kernel_task *t,int shared)
{ return shared ? &t->group_queue : &t->signal_queue; }
static uint64_t *bits(struct kernel_task *t,int shared)
{ return shared ? &t->group_pending : &t->signal_pending; }

void signal_pending_prepare_record(struct kernel_heap *heap,struct signal_pending_prepared *p)
{
    if(!heap)return;
    enum kernel_heap_status status=kernel_heap_allocate_zeroed(heap,1,sizeof(*p->record),(void **)&p->record);
    if(!status)p->record->heap=heap;
    else if(status!=KERNEL_HEAP_STATUS_EMPTY)__builtin_trap();
}
void signal_pending_prepare_index(struct kernel_task *t,int shared,struct kernel_heap *heap,
    struct signal_pending_prepared *p)
{
    int needs_index;
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        needs_index=!state(t,shared)->queue;
    }
    if(!heap)return;
    if(needs_index){
        enum kernel_heap_status status=kernel_heap_allocate_zeroed(heap,1,sizeof(*p->index),(void **)&p->index);
        if(!status)p->index->heap=heap;
        else if(status!=KERNEL_HEAP_STATUS_EMPTY)__builtin_trap();
    }
}
void signal_pending_prepare(struct kernel_task *t,int shared,const struct kernel_task *charge_target,struct kernel_heap *heap,
    const struct kernel_signal_wait_info *info,int supplied,struct signal_pending_prepared *p)
{
    if(info->signal<32 && !supplied && !info->uid) {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        struct kernel_signal_pending_state *s=state(t,shared);
        if(!s->inline_account || s->inline_account==kernel_credentials_task_locked(charge_target)->account)return;
    }
    signal_pending_prepare_index(t,shared,heap,p);
    signal_pending_prepare_record(heap,p);
}
int signal_pending_reserve_locked(const struct kernel_task *task,struct kernel_uid_account **owner)
{
    struct kernel_uid_account *account=kernel_credentials_task_locked(task)->account;
    if(account->pending>=task->group_leader->sigpending_limit.current)return -KERNEL_EAGAIN;
    if(account->pending==UINT64_MAX)__builtin_trap();
    account->pending++;*owner=kernel_uid_account_get_locked(account);return 0;
}
void signal_pending_unreserve_locked(struct kernel_uid_account *account)
{ uncharge(account,1); }

void signal_pending_install_index_locked(struct kernel_task *task,int shared,struct signal_pending_prepared *p)
{
    struct kernel_signal_pending_state *s=state(task,shared);
    if(!s->queue){if(!p->index)__builtin_trap();s->queue=p->index;p->index=0;s->queue->inline_pending=*bits(task,shared);}
}
void signal_pending_post_timer_locked(struct kernel_task *t,int shared,struct signal_queue_record *record)
{
    struct kernel_signal_queue *q=state(t,shared)->queue;unsigned slot=record->info.signal-1;
    if(!q||!record->timer)__builtin_trap();
    record->next=0;
    if(q->slots[slot].tail)q->slots[slot].tail->next=record;else q->slots[slot].head=record;
    q->slots[slot].tail=record;q->slots[slot].count++;q->native_count++;
    *bits(t,shared)|=UINT64_C(1)<<slot;
}
int signal_pending_needs_record_locked(struct kernel_task *t,int shared,const struct kernel_task *charge_target,
    const struct kernel_signal_wait_info *info,int supplied)
{
    struct kernel_signal_pending_state *s=state(t,shared);
    return info->signal>=32 || supplied || info->uid ||
        (s->inline_account && s->inline_account!=kernel_credentials_task_locked(charge_target)->account);
}
int signal_pending_post_locked(struct kernel_task *t,int shared,const struct kernel_task *charge_target,
    const struct kernel_signal_wait_info *info,int supplied,struct signal_pending_prepared *p)
{
    unsigned sig=info->signal;uint64_t bit=UINT64_C(1)<<(sig-1);
    struct kernel_signal_pending_state *s=state(t,shared);
    uint64_t *pending=bits(t,shared);
    if(sig<32 && (*pending&bit))return 1;
    int override=sig<32 && info->code>=0;
    struct kernel_uid_account *account=kernel_credentials_task_locked(charge_target)->account;
    int quota=override || account->pending<t->group_leader->sigpending_limit.current;
    int wants_record=signal_pending_needs_record_locked(t,shared,charge_target,info,supplied);
    int available=quota && (!wants_record || (p->record && (s->queue || p->index)));
    if(!available && sig>=32 && info->code!=0 && !(info->code==128 && !supplied))
        return -KERNEL_EAGAIN;
    if(!available&&(*pending&bit))return 1;
    if(available) {
        if(account->pending==UINT64_MAX)__builtin_trap();
        account->pending++;
        if(wants_record) {
            signal_pending_install_index_locked(t,shared,p);
            struct signal_queue_record *record=p->record;p->record=0;record->info=*info;
            record->account=kernel_uid_account_get_locked(account);
            unsigned slot=sig-1;
            if(s->queue->slots[slot].tail)s->queue->slots[slot].tail->next=record;
            else s->queue->slots[slot].head=record;
            s->queue->slots[slot].tail=record;s->queue->slots[slot].count++;
        } else {
            if(!s->inline_account)s->inline_account=kernel_uid_account_get_locked(account);
            s->inline_charged|=bit;
        }
    }
    if((!available||!wants_record)&&s->queue)s->queue->inline_pending|=bit;
    if(!(*pending&bit)) {
        uint32_t *senders=shared?t->group_sender:t->signal_sender;
        int16_t *codes=shared?t->group_signal_code:t->signal_code;
        senders[sig-1]=available?info->sender:0;codes[sig-1]=available?(int16_t)info->code:0;
        if(sig==17) {
            struct kernel_signal_child_info child={.uid=available?info->uid:0,.status=available?info->status:0};
            if(shared)t->group_child_info=child;else t->signal_child_info=child;
            uint64_t *times=shared?t->group_child_times:t->signal_child_times;
            times[0]=available?info->user_time:0;times[1]=available?info->system_time:0;
        }
    }
    *pending|=bit;return 0;
}

void signal_pending_take_locked(struct kernel_task *t,int shared,unsigned sig,
    struct kernel_signal_wait_info *out,struct signal_pending_prepared *p)
{
    struct kernel_signal_pending_state *s=state(t,shared);uint64_t bit=UINT64_C(1)<<(sig-1);
    if(!(*bits(t,shared)&bit))__builtin_trap();
    if(s->queue && !(s->queue->inline_pending&bit) && s->queue->slots[sig-1].head) {
        struct signal_queue_record *record=s->queue->slots[sig-1].head;
        *out=record->info;s->queue->slots[sig-1].head=record->next;
        if(!s->queue->slots[sig-1].head)s->queue->slots[sig-1].tail=0;
        if(!s->queue->slots[sig-1].count)__builtin_trap();
        s->queue->slots[sig-1].count--;
        if(record->timer){
            if(!s->queue->native_count)__builtin_trap();
            s->queue->native_count--;posix_timer_record_take_locked(record,out,p);
        } else {
            uncharge(record->account,1);
            record->next=p->retired;p->retired=record;
        }
        if(s->queue->slots[sig-1].head)return;
    } else {
        memset(out,0,sizeof(*out));out->signal=sig;
        if(s->queue)s->queue->inline_pending&=~bit;
        out->sender=shared?t->group_sender[sig-1]:t->signal_sender[sig-1];
        out->code=shared?t->group_signal_code[sig-1]:t->signal_code[sig-1];
        if(sig==17) {
            struct kernel_signal_child_info child=shared?t->group_child_info:t->signal_child_info;
            out->uid=child.uid;out->status=child.status;
            uint64_t *times=shared?t->group_child_times:t->signal_child_times;
            out->user_time=times[0];out->system_time=times[1];
        }
        if(s->inline_charged&bit) {
            uncharge(s->inline_account,1);s->inline_charged&=~bit;
            retire_inline_account(s,p);
        }
    }
    if(!s->queue||!s->queue->slots[sig-1].head)*bits(t,shared)&=~bit;
}

static int clear_locked(struct kernel_task *t,int shared,uint64_t mask,
    struct signal_pending_prepared *p,int ignored)
{
    struct kernel_signal_pending_state *s=state(t,shared);
    uint64_t charged=s->inline_charged&mask;
    uint64_t count=0;
    while(charged){charged&=charged-1;count++;}
    if(count)uncharge(s->inline_account,count);
    s->inline_charged&=~mask;retire_inline_account(s,p);
    if(s->queue)s->queue->inline_pending&=~mask;
    unsigned budget=16;
    if(s->queue)for(unsigned i=0;i<64;i++)if(mask&(UINT64_C(1)<<i)) {
        while(s->queue->slots[i].head) {
            if(!budget)return 1;
            budget--;
            struct signal_queue_record *r=s->queue->slots[i].head;
            s->queue->slots[i].head=r->next;s->queue->slots[i].count--;
            if(!s->queue->slots[i].head)s->queue->slots[i].tail=0;
            if(r->timer){s->queue->native_count--;posix_timer_record_clear_locked(r,ignored,p);}
            else {uncharge(r->account,1);r->next=p->retired;p->retired=r;}
        }
        *bits(t,shared)&=~(UINT64_C(1)<<i);
    }
    *bits(t,shared)&=~mask;return 0;
}
int signal_pending_clear_locked(struct kernel_task *t,int shared,uint64_t mask,struct signal_pending_prepared *p)
{return clear_locked(t,shared,mask,p,0);}
int signal_pending_ignore_locked(struct kernel_task *t,int shared,uint64_t mask,struct signal_pending_prepared *p)
{return clear_locked(t,shared,mask,p,1);}

void signal_pending_finish(struct signal_pending_prepared *p)
{
    while(p->completed){struct signal_queue_record *r=p->completed;p->completed=r->next;posix_timer_record_complete(r);}
    if(p->record){p->record->next=p->retired;p->retired=p->record;}
    while(p->retired) {
        struct signal_queue_record *r=p->retired;p->retired=r->next;
        if(r->timer)posix_timer_record_free(r);else {if(r->account)kernel_uid_account_put(r->account);kernel_heap_release(r->heap,r);}
    }
    if(p->index)kernel_heap_release(p->index->heap,p->index);
    for(unsigned i=0;i<2;i++)if(p->retired_accounts[i])kernel_uid_account_put(p->retired_accounts[i]);
    *p=(struct signal_pending_prepared){0};
}
void signal_pending_flush_timers(struct kernel_task *task)
{
    struct signal_pending_prepared p={0};
    {
        KERNEL_RAW_SCOPE(g,&kernel_wait_domain);
        for(int shared=0;shared<2;shared++) {
            struct kernel_task *owner=shared?task->group_leader:task;
            struct kernel_signal_queue *q=state(owner,shared)->queue;if(!q)continue;
            /* Exec has quiesced the group. Keep surviving ordinary records
             * in order; state/quota publication is atomic against senders. */
            for(unsigned i=0;i<64;i++) {
                struct signal_queue_record **link=&q->slots[i].head,*tail=0;
                while(*link){struct signal_queue_record *r=*link;
                    if(r->info.code!=-2){tail=r;link=&r->next;continue;}
                    *link=r->next;q->slots[i].count--;
                    if(r->timer){q->native_count--;posix_timer_record_clear_locked(r,0,&p);}
                    else {uncharge(r->account,1);r->next=p.retired;p.retired=r;}
                }
                q->slots[i].tail=tail;
                if(!q->slots[i].head&&!(q->inline_pending&(UINT64_C(1)<<i)))*bits(owner,shared)&=~(UINT64_C(1)<<i);
            }
        }
    }
    signal_pending_finish(&p);
}
void kernel_signal_pending_cleanup(struct kernel_task *t)
{
    struct signal_pending_prepared p={0};struct kernel_signal_queue *indexes[2]={0};
    for(;;) {
        int more;
        {KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
         more=signal_pending_clear_locked(t,0,UINT64_MAX,&p);
         if(!more && t->group_leader==t&&t->group_members<=1)more=signal_pending_clear_locked(t,1,UINT64_MAX,&p);
         if(!more){indexes[0]=t->signal_queue.queue;t->signal_queue.queue=0;
             if(t->group_leader==t&&t->group_members<=1){indexes[1]=t->group_queue.queue;t->group_queue.queue=0;}}}
        signal_pending_finish(&p);if(!more)break;
    }
    signal_pending_finish(&p);
    for(unsigned i=0;i<2;i++)if(indexes[i])kernel_heap_release(indexes[i]->heap,indexes[i]);
}
void kernel_signal_pending_adopt_locked(struct kernel_task *to,struct kernel_task *from)
{
    if(to->group_queue.queue || to->group_queue.inline_charged)__builtin_trap();
    to->group_queue=from->group_queue;from->group_queue=(struct kernel_signal_pending_state){0};
    memcpy(to->group_child_times,from->group_child_times,sizeof(to->group_child_times));
    from->group_pending=0;
}
