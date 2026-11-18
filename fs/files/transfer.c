#include "../vfs_notify.h"
#include "private.h"
#include "../open_file_internal.h"
#include "../pipe_internal.h"
#include "../char_device_internal.h"
#include "../block_file_internal.h"
#include "../uaccess_iov_internal.h"
#include "../../mm/uaccess_internal.h"
#include "../vfs_internal.h"
#include "../write_source.h"

#include <kernel/cost.h>
#include <kernel/credentials.h>
#include <kernel/cpu.h>
#include <kernel/errno.h>
#include <kernel/event_fd.h>
#include <kernel/mqueue.h>
#include <kernel/signal_fd.h>
#include <kernel/heap.h>
#include <kernel/mm.h>
#include <kernel/open_file.h>
#include <kernel/page.h>
#include <kernel/page_cache.h>
#include <kernel/scheduler.h>
#include <kernel/signal.h>
#include <kernel/socket.h>
#include <kernel/socket_control.h>
#include <kernel/task.h>
#include <kernel/time.h>
#include <kernel/uaccess.h>
#include <kernel/vfs.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <arch/context.h>
#include <arch/platform_io.h>

static int64_t sendfile_output(struct kernel_open_file_description **output_owner,
                               const void *buffer, size_t size,
                               uint64_t deadline, uint32_t socket_flags)
{
    struct kernel_open_file_description *output = *output_owner;
    enum kernel_open_file_kind kind = kernel_open_file_kind(output);
    if (kind == KERNEL_OPEN_FILE_KIND_SOCKET) {
        struct kernel_socket *socket = kernel_open_file_socket(output);
        struct kernel_socket_write_request request = {0};
        int tcp = kernel_socket_is_tcp(socket), result;
        if (tcp) kernel_socket_stream_begin(&request, output_owner);
        for (;;) {
            if (tcp) {
                result = kernel_socket_stream_reserve(&request, (uint32_t)size, socket_flags);
                if (result > 0) result = kernel_socket_stream_commit(&request, buffer, (uint32_t)result, socket_flags);
            } else result = kernel_socket_write_packet_buffer(output_owner, buffer, size, socket_flags);
            if (result != -KERNEL_EAGAIN || (socket_flags & KERNEL_SOCKET_MSG_DONTWAIT)) break;
            uint64_t now = deadline ? kernel_time_monotonic_ns() : 0;
            int waited = deadline && now >= deadline ? -KERNEL_EAGAIN :
                kernel_socket_wait_ready(socket, KERNEL_POLLOUT, deadline, socket_flags);
            if (waited != 0) { result = waited; break; }
        }
        if (tcp) kernel_socket_stream_finish(&request);
        else if (result == -KERNEL_EPIPE && kernel_socket_is_stream(socket) &&
                 !(socket_flags & KERNEL_SOCKET_MSG_NOSIGNAL))
            (void)kernel_signal_send_task(kernel_task_current(), 13U, 0);
        return result;
    }

    size_t written = 0;
    int error = 0;
    if (output->device && output->device->write) {
        error = output->device->write(output->device_instance, kernel_task_current(), kernel_open_file_flags(output), buffer, size, &written);
    } else return -KERNEL_EINVAL;
    if (written > size) __builtin_trap();
    return written != 0U ? (int64_t)written : error;
}

static int64_t regular_copy_output(struct kernel_open_file_description *out,
    const void *buffer,const struct kernel_page_cache_read_page *pages,size_t source_offset,
    size_t size,uint64_t position,struct kernel_vfs_size_limit *limit,int first)
{
    KERNEL_LOCK_SCOPE(operation);
    kernel_vfs_file_write_lock(&out->file,&operation);
    size_t allowed=size,written=0;
    int error=kernel_vfs_check_write_limit(limit,position,&allowed);
    if (error) return error;
    if (first) {
        error=kernel_vfs_file_modified(&out->file,position,0);
        if (error) return error;
    }
    error=pages?kernel_vfs_pwrite_cached(&out->file,position,pages,source_offset,allowed,&written):
        kernel_vfs_pwrite(&out->file,position,buffer,allowed,&written);
    if (written>allowed) __builtin_trap();
    /* sendfile still attempts the suffix at the size limit after accepting
     * the permitted prefix; copy_file_range already capped its input. */
    if (!error && written==allowed && allowed<size) {
        size_t suffix=size-allowed;
        (void)kernel_vfs_check_write_limit(limit,position+written,&suffix);
    }
    return written?(int64_t)written:error;
}

/* A source entry borrow pins bytes across the destination write. Neither its
 * index nor its inode is locked while allocating or waiting on the target. */
static __attribute__((noinline)) int64_t regular_cached_copy(
    struct kernel_open_file_description *input,struct kernel_open_file_description *output,
    struct kernel_page_cache *cache,uint64_t start,uint64_t out_start,uint64_t count,
    struct kernel_vfs_size_limit *limit)
{
    enum { COPY_WINDOW_PAGES=8 };
    struct kernel_page_cache_read_page pages[COPY_WINDOW_PAGES];
    uint64_t total=0;
    int error=0;
    size_t capacity=COPY_WINDOW_PAGES;
    while (total<count) {
        uint64_t position=start+total;
        size_t offset=position&BOAROS_PAGE_MASK;
        size_t wanted=capacity*BOAROS_PAGE_SIZE-offset;
        if (wanted>count-total) wanted=count-total;
        size_t nr=(offset+wanted+BOAROS_PAGE_MASK)>>BOAROS_PAGE_SHIFT;
        kernel_open_file_read_begin(input,position);
        if (kernel_page_cache_get_batch(cache,&input->file,position>>BOAROS_PAGE_SHIFT,pages,nr, kernel_open_file_read_no_reuse(input))
            ==KERNEL_PAGE_CACHE_STATUS_INVALID_ARGUMENT) __builtin_trap();
        size_t available=0,within=offset;
        int miss=0;
        error=0;
        for (size_t i=0;i<nr;i++) {
            const struct kernel_page_cache_read_page *page=&pages[i];
            if (page->status==KERNEL_PAGE_CACHE_STATUS_OUT_OF_RANGE) break;
            if (page->status) {
                error=page->status==KERNEL_PAGE_CACHE_STATUS_NO_MEMORY?-KERNEL_ENOMEM:-KERNEL_EIO;
                break;
            }
            if (within>=page->valid_bytes) break;
            size_t n=page->valid_bytes-within;
            if (n>wanted-available) n=wanted-available;
#if BOAROS_PAGE_CACHE_READAHEAD_PAGES
            miss|=page->cache_miss;
#endif
            available+=n;within=0;
            if (available==wanted || page->valid_bytes<BOAROS_PAGE_SIZE) break;
        }
        kernel_open_file_read_progress(input,position,available,miss);
        int64_t sent=available?regular_copy_output(output,NULL,pages,offset,available,
            out_start+total,limit,total==0):0;
        kernel_page_cache_put_batch(cache,pages,nr);
        if (sent<0) { error=(int)sent;break; }
        total+=(uint64_t)sent;
        if ((size_t)sent<available) break;
        /* Only a multi-page read can need the shared batch workspace.
         * Retry its unread suffix without that allocation under pressure. */
        if (error==-KERNEL_ENOMEM && nr>1) { capacity=1;continue; }
        if (available<wanted || error) break;
    }
    return total?(int64_t)total:error;
}

