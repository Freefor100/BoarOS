/* Real ring, user copies and creator UID accounting. */
#include <kernel/credentials.h>
static int64_t pipe_size(struct kernel_pipe *pipe,int command,uint64_t argument)
{ return kernel_pipe_capacity(pipe,command,argument); }
static void pipe_capacity_case(struct kernel_heap *heap)
{
    pipe_bind(0,0);struct kernel_pipe *pipe=pipe_table(heap);mm_test_bind_files(&files);
    assert(pipe_size(pipe,1032,0)==KERNEL_PIPE_DEFAULT_CAPACITY);
    assert(pipe_size(pipe,1031,0)==BOAROS_PAGE_SIZE);
    assert(pipe_size(pipe,1031,BOAROS_PAGE_SIZE+1)==2*BOAROS_PAGE_SIZE);
    assert(pipe_size(pipe,1031,UINT64_C(0x100000000)+1)==BOAROS_PAGE_SIZE);
    assert(pipe_size(pipe,1031,UINT32_MAX)==-KERNEL_EINVAL);
    assert(pipe_size(pipe,1031,32*BOAROS_PAGE_SIZE)==32*BOAROS_PAGE_SIZE);
    uint64_t physical_before=physical_page_available(heap->page_allocator);
    struct kernel_pipe_result written;struct kernel_uaccess_iovec target={PIPE_TARGET,BOAROS_PAGE_SIZE};int64_t got;
    for(unsigned i=0;i<24;i++) {
        memset(pipe_payload,(int)(0x20+i),BOAROS_PAGE_SIZE);
        assert(!kernel_pipe_write_buffer(pipe,pipe_payload,BOAROS_PAGE_SIZE,1|KERNEL_PIPE_NONBLOCK,&written) && written.bytes==BOAROS_PAGE_SIZE);
    }
    assert(physical_before-physical_page_available(heap->page_allocator)==24);
    for(unsigned i=0;i<20;i++)assert(!kernel_pipe_readv(pipe,&domain_mm,&target,1,BOAROS_PAGE_SIZE,0,&got) && got==BOAROS_PAGE_SIZE);
    for(unsigned i=24;i<40;i++) {
        memset(pipe_payload,(int)(0x20+i),BOAROS_PAGE_SIZE);
        assert(!kernel_pipe_write_buffer(pipe,pipe_payload,BOAROS_PAGE_SIZE,1|KERNEL_PIPE_NONBLOCK,&written) && written.bytes==BOAROS_PAGE_SIZE);
    }
    assert(pipe_size(pipe,1031,16*BOAROS_PAGE_SIZE)==-KERNEL_EBUSY);
    assert(pipe_size(pipe,1032,0)==32*BOAROS_PAGE_SIZE);
    assert(pipe_size(pipe,1031,64*BOAROS_PAGE_SIZE)==64*BOAROS_PAGE_SIZE);
    for(unsigned i=20;i<40;i++) {
        assert(!kernel_pipe_readv(pipe,&domain_mm,&target,1,BOAROS_PAGE_SIZE,0,&got) && got==BOAROS_PAGE_SIZE);
        size_t done;assert(!kernel_copy_from_user(&domain_mm,pipe_payload,PIPE_TARGET,BOAROS_PAGE_SIZE,&done) && done==BOAROS_PAGE_SIZE);
        for(unsigned j=0;j<BOAROS_PAGE_SIZE;j++)assert(pipe_payload[j]==0x20+i);
    }
    assert(pipe_size(pipe,1031,BOAROS_PAGE_SIZE)==BOAROS_PAGE_SIZE);
    uint64_t charge=kernel_uid_account_pipe_pages(kernel_credentials_current()->account,0,0);
    fail_next_allocation=1;
    assert(pipe_size(pipe,1031,64*BOAROS_PAGE_SIZE)==-KERNEL_ENOMEM && !fail_next_allocation);
    assert(pipe_size(pipe,1032,0)==BOAROS_PAGE_SIZE);
    assert(kernel_uid_account_pipe_pages(kernel_credentials_current()->account,0,0)==charge);
    assert(!kernel_files_release(&files));
    memset(pipe_payload,0x71,sizeof(pipe_payload));
    puts("PASS pipe resize: unsigned-int argument, page rounding, wrapped growth, occupied-slot shrink and exact content");
}

