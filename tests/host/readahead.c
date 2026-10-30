#define main original_file_io_main
#include "file_io_concurrency.c"
#undef main

static unsigned char contents[96*BOAROS_PAGE_SIZE+37];

static struct kernel_page_cache_statistics readahead_stats(struct kernel_page_cache *cache)
{
    struct kernel_page_cache_statistics stats;
    kernel_page_cache_get_statistics(cache,&stats);
    return stats;
}

static void read_bytes(struct kernel_open_file_description *file,uint64_t offset,size_t size)
{
    unsigned char bytes[4096];size_t copied=0;
    assert(size<=sizeof(bytes) && offset+size<=sizeof(contents));
    assert(!kernel_open_file_pread(file,offset,bytes,size,&copied) && copied==size);
    assert(!memcmp(bytes,contents+offset,size));
}

static void cold_stream(struct kernel_open_file_description *file,struct kernel_page_cache *cache)
{
    assert(!kernel_open_file_seek(file,0));
    kernel_page_cache_stop_worker(cache);
    assert(!kernel_page_cache_invalidate_node(cache,kernel_open_file_node(file)));
    assert(!kernel_page_cache_start_worker(cache));
}

static void wait_prediction(struct kernel_page_cache *cache,uint64_t completed)
{
    uint64_t deadline=kernel_time_monotonic_ns()+UINT64_C(2000000000);
    while(readahead_stats(cache).readahead_completed<completed && kernel_time_monotonic_ns()<deadline)
        sched_yield();
    assert(readahead_stats(cache).readahead_completed==completed);
}

static void cached_page(struct kernel_open_file_description *file,uint64_t index)
{
    uint64_t physical;size_t valid;
    assert(!kernel_open_file_lookup_page(file,index,&physical,&valid) && valid==BOAROS_PAGE_SIZE);
    assert(!memcmp(access_page(physical),contents+index*BOAROS_PAGE_SIZE,valid));
    assert(!physical_page_release(file->heap->page_allocator,physical));
}

static void unaligned_stream(struct kernel_open_file_description *file,struct kernel_page_cache *cache,size_t chunk)
{
    cold_stream(file,cache);
    struct kernel_page_cache_statistics before=readahead_stats(cache),after;
    /* musl's buffered stream can retain this residue on every later read. */
    uint64_t offset=chunk-9;
    read_bytes(file,0,offset);
    while(offset+chunk<BOAROS_PAGE_SIZE) {
        read_bytes(file,offset,chunk);offset+=chunk;
    }
    assert(readahead_stats(cache).readahead_queued==before.readahead_queued);
    read_bytes(file,offset,chunk);offset+=chunk;
    after=readahead_stats(cache);
    assert(after.readahead_queued==before.readahead_queued+1);
    wait_prediction(cache,before.readahead_completed+1);
    const unsigned initial=BOAROS_PAGE_CACHE_READAHEAD_PAGES<16384U/BOAROS_PAGE_SIZE?
        BOAROS_PAGE_CACHE_READAHEAD_PAGES:16384U/BOAROS_PAGE_SIZE;
    cached_page(file,2);cached_page(file,1+initial);
    uint64_t predicted_end=2+initial;unsigned window=initial;
    for(unsigned round=0;round<2;round++) {
        before=readahead_stats(cache);
        while(offset+chunk<=(predicted_end-1)*BOAROS_PAGE_SIZE) {
            read_bytes(file,offset,chunk);offset+=chunk;
            assert(readahead_stats(cache).readahead_queued==before.readahead_queued);
        }
        read_bytes(file,offset,chunk);offset+=chunk;
        assert(readahead_stats(cache).readahead_queued==before.readahead_queued+1);
        wait_prediction(cache,before.readahead_completed+1);
        window*=2;
        if(window>BOAROS_PAGE_CACHE_READAHEAD_PAGES)window=BOAROS_PAGE_CACHE_READAHEAD_PAGES;
        cached_page(file,predicted_end);
        predicted_end+=window;
        cached_page(file,predicted_end-1);
    }
    assert(offset%chunk==chunk-9);
    printf("PASS unaligned %zu-byte stream: first prediction and two window advances, no duplicate jobs\n",chunk);
}

static void prediction_boundaries(struct kernel_open_file_description *file,struct kernel_page_cache *cache)
{
    cold_stream(file,cache);
    struct kernel_page_cache_statistics before=readahead_stats(cache);
    read_bytes(file,0,1);read_bytes(file,1,1);
    assert(readahead_stats(cache).readahead_queued==before.readahead_queued);
    puts("PASS two one-byte header reads do not prefetch a page");

    for(uint64_t offset=2;offset<BOAROS_PAGE_SIZE;) {
        size_t count=BOAROS_PAGE_SIZE-offset;
        if(count>4096)count=4096;
        read_bytes(file,offset,count);offset+=count;
    }
    assert(file->readahead_cookie);
    kernel_open_file_read_begin(file,0);
    assert(!file->readahead_cookie && !file->read_sequential);
    read_bytes(file,0,1);
    assert(!kernel_open_file_seek(file,0) && !file->read_sequential && !file->read_end);
    puts("PASS nonsequential begin and seek cancel the current prediction");

    cold_stream(file,cache);
    before=readahead_stats(cache);
    uint64_t start=sizeof(contents)-BOAROS_PAGE_SIZE-50;
    for(size_t left=BOAROS_PAGE_SIZE;left;) {
        size_t count=left<4096?left:4096;
        read_bytes(file,start,count);start+=count;left-=count;
    }
    unsigned char tail[64];size_t copied=0;
    assert(!kernel_open_file_pread(file,start,tail,sizeof(tail),&copied) && copied==50);
    assert(!memcmp(tail,contents+start,copied));
    assert(!kernel_open_file_pread(file,sizeof(contents),tail,sizeof(tail),&copied) && !copied);
    assert(readahead_stats(cache).readahead_queued==before.readahead_queued);
    puts("PASS completed EOF prefix does not queue beyond the last file page");
}