/* Same-inode copies use the established staging semantics; in particular a
 * sendfile destination may overwrite bytes in its current source page. */
static int64_t regular_file_copy(struct kernel_mm *mm,
    struct kernel_open_file_description *input,
    struct kernel_open_file_description *output,
    uint64_t start, uint64_t out_start, uint64_t count,
    struct kernel_vfs_size_limit *limit, int sync_each)
{
    unsigned flags=kernel_open_file_flags(output);
    int synchronous=!!(flags&KERNEL_FILES_O_DSYNC);
    int datasync=(flags&KERNEL_FILES_O_SYNC)!=KERNEL_FILES_O_SYNC;
    struct kernel_page_cache *cache=kernel_open_file_page_cache(input);
    if (cache && !kernel_open_file_memory_backed(input) &&
        !kernel_vfs_files_share_node(&input->file,&output->file) && !(sync_each && synchronous)) {
        int64_t copied=regular_cached_copy(input,output,cache,start,out_start,count,limit);
        if (copied>0 && synchronous) {
            int synced=kernel_vfs_sync(&output->file,datasync,&output->observed_writeback_error);
            if (synced) return synced;
        }
        return copied;
    }
    struct kernel_task_io_buffer buffer = {0};
    uint64_t total = 0;
    int error = 0;
    if (kernel_task_io_buffer_acquire(&buffer, mm->allocator)) return -KERNEL_ENOMEM;
    while (total < count) {
        size_t wanted = count - total > BOAROS_PAGE_SIZE
            ? BOAROS_PAGE_SIZE : (size_t)(count - total), available = 0;
        error = kernel_open_file_pread(input, start + total, buffer.data, wanted, &available);
        if (available > wanted) __builtin_trap();
        if (!available) break;
        int64_t sent=regular_copy_output(output,buffer.data,NULL,0,available,out_start+total,limit,total==0);
        if (sent < 0) { error = (int)sent; break; }
        if ((uint64_t)sent > available) __builtin_trap();
        if (sent && sync_each && synchronous) {
            int synced = kernel_vfs_sync(&output->file, datasync, &output->observed_writeback_error);
            if (synced) { error = synced; break; }
        }
        total += (uint64_t)sent;
        if ((size_t)sent < available || error) break;
    }
    kernel_task_io_buffer_release(&buffer);
    if (total && !sync_each && synchronous) {
        int synced = kernel_vfs_sync(&output->file, datasync, &output->observed_writeback_error);
        if (synced) return synced;
    }
    return total ? (int64_t)total : error;
}

static int offset_capable(const struct kernel_open_file_description *);
static int64_t file_into_pipe(struct kernel_open_file_description **,
    struct kernel_open_file_description *,uint64_t *,size_t,unsigned,int,int,unsigned *);

