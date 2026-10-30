/* The backend models a sector device's read/modify/write callback. Pause after
 * the first snapshot; the peer must either finish or actually wait on the
 * transfer lock before allowing that snapshot to be written back. */
enum rmw_operation { RMW_SCALAR, RMW_BATCH, RMW_PRIO, RMW_BYTES };
struct rmw_fixture {
    unsigned char bytes[8192];
    size_t sector;
    struct kernel_block_device *disk;
    unsigned paused, resume, second_done, second_waiting;
    unsigned configure_pause,configure_resume;
};
struct rmw_call {
    struct rmw_fixture *fixture;
    struct kernel_block_device *device;
    enum rmw_operation operation;
    uint64_t offset;
    size_t size;
    unsigned actor;
    enum kernel_block_status status;
};
static _Thread_local struct rmw_call *rmw_current;
static void rmw_before_lock(struct kernel_rwlock *lock)
{
    if (!rmw_current || rmw_current->actor!=1 || lock!=&rmw_current->fixture->disk->transfer_lock) return;
    struct rmw_fixture *fixture=rmw_current->fixture;
    assert(!pthread_mutex_lock(&handshake));
    if (fixture->configure_pause==1) {
        fixture->configure_pause=2; pthread_cond_broadcast(&changed);
        while (!fixture->configure_resume)assert(!pthread_cond_wait(&changed,&handshake));
    }
    assert(!pthread_mutex_unlock(&handshake));
}
void block_model_lock_waiting(struct kernel_rwlock *lock)
{
    if (!rmw_current || rmw_current->actor!=2 || lock!=&rmw_current->fixture->disk->transfer_lock) return;
    assert(!pthread_mutex_lock(&handshake));
    rmw_current->fixture->second_waiting=1; pthread_cond_broadcast(&changed);
    assert(!pthread_mutex_unlock(&handshake));
}
static enum kernel_block_status rmw_backend_read(void *context,uint64_t offset,void *buffer,size_t size)
{
    struct rmw_fixture *fixture=context;
    assert(offset+size<=sizeof(fixture->bytes));
    assert(!pthread_mutex_lock(&handshake));
    memcpy(buffer,fixture->bytes+offset,size);
    assert(!pthread_mutex_unlock(&handshake)); return 0;
}
static enum kernel_block_status rmw_backend_write(void *context,uint64_t offset,const void *buffer,size_t size)
{
    struct rmw_fixture *fixture=context;
    assert(offset+size<=sizeof(fixture->bytes));
    while (size) {
        unsigned char snapshot[1024];
        size_t within=offset%fixture->sector;
        uint64_t first=offset-within;
        size_t amount=fixture->sector-within;
        if (amount>size)amount=size;
        assert(!pthread_mutex_lock(&handshake));
        memcpy(snapshot,fixture->bytes+first,fixture->sector);
        if (rmw_current && rmw_current->actor==1 && !fixture->paused) {
            fixture->paused=1; pthread_cond_broadcast(&changed);
            while (!fixture->resume)assert(!pthread_cond_wait(&changed,&handshake));
        }
        memcpy(snapshot+within,buffer,amount);
        memcpy(fixture->bytes+first,snapshot,fixture->sector);
        assert(!pthread_mutex_unlock(&handshake));
        offset+=amount; buffer=(const unsigned char *)buffer+amount; size-=amount;
    }
    return 0;
}
static enum kernel_block_status rmw_backend_batch(void *context,const struct kernel_block_span *spans,size_t count)
{
    for (size_t i=0;i<count;i++) {
        enum kernel_block_status result=rmw_backend_write(context,spans[i].offset,spans[i].buffer,spans[i].size);
        if (result)return result;
    }
    return 0;
}
static enum kernel_block_status rmw_backend_prio(void *context,const struct kernel_block_span *spans,size_t count,
    enum kernel_block_priority priority)
{
    assert(rmw_current && priority==(rmw_current->operation==RMW_PRIO ?
        KERNEL_BLOCK_PRIO_LOW : KERNEL_BLOCK_PRIO_NORMAL));
    return rmw_backend_batch(context,spans,count);
}
static void *rmw_actor(void *argument)
{
    struct rmw_call *call=argument; rmw_current=call;
    unsigned char bytes[2048],scratch[1024]; memset(bytes,call->actor==1?0x61:0x72,sizeof(bytes));
    struct kernel_block_span spans[3]={{1,0,0},{call->offset,bytes,call->size},
        {4096-call->device->start_bytes,bytes+1024,call->fixture->sector}};
    if (call->operation==RMW_SCALAR)
        call->status=kernel_block_write_at(call->device,call->offset,bytes,call->size);
    else if (call->operation==RMW_BYTES) {
        size_t done=0;
        call->status=kernel_block_write_bytes(call->device,call->offset,bytes,call->size,scratch,sizeof(scratch),&done);
        assert(call->status || done==call->size);
    } else if (call->operation==RMW_PRIO)
        call->status=kernel_block_write_batch_prio(call->device,spans,3,KERNEL_BLOCK_PRIO_LOW);
    else call->status=kernel_block_write_batch(call->device,spans,3);
    assert(!pthread_mutex_lock(&handshake));
    if (call->actor==2)call->fixture->second_done=1;
    pthread_cond_broadcast(&changed); assert(!pthread_mutex_unlock(&handshake));
    rmw_current=0; return 0;
}
struct rmw_plan {
    const char *name;
    size_t sector;
    uint64_t partition_start;
    size_t partition_sector;
    enum rmw_operation first_operation;
    uint64_t first_offset;
    size_t first_size;
    enum rmw_operation second_operation;
    uint64_t second_offset;
    size_t second_size;
    unsigned backend,parallel,configure;
};
static void driver_rmw_race(const struct rmw_plan *plan)
{
    struct rmw_fixture fixture={.sector=plan->sector,.configure_pause=plan->configure};
    memset(fixture.bytes,0x3c,sizeof(fixture.bytes));
    struct kernel_block_device d={.context=&fixture,.read=rmw_backend_read,.write=rmw_backend_write,
        .logical_block_size=plan->configure?512:plan->sector,.capacity_bytes=sizeof(fixture.bytes)};
    struct kernel_block_device p={.parent=&d,.start_bytes=plan->partition_start,
        .logical_block_size=plan->partition_sector,.capacity_bytes=sizeof(fixture.bytes)-plan->partition_start};
    fixture.disk=&d;
    if (plan->backend==1)d.write_batch=rmw_backend_batch;
    if (plan->backend==2)d.write_batch_prio=rmw_backend_prio;
    assert(!kernel_block_register(&d,0xfd00));
    if (plan->partition_start)assert(!kernel_block_register(&p,0xfd01));
    struct rmw_call calls[2]={{&fixture,plan->partition_start?&p:&d,plan->first_operation,
        plan->first_offset,plan->first_size,1,0},
        {&fixture,&d,plan->second_operation,plan->second_offset,plan->second_size,2,0}};
    pthread_t actors[2]; assert(!pthread_create(&actors[0],0,rmw_actor,&calls[0]));
    if (plan->configure) {
        assert(!pthread_mutex_lock(&handshake));
        while (fixture.configure_pause!=2)assert(!pthread_cond_wait(&changed,&handshake));
        assert(!pthread_mutex_unlock(&handshake));
        {
            KERNEL_LOCK_SCOPE(transfer); kernel_rwlock_write(&d.transfer_lock,&transfer);
            d.logical_block_size=plan->sector;
        }
        assert(!pthread_mutex_lock(&handshake));
        fixture.configure_resume=1; pthread_cond_broadcast(&changed);
        assert(!pthread_mutex_unlock(&handshake));
    }
    assert(!pthread_mutex_lock(&handshake));
    while (!fixture.paused)assert(!pthread_cond_wait(&changed,&handshake));
    assert(!pthread_mutex_unlock(&handshake));
    assert(kernel_block_unregister(calls[0].device)==-KERNEL_EBUSY);
    assert(!pthread_create(&actors[1],0,rmw_actor,&calls[1]));
    assert(!pthread_mutex_lock(&handshake));
    while (!fixture.second_done && !fixture.second_waiting)assert(!pthread_cond_wait(&changed,&handshake));
    unsigned parallel=fixture.second_done;
    fixture.resume=1; pthread_cond_broadcast(&changed); assert(!pthread_mutex_unlock(&handshake));
    assert(!pthread_join(actors[0],0) && !pthread_join(actors[1],0));
    unsigned char expected[sizeof(fixture.bytes)]; memset(expected,0x3c,sizeof(expected));
    memset(expected+plan->partition_start+plan->first_offset,0x61,plan->first_size);
    if (plan->first_operation==RMW_BATCH || plan->first_operation==RMW_PRIO)memset(expected+4096,0x61,plan->sector);
    memset(expected+plan->second_offset,0x72,plan->second_size);
    if (plan->second_operation==RMW_BATCH || plan->second_operation==RMW_PRIO)memset(expected+4096,0x72,plan->sector);
    if (memcmp(expected,fixture.bytes,sizeof(expected)) || (plan->parallel && !parallel))
        fprintf(stderr,"driver RMW case failed: %s; peer completed before first writeback=%u\n",plan->name,parallel);
    assert(!calls[0].status && !calls[1].status && !memcmp(expected,fixture.bytes,sizeof(expected)));
    assert(!plan->parallel || parallel);
    assert(!p.references && d.references==!!plan->partition_start);
    if (plan->partition_start)assert(!kernel_block_unregister(&p));
    assert(!kernel_block_unregister(&d));
}
static void driver_rmw_cases(void)
{
    const struct rmw_plan plans[]={
        {"scalar partial writes",512,0,512,RMW_SCALAR,1,1,RMW_SCALAR,2,1,0,0,0},
        {"batch partial and scalar",512,0,512,RMW_BATCH,1,1,RMW_SCALAR,2,1,1,0,0},
        {"priority batch partial and whole sector",512,0,512,RMW_PRIO,1,1,RMW_BATCH,0,512,2,0,0},
        {"fallback batch and byte I/O",512,0,512,RMW_BATCH,1,1,RMW_BYTES,2,1,0,0,0},
        {"physical sector and partition start",1024,512,256,RMW_SCALAR,0,512,RMW_SCALAR,0,512,0,0,0},
        {"translated priority batch",1024,512,256,RMW_PRIO,0,512,RMW_SCALAR,0,512,2,0,0},
        {"parallel aligned scalar",512,0,512,RMW_SCALAR,0,512,RMW_SCALAR,512,512,0,1,0},
        {"parallel aligned batch with empty span",512,0,512,RMW_BATCH,0,512,RMW_SCALAR,512,512,1,1,0},
        {"parallel aligned priority batch",512,0,512,RMW_PRIO,0,512,RMW_SCALAR,512,512,2,1,0},
        {"partition offset restores physical alignment",1024,512,256,RMW_PRIO,512,1024,RMW_SCALAR,2048,1024,2,1,0},
        {"configuration before transfer acquisition",1024,0,1024,RMW_SCALAR,512,512,RMW_SCALAR,0,1024,0,0,1},
        {"configuration before batch acquisition",1024,0,1024,RMW_PRIO,512,512,RMW_SCALAR,0,1024,2,0,1},
    };
    for (size_t i=0;i<sizeof(plans)/sizeof(plans[0]);i++)driver_rmw_race(&plans[i]);
    puts("block driver RMW: scalar/batch/priority/fallback, physical alignment, byte I/O and aligned concurrency passed");
}