static void pipe_capacity_merge_case(struct kernel_heap *heap)
{
    pipe_bind(0,0);struct kernel_pipe *pipe=pipe_table(heap);mm_test_bind_files(&files);
    assert(pipe_size(pipe,1031,32*BOAROS_PAGE_SIZE)==32*BOAROS_PAGE_SIZE);
    struct kernel_pipe_result write;
    for(unsigned i=0;i<31;i++)assert(!kernel_pipe_write_buffer(pipe,"S",1,1,&write) && write.bytes==1);
    struct kernel_uaccess_iovec source={PIPE_SOURCE,1};
    assert(!kernel_pipe_writev(pipe,&domain_mm,&source,1,1,1,&write) && write.bytes==1);
    assert(!(kernel_pipe_poll(pipe,2,0,0)&KERNEL_POLLOUT));
    assert(pipe_size(pipe,1031,16*BOAROS_PAGE_SIZE)==-KERNEL_EBUSY);
    assert(pipe_size(pipe,1031,64*BOAROS_PAGE_SIZE)==64*BOAROS_PAGE_SIZE);
    assert(pipe_size(pipe,1031,32*BOAROS_PAGE_SIZE)==32*BOAROS_PAGE_SIZE);
    source.length=7;
    assert(!kernel_pipe_writev(pipe,&domain_mm,&source,1,7,1|KERNEL_PIPE_NONBLOCK,&write) && write.bytes==7);
    assert(!kernel_pipe_write_buffer(pipe,"S",1,1|KERNEL_PIPE_NONBLOCK,&write) && write.bytes==-KERNEL_EAGAIN);
    struct kernel_uaccess_iovec target={PIPE_TARGET,39};int64_t got;size_t done;
    assert(!kernel_pipe_readv(pipe,&domain_mm,&target,1,39,0,&got) && got==39);
    assert(!kernel_copy_from_user(&domain_mm,pipe_payload,PIPE_TARGET,39,&done) && done==39);
    for(unsigned i=0;i<39;i++)assert(pipe_payload[i]==(i<31?'S':0x71));
    assert(!kernel_files_release(&files));memset(pipe_payload,0x71,sizeof(pipe_payload));
    puts("PASS pipe resized tail: slot31 merge attribute survives growth/shrink; full poll and buffer-write stay slot based");
}