static int64_t sendfile_pinned(struct kernel_files *files, struct kernel_mm *mm,
    struct kernel_open_file_description *input,
    struct kernel_open_file_description **output_owner,
    int explicit_offset, int64_t *position, uint64_t count)
{
    struct kernel_open_file_description *output = *output_owner;
    if (output->kind==KERNEL_OPEN_FILE_KIND_PIPE) {
        if (input->kind!=KERNEL_OPEN_FILE_KIND_REGULAR) return -KERNEL_EINVAL;
        uint64_t start=(uint64_t)*position;unsigned broken=0;
        int64_t result=file_into_pipe(&input,output,&start,count,0,!explicit_offset,1,&broken);
        if (explicit_offset) *position=(int64_t)start;
        if (kernel_files_pipe_result(result,broken)) __builtin_trap();
        return result;
    }
    uint32_t socket_flags = output->kind == KERNEL_OPEN_FILE_KIND_SOCKET &&
        (kernel_open_file_flags(output) & KERNEL_FILES_O_NONBLOCK) ? KERNEL_SOCKET_MSG_DONTWAIT : 0;
    KERNEL_LOCK_SCOPE(first_guard);
    KERNEL_LOCK_SCOPE(second_guard);
    struct kernel_open_file_description *first =
        !explicit_offset && input->kind == KERNEL_OPEN_FILE_KIND_REGULAR ? input : 0;
    struct kernel_open_file_description *second =
        output->kind == KERNEL_OPEN_FILE_KIND_REGULAR && output != first ? output : 0;
    if (first != 0 && second != 0 && (uintptr_t)first > (uintptr_t)second) {
        struct kernel_open_file_description *swap = first; first = second; second = swap;
    }
    /* rank 10 的 OFD 锁按 key 取得；同一 OFD 只有一个锁 owner。 */
    if (first != 0) kernel_mutex_lock(&first->offset_lock, &first_guard);
    if (second != 0) kernel_mutex_lock(&second->offset_lock, &second_guard);
    uint64_t start = explicit_offset ? (uint64_t)*position : input->offset;
    uint64_t out_start = output->offset, total = 0;
    struct kernel_vfs_size_limit size_limit = {.maximum=kernel_task_current_fsize_limit()};
    int error = 0;
    if (count > (uint64_t)INT64_MAX || start > (uint64_t)INT64_MAX - count)
        return -KERNEL_EINVAL;
    if (count > KERNEL_FILES_MAX_RW_COUNT) count = KERNEL_FILES_MAX_RW_COUNT;
    uint64_t limit = input->kind == KERNEL_OPEN_FILE_KIND_REGULAR
        ? kernel_vfs_file_max_size(&input->file) : (uint64_t)INT64_MAX;
    if (output->kind == KERNEL_OPEN_FILE_KIND_REGULAR) {
        uint64_t out_limit = kernel_vfs_file_max_size(&output->file);
        if (limit > out_limit) limit = out_limit;
    }
    if (start > limit || (count != 0U && start == limit)) return -KERNEL_EOVERFLOW;
    if (count > limit - start) count = limit - start;
    if (out_start > (uint64_t)INT64_MAX - count) return -KERNEL_EINVAL;
    if (kernel_open_file_flags(output) & KERNEL_FILES_O_APPEND) return -KERNEL_EINVAL;
    if (input->kind != KERNEL_OPEN_FILE_KIND_REGULAR) return -KERNEL_EINVAL;
    if (count) kernel_open_file_read_begin(input, start);
    kernel_vfs_file_accessed(&input->file,kernel_open_file_flags(input));
    if (count == 0U || start >= kernel_open_file_size(input)) return 0;
    if (output->kind == KERNEL_OPEN_FILE_KIND_REGULAR) {
        int64_t result = regular_file_copy(mm, input, output, start, out_start, count, &size_limit, 1);
        kernel_files_size_limit_notify(&size_limit);
        if (result > 0) {
            if (explicit_offset || output != input) output->offset = out_start + (uint64_t)result;
            if (explicit_offset) *position = (int64_t)(start + (uint64_t)result);
            else input->offset = start + (uint64_t)result;
        }
        return result;
    }
    int datagram=kernel_socket_preserves_messages(kernel_open_file_socket(output));
    int direct=output->kind==KERNEL_OPEN_FILE_KIND_SOCKET && !datagram;
    struct kernel_task_io_buffer source_buffer = {0};
    if (!direct && !datagram && kernel_task_io_buffer_acquire(&source_buffer, mm->allocator) != KERNEL_TASK_STATUS_OK)
        return -KERNEL_ENOMEM;
    size_t capacity = BOAROS_PAGE_SIZE;
    void *staging = source_buffer.data, *datagram_staging = 0;
    if (datagram) {
        /* Linux splice_to_socket batches at most sixteen target pages. */
        size_t batch=(size_t)BOAROS_PAGE_SIZE*16U;
        capacity=count>batch ? batch : (size_t)count;
        enum kernel_heap_status allocated = kernel_heap_allocate(files->heap, capacity, &datagram_staging);
        if (allocated != KERNEL_HEAP_STATUS_OK) {
            if (source_buffer.allocator) kernel_task_io_buffer_release(&source_buffer);
            if (allocated != KERNEL_HEAP_STATUS_EMPTY) __builtin_trap();
            return -KERNEL_ENOMEM;
        }
        staging = datagram_staging;
    }
    uint64_t timeout = output->kind == KERNEL_OPEN_FILE_KIND_SOCKET && !(socket_flags & KERNEL_SOCKET_MSG_DONTWAIT)
        ? kernel_socket_send_timeout(kernel_open_file_socket(output)) : 0;
    uint64_t deadline = 0;
    if (timeout != 0U) {
        uint64_t now = kernel_time_monotonic_ns();
        deadline = UINT64_MAX - now < timeout ? UINT64_MAX : now + timeout;
    }
    while (total < count) {
        size_t wanted = count - total > capacity
            ? capacity : (size_t)(count - total), available = 0;
        if (direct) {
            uint64_t position=start+total,physical;
            const void *mapped=NULL;
            size_t offset=position&BOAROS_PAGE_MASK,valid;
            kernel_open_file_read_begin(input,position);
            int memory=kernel_open_file_memory_backed(input);
            int cache_miss=0;
            enum kernel_page_cache_status loaded;
#if BOAROS_PAGE_CACHE_READAHEAD_PAGES
            if (!memory) {
                struct kernel_page_cache_read_page page = {0};
                struct kernel_page_cache *cache=kernel_open_file_page_cache(input);
                loaded=kernel_page_cache_get_batch(cache,&input->file,
                    position>>BOAROS_PAGE_SHIFT,&page,1, kernel_open_file_read_no_reuse(input));
                if (!loaded) {
                    physical=page.physical_address;valid=page.valid_bytes;cache_miss=page.cache_miss;
                    mapped=kernel_page_cache_read_data(&page);
                    if (physical_page_acquire(cache->allocator, physical)) __builtin_trap();
                    kernel_page_cache_put_batch(cache, &page, 1);
                }
            } else
#endif
                loaded=memory?kernel_open_file_lookup_page(input,position>>BOAROS_PAGE_SHIFT,&physical,&valid):
                    kernel_open_file_get_page(input,position>>BOAROS_PAGE_SHIFT,&physical,&valid);
            if (loaded==KERNEL_PAGE_CACHE_STATUS_NOT_FOUND && memory) {
                uint64_t size=kernel_open_file_size(input);
                if (position>=size) break;
                available=BOAROS_PAGE_SIZE-offset;
                if (available>wanted) available=wanted;
                if (available>size-position) available=(size_t)(size-position);
                /* 稀疏内存文件的洞只生成当前零片段，不为读取建立持久文件页。 */
                if (kernel_task_io_buffer_acquire(&source_buffer,mm->allocator)) {
                    error=-KERNEL_ENOMEM;break;
                }
                staging=source_buffer.data;
                memset(staging,0,available);
            } else if (loaded==KERNEL_PAGE_CACHE_STATUS_OK) {
                /* 页引用跨目标等待保活；TCP仍COPY进ACK前自有的协议版本。 */
                kernel_task_io_buffer_take_page(&source_buffer,files->heap->page_allocator,physical,mapped);
                if (offset<valid) {
                    available=valid-offset;
                    if (available>wanted) available=wanted;
                }
                staging=(unsigned char *)source_buffer.data+offset;
            } else {
                if (loaded!=KERNEL_PAGE_CACHE_STATUS_OUT_OF_RANGE)
                    error=loaded==KERNEL_PAGE_CACHE_STATUS_NO_MEMORY?-KERNEL_ENOMEM:-KERNEL_EIO;
                break;
            }
            kernel_open_file_read_progress(input,position,available,cache_miss);
        } else {
            error=kernel_open_file_pread(input,start+total,staging,wanted,&available);
            if (available>wanted) __builtin_trap();
        }
        if (!available) {
            if (direct && source_buffer.allocator) kernel_task_io_buffer_release(&source_buffer);
            break;
        }
        int64_t sent=sendfile_output(output_owner,staging,available,deadline,socket_flags);
        if (direct) kernel_task_io_buffer_release(&source_buffer);
        if (sent < 0) { error = (int)sent; break; }
        if ((uint64_t)sent > available) __builtin_trap();
        total += (uint64_t)sent;
        if ((size_t)sent < available || error != 0) break;
    }
    if (datagram_staging) kernel_heap_release(files->heap, datagram_staging);
    if (source_buffer.allocator) kernel_task_io_buffer_release(&source_buffer);
    kernel_files_size_limit_notify(&size_limit);
    if (total != 0U && error == -KERNEL_ERESTARTSYS)
        kernel_signal_clear_syscall_restart(kernel_task_current());
    if (total != 0U) {
        if (explicit_offset) *position = (int64_t)(start + total);
        else input->offset = start + total;
        return (int64_t)total;
    }
    return error;
}