static void demand_reset_and_error_prefix(struct kernel_open_file_description *file,struct kernel_page_cache *cache)
{
    static unsigned char bytes[65536];
    cold_stream(file,cache);
    read_bytes(file,0,1015);
    for(uint64_t offset=1015;offset<BOAROS_PAGE_SIZE;) {
        size_t count=BOAROS_PAGE_SIZE-offset;
        if(count>4096)count=4096;
        read_bytes(file,offset,count);offset+=count;
    }
    struct kernel_page_cache_statistics before=readahead_stats(cache);
    wait_prediction(cache,before.readahead_queued);
    size_t copied=0;
    assert(!kernel_open_file_pread(file,BOAROS_PAGE_SIZE,bytes,sizeof(bytes),&copied) && copied==sizeof(bytes));
    assert(!memcmp(bytes,contents+BOAROS_PAGE_SIZE,copied));
    assert(!file->read_sequential && !file->read_end && !file->readahead_cookie);
    assert(readahead_stats(cache).readahead_queued==before.readahead_queued);
    puts("PASS 64 KiB demand batch resets prediction and queues no competing stream");

    cold_stream(file,cache);
    kernel_page_cache_stop_worker(cache);
    read_bytes(file,0,1015);
    before=readahead_stats(cache);fail_memlock_read=1;
    assert(kernel_open_file_pread(file,1015,bytes,BOAROS_PAGE_SIZE,&copied)==-KERNEL_EIO);
    assert(copied==BOAROS_PAGE_SIZE-1015 && !memcmp(bytes,contents+1015,copied));
    assert(file->read_end==BOAROS_PAGE_SIZE && file->read_sequential && !file->readahead_cookie);
    assert(kernel_open_file_pread(file,BOAROS_PAGE_SIZE,bytes,4096,&copied)==-KERNEL_EIO && !copied);
    assert(file->read_end==BOAROS_PAGE_SIZE);
    fail_memlock_read=0;
    read_bytes(file,BOAROS_PAGE_SIZE,4096);
    assert(readahead_stats(cache).readahead_queued==before.readahead_queued);
    puts("PASS read error preserves only the completed prefix; stopped worker accepts no prediction");
}

int main(int argc,char **argv)
{
    assert(argc==2);setvbuf(stdout,NULL,_IONBF,0);mm_test_attach(0);mm_test_activate_task();
    struct physical_page_allocator pages={0},backend_pages={0};struct kernel_heap heap={0},backend_heap={0};
    initialize(&pages,&heap,arena,sizeof(arena));initialize(&backend_pages,&backend_heap,backend,sizeof(backend));
    uint64_t available=physical_page_available(&pages),backend_available=physical_page_available(&backend_pages);
    image=open(argv[1],O_RDWR);assert(image>=0);struct stat info;assert(!fstat(image,&info));
    struct kernel_page_cache cache={0};assert(!kernel_page_cache_init(&cache,&heap,&pages));
    struct kernel_block_device disk={.read=read_image,.write=write_image,.flush=flush_image,.capacity_bytes=info.st_size,.logical_block_size=512,.cache_mode=KERNEL_BLOCK_CACHE_WRITEBACK};
    assert(!kernel_block_register(&disk,KERNEL_BLOCK_DEVICE_NUMBER(107)));
    struct kernel_vfs_mount mount={0};assert(!kernel_vfs_mount_ext4(&mount,&disk,&backend_heap,&cache,0));
    assert(!kernel_vfs_start_journal_worker(&mount));
    struct kernel_open_file_description *file=NULL;int error=0;
    assert(!kernel_open_file_create_mode(&heap,&mount,"/unaligned-stream",0600,&file,&error) && !error);
    for(size_t i=0;i<sizeof(contents);i++)contents[i]=(unsigned char)(i/BOAROS_PAGE_SIZE+1);
    size_t copied=0;assert(!kernel_vfs_pwrite(&file->file,0,contents,sizeof(contents),&copied) && copied==sizeof(contents));
    uint64_t observed=0;assert(!kernel_vfs_sync(&file->file,0,&observed));
    unaligned_stream(file,&cache,1024);unaligned_stream(file,&cache,4096);
    prediction_boundaries(file,&cache);
    demand_reset_and_error_prefix(file,&cache);
    kernel_page_cache_stop_worker(&cache);
    assert(!kernel_open_file_release(&file) && !kernel_vfs_unmount(&mount));
    assert(!kernel_page_cache_destroy(&cache) && !kernel_block_unregister(&disk));
    mm_test_finish();
    assert(physical_page_available(&pages)==available && physical_page_available(&backend_pages)==backend_available);
    assert(!close(image));puts("PASS readahead owners returned");return 0;
}