struct capacity_actor { int64_t result; unsigned done; };
static void *pipe_resize_actor(void *argument)
{
    struct capacity_actor *actor=argument;pipe_bind(2,&views[2]);
    KERNEL_FILES_PIN_SCOPE(pin);
    struct kernel_open_file_description *file=kernel_files_hold_fd(&views[2],0,&pin);assert(file);
    actor->result=pipe_size(file->pipe,1031,32*BOAROS_PAGE_SIZE);
    kernel_files_pin_guard_release(&pin);assert(!kernel_files_release(&views[2]));
    mm_test_bind_files(0);mm_test_bind(0);
    __atomic_store_n(&actor->done,1,__ATOMIC_RELEASE);return 0;
}
static void pipe_capacity_copy_case(struct kernel_heap *heap)
{
    for(unsigned mode=0;mode<3;mode++) {
        pipe_bind(0,0);struct kernel_pipe *pipe=pipe_table(heap);mm_test_bind_files(&files);
        struct kernel_uaccess_iovec source={PIPE_SOURCE,1},target={PIPE_TARGET,2};
        struct kernel_pipe_result written;
        if(mode==1)assert(!kernel_pipe_writev(pipe,&domain_mm,&source,1,1,1,&written) && written.bytes==1);
        if(mode==2)assert(!kernel_pipe_write_buffer(pipe,"RR",2,1,&written) && written.bytes==2);
        struct pipe_actor actor={.write=mode!=2,.fd=mode!=2?1:0,.address=mode!=2?PIPE_SOURCE:PIPE_TARGET,.count=1};
        pause_copy=mode!=2;pause_read_copy=mode==2;pause_park=0;
        pthread_t copying,resizer;start_actor(&actor,&copying);
        while(!__atomic_load_n(&copy_ready,__ATOMIC_ACQUIRE))sched_yield();
        views[2]=(struct kernel_files){0};assert(!kernel_files_acquire(&views[2],&files));
        struct capacity_actor resize={0};unsigned parked=mm_test_parked(2);
        assert(!pthread_create(&resizer,0,pipe_resize_actor,&resize));
        while(!__atomic_load_n(&resize.done,__ATOMIC_ACQUIRE) && mm_test_parked(2)==parked)sched_yield();
        assert(!__atomic_load_n(&resize.done,__ATOMIC_ACQUIRE) && mm_test_parked(2)>parked);
        /* Closing the numeric writer cannot revoke the copy's pinned OFD. */
        int64_t got;assert(!kernel_files_close(&files,1,&got) && !got);
        assert(pipe_size(pipe,1032,0)==KERNEL_PIPE_DEFAULT_CAPACITY);
        __atomic_store_n(&resume_copy,1,__ATOMIC_RELEASE);
        assert(!pthread_join(copying,0) && !pthread_join(resizer,0));
        assert(actor.bytes==1 && resize.result==32*BOAROS_PAGE_SIZE);
        unsigned left=mode==1?2:1;
        assert(!kernel_pipe_readv(pipe,&domain_mm,&target,1,left,0,&got) && got==left);
        assert(!kernel_pipe_readv(pipe,&domain_mm,&target,1,1,0,&got) && !got);
        assert(!kernel_files_release(&files));
    }
    puts("PASS pipe resize/usercopy handshake: new-slot write, tail merge and read finish before move; close preserves pinned copy");
}

static void pipe_capacity_wake_case(struct kernel_heap *heap)
{
    pipe_bind(0,0);struct kernel_pipe *pipe=pipe_table(heap);mm_test_bind_files(&files);
    assert(pipe_size(pipe,1031,BOAROS_PAGE_SIZE)==BOAROS_PAGE_SIZE);
    struct kernel_pipe_result filled;
    assert(!kernel_pipe_write_buffer(pipe,pipe_payload,BOAROS_PAGE_SIZE,1,&filled) && filled.bytes==BOAROS_PAGE_SIZE);
    struct pipe_actor actor={.write=1,.fd=1,.address=PIPE_SOURCE,.count=1};
    pause_copy=0;pause_park=1;expected_queue=&pipe->write_queue;
    pthread_t writer;start_actor(&actor,&writer);
    while(!__atomic_load_n(&park_ready,__ATOMIC_ACQUIRE))sched_yield();
    assert(pipe_size(pipe,1031,2*BOAROS_PAGE_SIZE)==2*BOAROS_PAGE_SIZE);
    __atomic_store_n(&resume_park,1,__ATOMIC_RELEASE);
    assert(!pthread_join(writer,0) && actor.bytes==1);
    struct kernel_uaccess_iovec target={PIPE_TARGET,BOAROS_PAGE_SIZE+1};int64_t got;
    assert(!kernel_pipe_readv(pipe,&domain_mm,&target,1,BOAROS_PAGE_SIZE+1,0,&got) && got==BOAROS_PAGE_SIZE+1);
    assert(!kernel_files_release(&files));
    puts("PASS pipe growth wakes a registered full-pipe writer before park and preserves its admitted byte");
}