enum kernel_files_status kernel_files_sendfile(
    struct kernel_files *files, struct kernel_mm *mm, int64_t out_fd,
    int64_t in_fd, uint64_t user_offset, uint64_t count, int64_t *linux_result)
{
    KERNEL_SERVICE_SCOPE_IF(service, 0);
    COST_SCOPE(sendfile_cost, OPERATION_TICKS);
    struct kernel_open_file_description *input = 0, *output = 0;
    int64_t position = 0;
    size_t copied = 0;
    if (!kernel_files_is_live(files) || mm == 0 || linux_result == 0)
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    if (user_offset != 0U && (kernel_copy_from_user(mm, &position, user_offset,
            sizeof(position), &copied) != KERNEL_UACCESS_STATUS_OK || copied != sizeof(position))) {
        *linux_result = -KERNEL_EFAULT; return KERNEL_FILES_STATUS_OK;
    }
    enum kernel_files_status status = kernel_files_pin_data(files, in_fd, &input, linux_result);
    KERNEL_FILES_NOTE(files, read_calls, 1);
    KERNEL_FILES_NOTE(files, write_calls, 1);
    if (status != KERNEL_FILES_STATUS_OK || *linux_result != 0) goto done;
    if (!kernel_open_file_readable(input)) { *linux_result = -KERNEL_EBADF; goto done; }
    if (user_offset != 0U && (input->kind == KERNEL_OPEN_FILE_KIND_PIPE ||
            input->kind == KERNEL_OPEN_FILE_KIND_SOCKET || (input->device && !input->device->positioned))) {
        *linux_result = -KERNEL_ESPIPE; goto done;
    }
    uint64_t checked_position = (uint64_t)position;
    if (!user_offset) {
        /* Linux checks the input range before looking up out_fd. Snapshot the
         * shared offset here; sendfile_pinned locks and rechecks before I/O. */
        KERNEL_LOCK_SCOPE(snapshot);
        kernel_mutex_lock(&input->offset_lock, &snapshot);
        checked_position = input->offset;
    }
    if (position < 0 || count > (uint64_t)INT64_MAX ||
        checked_position > (uint64_t)INT64_MAX - count) {
        *linux_result = -KERNEL_EINVAL; goto done;
    }
    status = kernel_files_pin_data(files, out_fd, &output, linux_result);
    if (status != KERNEL_FILES_STATUS_OK || *linux_result != 0) goto done;
    if (!kernel_files_description_writable(output)) { *linux_result = -KERNEL_EBADF; goto done; }
    if (input->kind != KERNEL_OPEN_FILE_KIND_REGULAR ||
        (output->kind != KERNEL_OPEN_FILE_KIND_SOCKET && output->kind != KERNEL_OPEN_FILE_KIND_PIPE))
        kernel_service_scope_begin(&service);
    /* 两份 pin 由仍会恢复的 syscall 栈持有；退出先唤醒等待，正常展开再交还。 */
    if (!user_offset) position=(int64_t)checked_position;
    *linux_result = sendfile_pinned(files, mm, input, &output, user_offset != 0U, &position, count);
    if (*linux_result > 0) {
        kernel_vfs_notify_file(&input->file,VFS_NOTIFY_ACCESS);
        KERNEL_FILES_NOTE(files, bytes_read, (uint64_t)*linux_result);
        KERNEL_FILES_NOTE(files, bytes_written, (uint64_t)*linux_result);
    }
done:
    if (output != 0) status = kernel_files_release_io(files, &output, status);
    if (input != 0) status = kernel_files_release_io(files, &input, status);
    if (status != KERNEL_FILES_STATUS_OK) return status;
    /* 即使 do_sendfile 已失败也写回 offset；只读指针的 fault 覆盖先前结果。 */
    copied = 0;
    if (user_offset != 0U && (kernel_copy_to_user(mm, user_offset, &position,
            sizeof(position), &copied) != KERNEL_UACCESS_STATUS_OK || copied != sizeof(position)))
        *linux_result = -KERNEL_EFAULT;
    return KERNEL_FILES_STATUS_OK;
}

static int64_t copy_range_pinned(struct kernel_mm *mm,
    struct kernel_open_file_description *input,
    struct kernel_open_file_description **output_owner,
    uint64_t user_in, int64_t position_in, uint64_t user_out, int64_t position_out,
    uint64_t count)
{
    struct kernel_open_file_description *output = *output_owner;
    KERNEL_LOCK_SCOPE(first_guard);
    KERNEL_LOCK_SCOPE(second_guard);
    struct kernel_open_file_description *first = user_in ? 0 : input;
    struct kernel_open_file_description *second = !user_out && output != first ? output : 0;
    if (first && second && (uintptr_t)first > (uintptr_t)second) {
        struct kernel_open_file_description *swap = first; first = second; second = swap;
    }
    if (first) kernel_mutex_lock(&first->offset_lock, &first_guard);
    if (second) kernel_mutex_lock(&second->offset_lock, &second_guard);
    uint64_t start = user_in ? (uint64_t)position_in : input->offset;
    uint64_t out_start = user_out ? (uint64_t)position_out : output->offset;
    struct kernel_vfs_size_limit limit = {.maximum = kernel_task_current_fsize_limit()};
    /* Linux 先检查文件类型/打开模式和实例，再缩短 EOF/写入限额范围。 */
    uint32_t in_type = kernel_open_file_mode(input) & KERNEL_VFS_S_IFMT;
    uint32_t out_type = kernel_open_file_mode(output) & KERNEL_VFS_S_IFMT;
    if (in_type == KERNEL_VFS_S_IFDIR || out_type == KERNEL_VFS_S_IFDIR) return -KERNEL_EISDIR;
    if (input->kind != KERNEL_OPEN_FILE_KIND_REGULAR || output->kind != KERNEL_OPEN_FILE_KIND_REGULAR)
        return -KERNEL_EINVAL;
    if (!kernel_open_file_readable(input) || !kernel_files_description_writable(output) ||
        (kernel_open_file_flags(output) & KERNEL_FILES_O_APPEND)) return -KERNEL_EBADF;
    if (input->file.mount->private_data != output->file.mount->private_data) return -KERNEL_EXDEV;
    if (kernel_vfs_node_attributes(output->file.private_data)&KERNEL_VFS_INODE_IMMUTABLE) return -KERNEL_EPERM;
    if (start > UINT64_MAX - count || out_start > UINT64_MAX - count) return -KERNEL_EOVERFLOW;
    uint64_t size = kernel_open_file_size(input);
    if ((int64_t)start >= (int64_t)size) count = 0;
    else if (count > size - start) count = size - start;
    size_t allowed = (size_t)count;
    int error = kernel_vfs_check_write_limit(&limit, out_start, &allowed);
    if (error) { kernel_files_size_limit_notify(&limit); return error; }
    count = allowed;
    uint64_t maximum = kernel_vfs_file_max_size(&output->file);
    if ((int64_t)out_start >= (int64_t)maximum) return -KERNEL_EFBIG;
    if (count > maximum - out_start) count = maximum - out_start;
    if (kernel_vfs_files_share_node(&input->file, &output->file) &&
        out_start + count > start && out_start < start + count) return -KERNEL_EINVAL;
    if ((int64_t)start < 0 || (int64_t)out_start < 0) return -KERNEL_EINVAL;
    if (!count) return 0;
    if (count > KERNEL_FILES_MAX_RW_COUNT) count = KERNEL_FILES_MAX_RW_COUNT;
    kernel_open_file_read_begin(input, start);
    kernel_vfs_file_accessed(&input->file,kernel_open_file_flags(input));
    int64_t result = regular_file_copy(mm, input, output, start, out_start, count, &limit, 0);
    kernel_files_size_limit_notify(&limit);
    if (result > 0) {
        position_in = (int64_t)(start + (uint64_t)result);
        position_out = (int64_t)(out_start + (uint64_t)result);
        size_t copied = 0;
        /* 输入 copyout fault 仍须提交输出；两个用户指针重合时输出位置最终可见。 */
        if (user_in) {
            if (kernel_copy_to_user(mm, user_in, &position_in, sizeof(position_in), &copied) ||
                copied != sizeof(position_in)) result = -KERNEL_EFAULT;
        } else input->offset = (uint64_t)position_in;
        if (user_out) {
            copied = 0;
            if (kernel_copy_to_user(mm, user_out, &position_out, sizeof(position_out), &copied) ||
                copied != sizeof(position_out)) result = -KERNEL_EFAULT;
        } else output->offset = (uint64_t)position_out;
    }
    return result;
}

enum kernel_files_status kernel_files_copy_range(
    struct kernel_files *files, struct kernel_mm *mm, int64_t in_fd,
    uint64_t user_in, int64_t out_fd, uint64_t user_out,
    uint64_t count, uint32_t flags, int64_t *linux_result)
{
    KERNEL_SERVICE_SCOPE(service);
    struct kernel_open_file_description *input = 0, *output = 0;
    int64_t position_in = 0, position_out = 0;
    size_t copied = 0;
    if (!kernel_files_is_live(files) || !mm || !linux_result)
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    /* fd 的两次查找均先于用户 offset 读取；不同于 sendfile 的入口顺序。 */
    enum kernel_files_status status = kernel_files_pin_data(files, in_fd, &input, linux_result);
    if (status || *linux_result) goto done;
    status = kernel_files_pin_data(files, out_fd, &output, linux_result);
    if (status || *linux_result) goto done;
    if (user_in && (kernel_copy_from_user(mm, &position_in, user_in, sizeof(position_in), &copied) ||
        copied != sizeof(position_in))) { *linux_result = -KERNEL_EFAULT; goto done; }
    copied = 0;
    if (user_out && (kernel_copy_from_user(mm, &position_out, user_out, sizeof(position_out), &copied) ||
        copied != sizeof(position_out))) { *linux_result = -KERNEL_EFAULT; goto done; }
    if (flags) { *linux_result = -KERNEL_EINVAL; goto done; }
    KERNEL_FILES_NOTE(files, read_calls, 1);
    KERNEL_FILES_NOTE(files, write_calls, 1);
    *linux_result = copy_range_pinned(mm, input, &output, user_in, position_in,
        user_out, position_out, count);
    if (*linux_result > 0) {
        kernel_vfs_notify_file(&input->file,VFS_NOTIFY_ACCESS);
        KERNEL_FILES_NOTE(files, bytes_read, (uint64_t)*linux_result);
        KERNEL_FILES_NOTE(files, bytes_written, (uint64_t)*linux_result);
    }
done:
    if (output) status = kernel_files_release_io(files, &output, status);
    if (input) status = kernel_files_release_io(files, &input, status);
    return status;
}

#define SPLICE_NONBLOCK 2U
#define SPLICE_MORE 4U
#define SPLICE_ALL 15U

static int copy_offset_in(struct kernel_mm *mm,uint64_t user,int64_t *position)
{
    size_t copied;
    return user && (kernel_copy_from_user(mm,position,user,sizeof(*position),&copied) || copied!=sizeof(*position))?-KERNEL_EFAULT:0;
}
static int copy_offset_out(struct kernel_mm *mm,uint64_t user,int64_t position)
{
    size_t copied;
    return user && (kernel_copy_to_user(mm,user,&position,sizeof(position),&copied) || copied!=sizeof(position))?-KERNEL_EFAULT:0;
}
static int signed_transfer_range(uint64_t offset,uint64_t count)
{ return offset>INT64_MAX || count>INT64_MAX-offset?-KERNEL_EINVAL:0; }
static void release_transfer_page(struct kernel_pipe *pipe,void *page)
{
    uint64_t physical;
    if (!pipe->heap->physical_address(page,&physical) || physical_page_release(pipe->allocator,physical)) __builtin_trap();
}
static int new_transfer_page(struct kernel_pipe *pipe,void **page)
{
    uint64_t physical;
    enum physical_page_status status=physical_page_allocate(pipe->allocator,&physical);
    if (status==PHYSICAL_PAGE_STATUS_EMPTY) return -KERNEL_ENOMEM;
    if (status || physical_page_resolve(pipe->allocator,physical,page)) __builtin_trap();
    return 0;
}

/* Regular sources lend their real cached/memory page. Other read_iter-style
 * sources fill the final pipe page; no user address or second staging copy. */