static void pipe_capacity_quota_case(struct kernel_heap *heap)
{
    pipe_bind(0,0);
    const struct kernel_credentials *root=kernel_credentials_current();
    struct kernel_credentials *user=kernel_credentials_clone(heap,root);assert(user);
    assert(!kernel_credentials_set_account(user,1007));
    user->uid=1007;user->euid=user->fsuid=2007;user->effective=0;
    const struct kernel_credentials *previous=kernel_credentials_override(user);
    struct kernel_pipe *probe=0;assert(!kernel_pipe_create(heap,&probe));
    struct kernel_heap_statistics before,after;
    kernel_heap_get_statistics(heap,&before);
    assert(pipe_size(probe,1031,UINT32_C(0x80000000))==-KERNEL_EPERM);
    kernel_heap_get_statistics(heap,&after);
    printf("PIPE-MAX-REJECT heap-allocation-calls=%llu\n",
        (unsigned long long)(after.allocation_calls-before.allocation_calls));
    assert(after.allocation_calls==before.allocation_calls);
    assert(!kernel_pipe_destroy_unowned(probe));
    struct kernel_pipe *pipes[257]={0};
    const unsigned per=1048576/BOAROS_PAGE_SIZE,count=16384/per;
    /* Fill the real UID's soft page allowance without allocating data pages. */
    for(unsigned i=0;i<count;i++) {
        assert(!kernel_pipe_create(heap,&pipes[i]));
        assert(pipe_size(pipes[i],1031,1048576)==1048576);
    }
    assert(kernel_uid_account_pipe_pages(user->account,0,0)==16384);
    assert(!kernel_pipe_create(heap,&pipes[count]));
    assert(pipe_size(pipes[count],1032,0)==2*BOAROS_PAGE_SIZE);
    assert(pipe_size(pipes[count],1031,4*BOAROS_PAGE_SIZE)==-KERNEL_EPERM);
    assert(kernel_uid_account_pipe_pages(user->account,0,0)==16386);
    user->effective=UINT64_C(1)<<KERNEL_CAP_SYS_ADMIN;
    assert(pipe_size(pipes[count],1031,4*BOAROS_PAGE_SIZE)==4*BOAROS_PAGE_SIZE);
    assert(pipe_size(pipes[count],1031,2097152)==-KERNEL_EPERM);
    user->effective=UINT64_C(1)<<KERNEL_CAP_SYS_RESOURCE;
    assert(pipe_size(pipes[count],1031,2097152)==2097152);
    user->effective=0;
    assert(pipe_size(pipes[count],1031,4*BOAROS_PAGE_SIZE)==4*BOAROS_PAGE_SIZE);
    assert(pipe_size(pipes[0],1031,2097152)==-KERNEL_EPERM);
    assert(pipe_size(pipes[0],1031,BOAROS_PAGE_SIZE)==BOAROS_PAGE_SIZE);
    /* This pipe belongs to its creator even after current real UID changes. */
    kernel_credentials_override(previous);
    assert(pipe_size(pipes[count],1031,8*BOAROS_PAGE_SIZE)==8*BOAROS_PAGE_SIZE);
    for(unsigned i=0;i<=count;i++)assert(!kernel_pipe_destroy_unowned(pipes[i]));
    assert(!kernel_uid_account_pipe_pages(user->account,0,0));
    kernel_credentials_put(user);
    /* The original credentials may disappear while the pipe still exists. */
    user=kernel_credentials_clone(heap,root);assert(user);
    assert(!kernel_credentials_set_account(user,1008));user->uid=1008;user->effective=0;
    previous=kernel_credentials_override(user);
    struct kernel_pipe *survivor=0;assert(!kernel_pipe_create(heap,&survivor));
    struct kernel_uid_account *creator=user->account;
    kernel_credentials_override(previous);kernel_credentials_put(user);
    assert(pipe_size(survivor,1031,32*BOAROS_PAGE_SIZE)==32*BOAROS_PAGE_SIZE);
    assert(kernel_uid_account_pipe_pages(creator,0,0)==32);
    assert(!kernel_pipe_destroy_unowned(survivor));
    creator=kernel_uid_account_get(heap,1008);assert(creator);
    assert(!kernel_uid_account_pipe_pages(creator,0,0));kernel_uid_account_put(creator);
    puts("PASS pipe creator real-UID quota: soft boundary, two-page default, growth refusal, permitted shrink and final return");
}