static int read_transfer_page(struct kernel_open_file_description *file,struct kernel_pipe *pipe,
    uint64_t position,size_t wanted,void **page,size_t *offset,size_t *done,int *miss,uint32_t status_flags)
{
    *done=0;*offset=0;*miss=0;
    if (file->kind==KERNEL_OPEN_FILE_KIND_REGULAR) {
        uint64_t physical=0;size_t valid=0;
        enum kernel_page_cache_status status;
        if (kernel_open_file_memory_backed(file)) {
            uint64_t size=kernel_open_file_size(file);
            if (position>=size) return 0;
            status=kernel_open_file_lookup_page(file,position>>BOAROS_PAGE_SHIFT,&physical,&valid);
            if (status==KERNEL_PAGE_CACHE_STATUS_NOT_FOUND) {
                int error=new_transfer_page(pipe,page);if(error)return error;
                memset(*page,0,BOAROS_PAGE_SIZE);
                *done=size-position<wanted?(size_t)(size-position):wanted;
                return 0;
            }
        } else {
            struct kernel_page_cache_read_page part={0};
            struct kernel_page_cache *cache=kernel_open_file_page_cache(file);
            status=kernel_page_cache_get_batch(cache,&file->file,position>>BOAROS_PAGE_SHIFT,&part,1,
                kernel_open_file_read_no_reuse(file));
            if (!status) {
                physical=part.physical_address;valid=part.valid_bytes;
#if BOAROS_PAGE_CACHE_READAHEAD_PAGES
                *miss=part.cache_miss;
#endif
                if (physical_page_acquire(cache->allocator,physical)) __builtin_trap();
                kernel_page_cache_put_batch(cache,&part,1);
            }
        }
        if (status==KERNEL_PAGE_CACHE_STATUS_OUT_OF_RANGE) return 0;
        if (status) return status==KERNEL_PAGE_CACHE_STATUS_NO_MEMORY?-KERNEL_ENOMEM:-KERNEL_EIO;
        if (physical_page_resolve(pipe->allocator,physical,page)) __builtin_trap();
        *offset=position&BOAROS_PAGE_MASK;
        if (valid<=*offset) {release_transfer_page(pipe,*page);return 0;}
        *done=valid-*offset;if (*done>wanted) *done=wanted;
        return 0;
    }
    if (!file->block && file->kind!=KERNEL_OPEN_FILE_KIND_GENERATED &&
        !(file->device && file->device->splice_read && file->device->read)) return -KERNEL_EINVAL;
    if (file->kind == KERNEL_OPEN_FILE_KIND_GENERATED && kernel_vfs_file_control_prefix(&file->file))
        return -KERNEL_EINVAL;
    int error=new_transfer_page(pipe,page);
    if (error) return error;
    if (file->block) error=kernel_block_file_pread(file,position,*page,wanted,status_flags,done);
    else if (file->kind==KERNEL_OPEN_FILE_KIND_GENERATED && kernel_vfs_file_is_control(&file->file,0)) {
        error=kernel_vfs_file_control_permission(&file->file,0);
        if (!error) error=kernel_vfs_file_control(&file->file,0,&position,*page,wanted,done);
    }
    else if (file->kind==KERNEL_OPEN_FILE_KIND_GENERATED) {
        error=kernel_open_file_generate(file);
        if (!error && position<file->generated_length) {
            *done=file->generated_length-position;if (*done>wanted)*done=wanted;
            memcpy(*page,file->generated_data+position,*done);
        }
    } else if (file->device && file->device->splice_read && file->device->read)
        error=file->device->read(file->device_instance,kernel_task_current(),status_flags,*page,wanted,done);
    else error=-KERNEL_EINVAL;
    if (!*done) release_transfer_page(pipe,*page);
    return error;
}
static int64_t socket_into_pipe(struct kernel_open_file_description **input,
    struct kernel_pipe_write_borrow *output,size_t count,unsigned flags,unsigned *broken)
{
    struct kernel_socket *socket=kernel_open_file_socket(*input);
    struct kernel_socket_read_request request={0};
    int nonblocking=!!(kernel_open_file_flags(*input)&KERNEL_FILES_O_NONBLOCK);
    /* UNIX stream's splice_read also honors the pipe NONBLOCK hint. */
    if (kernel_socket_is_unix(socket) && kernel_socket_is_stream(socket) && (flags&SPLICE_NONBLOCK)) nonblocking=1;
    uint64_t deadline=0;int result;
    for (;;) {
        result=kernel_socket_reserve_read(socket,kernel_task_current(),&request,input,
            (uint32_t)count,1,NULL);
        if (result!=-KERNEL_EAGAIN || nonblocking) break;
        if (!deadline) {
            uint64_t timeout=kernel_socket_receive_timeout(socket),now=kernel_time_monotonic_ns();
            if (timeout) deadline=UINT64_MAX-now<timeout?UINT64_MAX:now+timeout;
        }
        uint64_t now=deadline?kernel_time_monotonic_ns():0;
        result=deadline && now>=deadline?-KERNEL_EAGAIN:
            kernel_socket_wait_ready(socket,KERNEL_POLLIN,deadline,0);
        if (result) break;
    }
    if (result<=0) {
        if (!result && request.socket) (void)kernel_socket_finish_read(&request,0);
        return result;
    }
    size_t total=0;int error=0;
    while (total<(size_t)result && output->slots) {
        void *page=NULL;error=new_transfer_page(output->pipe,&page);
        if (error) break;
        size_t n=(size_t)result-total;if(n>BOAROS_PAGE_SIZE)n=BOAROS_PAGE_SIZE;
        kernel_socket_copy_read(&request,(uint32_t)total,page,(uint32_t)n);
        error=kernel_pipe_push_page(output,page,0,n);
        if (error) {release_transfer_page(output->pipe,page);break;}
        total+=n;
    }
    if (!total) kernel_socket_abort_read(&request);
    else {
        request.completed_bytes=(uint32_t)total;
        (void)kernel_socket_finish_read(&request,total!=(size_t)result);
    }
    *broken=error==-KERNEL_EPIPE;
    return total?(int64_t)total:error;
}
static int64_t file_into_pipe(struct kernel_open_file_description **input,
    struct kernel_open_file_description *output,uint64_t *position,size_t count,unsigned flags,
    int shared,int sendfile,unsigned *broken)
{
    struct kernel_pipe *pipe=output->pipe;
    struct kernel_open_file_description *file=*input;
    if (sendfile) {
        uint64_t maximum=kernel_vfs_file_max_size(&file->file);
        if (*position>maximum || (count && *position==maximum)) return -KERNEL_EOVERFLOW;
        if (count>maximum-*position)count=(size_t)(maximum-*position);
        if (!count || *position>=kernel_open_file_size(file)) {
            kernel_vfs_file_accessed(&file->file,kernel_open_file_flags(file));return 0;
        }
    }
    struct kernel_pipe_write_borrow borrow={0};
    int error=kernel_pipe_write_begin(pipe,!!((flags&SPLICE_NONBLOCK) ||
        (kernel_open_file_flags(output)&KERNEL_FILES_O_NONBLOCK)),&borrow);
    if (error) {*broken=error==-KERNEL_EPIPE;return error;}
    uint64_t maximum=(uint64_t)borrow.slots*BOAROS_PAGE_SIZE;
    if (count>maximum) count=(size_t)maximum;
    if (count>KERNEL_FILES_MAX_RW_COUNT)count=KERNEL_FILES_MAX_RW_COUNT;
    if ((*input)->kind==KERNEL_OPEN_FILE_KIND_SOCKET) {
        int64_t result=socket_into_pipe(input,&borrow,count,flags,broken);
        kernel_pipe_write_finish(&borrow);return result;
    }
    uint64_t total=0;
    KERNEL_LOCK_SCOPE(position_owner);
    if (offset_capable(file)) kernel_mutex_lock(&file->offset_lock,&position_owner);
    if (shared && offset_capable(file)) *position=file->offset;
    error=signed_transfer_range(*position,count);
    if (!error && sendfile) {
        uint64_t maximum=kernel_vfs_file_max_size(&file->file);
        if (*position>maximum || (count && *position==maximum)) error=-KERNEL_EOVERFLOW;
        else if(count>maximum-*position)count=(size_t)(maximum-*position);
    }
    if (error) {
        if(position_owner.lock)kernel_lock_release(&position_owner);
        kernel_pipe_write_finish(&borrow);return error;
    }
    uint32_t status_flags=kernel_open_file_flags(file);
    if (file->kind==KERNEL_OPEN_FILE_KIND_REGULAR) {
        kernel_vfs_file_accessed(&file->file,status_flags);kernel_open_file_read_begin(file,*position);
    }
    while (total<count && borrow.slots) {
        void *page=NULL;size_t offset,done;int miss;
        size_t wanted=count-total;
        size_t capacity=file->kind==KERNEL_OPEN_FILE_KIND_REGULAR?
            BOAROS_PAGE_SIZE-(*position&BOAROS_PAGE_MASK):BOAROS_PAGE_SIZE;
        if(wanted>capacity)wanted=capacity;
        error=read_transfer_page(file,pipe,*position,wanted,&page,&offset,&done,&miss,status_flags);
        if (!done) break;
        int submitted=kernel_pipe_push_page(&borrow,page,offset,done);
        if (submitted) {release_transfer_page(pipe,page);error=submitted;break;}
        if (file->kind==KERNEL_OPEN_FILE_KIND_REGULAR) kernel_open_file_read_progress(file,*position,done,miss);
        if (file->kind==KERNEL_OPEN_FILE_KIND_REGULAR || file->kind==KERNEL_OPEN_FILE_KIND_GENERATED || file->block) *position+=done;
        total+=done;
        if (error) break;
    }
    if (shared && offset_capable(file)) file->offset=*position;
    if(position_owner.lock)kernel_lock_release(&position_owner);
    kernel_pipe_write_finish(&borrow);*broken=error==-KERNEL_EPIPE;
    if(total && error==-KERNEL_ERESTARTSYS)kernel_signal_clear_syscall_restart(kernel_task_current());
    return total?(int64_t)total:error;
}
static int64_t pipe_into_file(struct kernel_open_file_description *input,
    struct kernel_open_file_description **output,uint64_t *position,size_t count,unsigned flags,
    int shared,uint64_t *accepted)
{
    struct kernel_open_file_description *file=*output;
    if (file->kind!=KERNEL_OPEN_FILE_KIND_REGULAR && !file->block && file->kind!=KERNEL_OPEN_FILE_KIND_SOCKET &&
        !(file->device && file->device->splice_write && file->device->write)) return -KERNEL_EINVAL;
    struct kernel_pipe_read_borrow borrow={0};
    int64_t ready=kernel_pipe_read_begin(input->pipe,count,!!((flags&SPLICE_NONBLOCK) ||
        (kernel_open_file_flags(input)&KERNEL_FILES_O_NONBLOCK)),&borrow);
    if (ready<=0) return ready;
    KERNEL_LOCK_SCOPE(position_owner);
    if (offset_capable(file)) kernel_mutex_lock(&file->offset_lock,&position_owner);
    if (shared && offset_capable(file)) *position=file->offset;
    int error=signed_transfer_range(*position,count),sync_error=0;
    uint64_t total=0,start=*position;
    struct kernel_vfs_size_limit limit={.maximum=kernel_task_current_fsize_limit()};
    uint32_t status_flags=kernel_open_file_flags(file);
    uint32_t socket_flags=(flags&SPLICE_MORE)?KERNEL_SOCKET_MSG_MORE:0;
    if(status_flags&KERNEL_FILES_O_NONBLOCK)socket_flags|=KERNEL_SOCKET_MSG_DONTWAIT;
    uint64_t deadline=0;
    if(file->kind==KERNEL_OPEN_FILE_KIND_SOCKET) {
        uint64_t timeout=kernel_socket_send_timeout(kernel_open_file_socket(file)),now=kernel_time_monotonic_ns();
        if(timeout)deadline=UINT64_MAX-now<timeout?UINT64_MAX:now+timeout;
    }
    int message=file->kind==KERNEL_OPEN_FILE_KIND_SOCKET && kernel_socket_preserves_messages(kernel_open_file_socket(file));
    void *staging=NULL;size_t capacity=0;
    if(message && !error) {
        capacity=borrow.length;if(capacity>16U*BOAROS_PAGE_SIZE)capacity=16U*BOAROS_PAGE_SIZE;
        enum kernel_heap_status allocated=kernel_heap_allocate(file->heap,capacity,&staging);
        if(allocated && allocated!=KERNEL_HEAP_STATUS_EMPTY)__builtin_trap();
        if(allocated)error=-KERNEL_ENOMEM;
    }
    while (!error && total<borrow.length) {
        const void *data;size_t length;
        kernel_pipe_read_fragment(&borrow,(size_t)total,&data,&length);
        int64_t written;
        if(message) {
            size_t size=0;
            for(unsigned fragments=0;fragments<16 && total+size<borrow.length && size<capacity;fragments++) {
                kernel_pipe_read_fragment(&borrow,(size_t)total+size,&data,&length);
                if(length>capacity-size)length=capacity-size;
                memcpy((unsigned char *)staging+size,data,length);size+=length;
            }
            length=size;
            uint32_t send_flags=socket_flags;
            if(total+size<borrow.length)send_flags|=KERNEL_SOCKET_MSG_MORE;
            written=sendfile_output(output,staging,size,deadline,send_flags);
        } else if(file->kind==KERNEL_OPEN_FILE_KIND_REGULAR)
            written=regular_copy_output(file,data,NULL,0,length,*position,&limit,total==0);
        else if(file->block) {
            size_t done=0;error=kernel_block_file_pwrite(file,*position,data,length,status_flags,&done);
            written=done?(int64_t)done:error;
        } else written=sendfile_output(output,data,length,deadline,socket_flags);
        if(written<0){error=(int)written;break;}
        total+=(uint64_t)written;
        if(file->kind==KERNEL_OPEN_FILE_KIND_REGULAR || file->kind==KERNEL_OPEN_FILE_KIND_GENERATED || file->block)
            *position+=(uint64_t)written;
        if((size_t)written<length)break;
    }
    if(staging)kernel_heap_release(file->heap,staging);
    if(total && error==-KERNEL_ERESTARTSYS)kernel_signal_clear_syscall_restart(kernel_task_current());
    if(total && (status_flags&KERNEL_FILES_O_DSYNC)) {
        int datasync=(status_flags&KERNEL_FILES_O_SYNC)!=KERNEL_FILES_O_SYNC;
        if(file->kind==KERNEL_OPEN_FILE_KIND_REGULAR)
            sync_error=kernel_vfs_sync_range_borrowing(&file->file,*position-total,*position,datasync,
                &file->observed_writeback_error,NULL);
        else if(file->block)sync_error=kernel_block_file_sync(file);
    }
    /* iter_file_splice_write publishes neither ppos nor pipe consumption
     * when write_iter's final sync fails; the changed file data remains. */
    if(sync_error){*position=start;total=0;}
    *accepted=total;
    if(shared && offset_capable(file))file->offset=*position;
    if(position_owner.lock)kernel_lock_release(&position_owner);
    kernel_pipe_read_finish(&borrow,(size_t)total);
    kernel_files_size_limit_notify(&limit);
    return sync_error?sync_error:total?(int64_t)total:error;
}
static int offset_capable(const struct kernel_open_file_description *file)
{
    return file->kind==KERNEL_OPEN_FILE_KIND_REGULAR || file->kind==KERNEL_OPEN_FILE_KIND_GENERATED ||
        file->block || (file->device && file->device->positioned);
}

enum kernel_files_status kernel_files_splice(struct kernel_files *files,struct kernel_mm *mm,
    int64_t in_fd,uint64_t user_in,int64_t out_fd,uint64_t user_out,uint64_t count,uint32_t flags,int64_t *result)
{
    if(!kernel_files_is_live(files) || !mm || !result)return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    *result=0;
    if(!count)return KERNEL_FILES_STATUS_OK;
    if(flags&~SPLICE_ALL){*result=-KERNEL_EINVAL;return KERNEL_FILES_STATUS_OK;}
    struct kernel_open_file_description *input=NULL,*output=NULL;
    enum kernel_files_status status=kernel_files_pin(files,in_fd,&input,result);
    if(status || *result)goto done;
    status=kernel_files_pin(files,out_fd,&output,result);if(status || *result)goto done;
    int inp=input->kind==KERNEL_OPEN_FILE_KIND_PIPE,outp=output->kind==KERNEL_OPEN_FILE_KIND_PIPE;
    if((inp && user_in) || (outp && user_out)){*result=-KERNEL_ESPIPE;goto done;}
    int64_t in=0,out=0;
    *result=copy_offset_in(mm,user_out,&out);if(*result)goto done;
    *result=copy_offset_in(mm,user_in,&in);if(*result)goto done;
    if(!kernel_open_file_readable(input) || !kernel_files_description_writable(output)){*result=-KERNEL_EBADF;goto done;}
    if((!inp && !outp) || (inp && outp && input->pipe==output->pipe)){*result=-KERNEL_EINVAL;goto done;}
    if((user_in && !offset_capable(input)) || (user_out && !offset_capable(output))){*result=-KERNEL_EINVAL;goto done;}
    if(inp && !outp && (kernel_open_file_flags(output)&KERNEL_FILES_O_APPEND)){*result=-KERNEL_EINVAL;goto done;}
    {
        KERNEL_SERVICE_SCOPE_IF(service,
            (!inp && (input->kind==KERNEL_OPEN_FILE_KIND_GENERATED || input->device ||
                (input->kind==KERNEL_OPEN_FILE_KIND_REGULAR && kernel_vfs_file_metadata_requires_cpu0(&input->file)))) ||
            (!outp && (output->device || (output->kind==KERNEL_OPEN_FILE_KIND_REGULAR && kernel_vfs_file_metadata_requires_cpu0(&output->file)))));
        struct kernel_open_file_description *file=inp?output:input;
        KERNEL_LOCK_SCOPE(position_owner);
        if(!inp || !outp) {
            if(offset_capable(file))kernel_mutex_lock(&file->offset_lock,&position_owner);
            if(inp) {if(!user_out)out=(int64_t)file->offset;*result=signed_transfer_range((uint64_t)out,count);}
            else {if(!user_in)in=(int64_t)file->offset;*result=signed_transfer_range((uint64_t)in,count);}
            if(position_owner.lock)kernel_lock_release(&position_owner);
            if(*result)goto transfer_done;
        }
        unsigned broken=0;uint64_t accepted=0;
        if(inp && outp) {
            *result=kernel_pipe_link(input->pipe,output->pipe,count,!!((flags&SPLICE_NONBLOCK) ||
                ((kernel_open_file_flags(input)|kernel_open_file_flags(output))&KERNEL_FILES_O_NONBLOCK)),1,&broken);
        } else if(outp) {
            uint64_t position=(uint64_t)in;
            *result=file_into_pipe(&input,output,&position,count,flags,!user_in,0,&broken);
            in=(int64_t)position;
        } else {
            uint64_t position=(uint64_t)out;
            *result=pipe_into_file(input,&output,&position,count,flags,!user_out,&accepted);
            out=(int64_t)position;
        }
        if(kernel_files_pipe_result(*result,broken))status=KERNEL_FILES_STATUS_STATE;
        if(*result>0 || accepted) {
            int64_t completed=*result>0?*result:(int64_t)accepted;
            kernel_vfs_notify_file(&input->file,VFS_NOTIFY_ACCESS);kernel_vfs_notify_file(&output->file,VFS_NOTIFY_MODIFY);
            KERNEL_FILES_NOTE(files,bytes_read,(uint64_t)completed);
            KERNEL_FILES_NOTE(files,bytes_written,(uint64_t)completed);
        }
        if(position_owner.lock)kernel_lock_release(&position_owner);
        /* Output faults preserve already consumed pipe bytes and accepted data. */
        if(*result>=0) {
            int error=copy_offset_out(mm,user_out,out);
            if(!error)error=copy_offset_out(mm,user_in,in);
            if(error)*result=error;
        }
transfer_done:;
    }
done:
    if(output)status=kernel_files_release_io(files,&output,status);
    if(input)status=kernel_files_release_io(files,&input,status);
    return status;
}
enum kernel_files_status kernel_files_tee(struct kernel_files *files,int64_t in_fd,int64_t out_fd,
    uint64_t count,uint32_t flags,int64_t *result)
{
    *result=0;
    if(flags&~SPLICE_ALL){*result=-KERNEL_EINVAL;return KERNEL_FILES_STATUS_OK;}
    if(!count)return KERNEL_FILES_STATUS_OK;
    KERNEL_FILES_PIN_SCOPE(a);KERNEL_FILES_PIN_SCOPE(b);
    struct kernel_open_file_description *in=kernel_files_hold_fd(files,in_fd,&a);
    if(!in){*result=-KERNEL_EBADF;return KERNEL_FILES_STATUS_OK;}
    struct kernel_open_file_description *out=kernel_files_hold_fd(files,out_fd,&b);
    if(!out || !kernel_open_file_readable(in) || !kernel_files_description_writable(out)){*result=-KERNEL_EBADF;return KERNEL_FILES_STATUS_OK;}
    if(in->kind!=KERNEL_OPEN_FILE_KIND_PIPE || out->kind!=KERNEL_OPEN_FILE_KIND_PIPE || in->pipe==out->pipe){*result=-KERNEL_EINVAL;return KERNEL_FILES_STATUS_OK;}
    unsigned broken=0;
    *result=kernel_pipe_link(in->pipe,out->pipe,count,!!((flags&SPLICE_NONBLOCK) ||
        ((kernel_open_file_flags(in)|kernel_open_file_flags(out))&KERNEL_FILES_O_NONBLOCK)),0,&broken);
    if(kernel_files_pipe_result(*result,broken))return KERNEL_FILES_STATUS_STATE;
    if(*result>0){
        kernel_vfs_notify_file(&in->file,VFS_NOTIFY_ACCESS);kernel_vfs_notify_file(&out->file,VFS_NOTIFY_MODIFY);
    }
    return KERNEL_FILES_STATUS_OK;
}
enum kernel_files_status kernel_files_vmsplice(struct kernel_files *files,struct kernel_mm *mm,
    int64_t fd,uint64_t user_iov,uint64_t count,uint32_t flags,int64_t *result)
{
    *result=0;
    if(flags&~SPLICE_ALL){*result=-KERNEL_EINVAL;return KERNEL_FILES_STATUS_OK;}
    KERNEL_FILES_PIN_SCOPE(pin);
    struct kernel_open_file_description *file=kernel_files_hold_fd(files,fd,&pin);
    if(!file || file->kind==KERNEL_OPEN_FILE_KIND_PATH){*result=-KERNEL_EBADF;return KERNEL_FILES_STATUS_OK;}
    int writing=kernel_open_file_writable(file);
    if(!writing && !kernel_open_file_readable(file)){*result=-KERNEL_EBADF;return KERNEL_FILES_STATUS_OK;}
    if(count>1024){*result=-KERNEL_EINVAL;return KERNEL_FILES_STATUS_OK;}
    struct kernel_uaccess_iov_snapshot snapshot={0};
    enum kernel_files_status status=kernel_uaccess_iov_import(&snapshot,files->heap,mm,user_iov,(size_t)count,result);
    if(status || *result || !snapshot.total)goto done;
    if(file->kind!=KERNEL_OPEN_FILE_KIND_PIPE){*result=-KERNEL_EBADF;goto done;}
    unsigned broken=0;
    int nonblocking=!!(flags&SPLICE_NONBLOCK);
    if(writing)*result=kernel_pipe_vmsplice(file->pipe,mm,snapshot.view,snapshot.count,snapshot.total,nonblocking,&broken);
    else if(kernel_pipe_readv(file->pipe,mm,snapshot.view,snapshot.count,snapshot.total,nonblocking?KERNEL_PIPE_NONBLOCK:0,result))status=KERNEL_FILES_STATUS_STATE;
    if(kernel_files_pipe_result(*result,broken))status=KERNEL_FILES_STATUS_STATE;
    if(*result>0) {
        kernel_vfs_notify_file(&file->file,writing?VFS_NOTIFY_MODIFY:VFS_NOTIFY_ACCESS);
    }
done:
    kernel_uaccess_iov_cleanup(&snapshot,files->heap);
    return status;
}
