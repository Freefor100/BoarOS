#include <kernel/cost.h>
#include <arch/riscv/mm.h>
#include <arch/riscv/sbi.h>
#include <arch/riscv/virt_uart.h>
#include <arch/riscv/virtio_mmio_block.h>
#include <kernel/dtb.h>
#include <kernel/errno.h>
#include <kernel/files.h>
#include <kernel/fs_context.h>
#include <kernel/heap.h>
#include <kernel/mm.h>
#include <kernel/open_file.h>
#include <kernel/page.h>
#include <kernel/page_cache.h>
#include <kernel/socket.h>
#include <kernel/shm.h>
#include <kernel/scheduler.h>
#include <kernel/time.h>
#include <kernel/uaccess.h>
#include <kernel/vfs.h>
#include <string.h>
#include "lwip/memp.h"

#define USER UINT64_C(0x10000)
#define BUFFER UINT64_C(0x100000)
#define MIB UINT64_C(1048576)
static unsigned char pool[128 * MIB] __attribute__((aligned(4096)));
static unsigned char payload[4096];
static uint64_t active_satp;
#if BOAROS_COST_DIAGNOSTICS
static uint32_t cost_frequency;
static unsigned cost_copy_measuring;
static uint64_t cost_copy_observed;
#endif
enum kernel_uaccess_status __real_kernel_copy_from_user(struct kernel_mm *, void *, uint64_t, size_t, size_t *);
enum kernel_uaccess_status __wrap_kernel_copy_from_user(struct kernel_mm *mm, void *to, uint64_t from, size_t size, size_t *copied)
{
    enum kernel_uaccess_status status = __real_kernel_copy_from_user(mm, to, from, size, copied);
#if BOAROS_COST_DIAGNOSTICS
    if (cost_copy_measuring) cost_copy_observed += *copied;
#endif
    return status;
}

static unsigned freeze_lwip_clock;
static uint32_t frozen_lwip_ms;
uint32_t __real_sys_now(void);
uint32_t __wrap_sys_now(void)
{ return freeze_lwip_clock ? frozen_lwip_ms : __real_sys_now(); }
static unsigned polling_snapshot, snapshot_protocol_calls;
struct netif;
void __real_netif_poll_all(void);
void __wrap_netif_poll_all(void)
{ if (polling_snapshot) snapshot_protocol_calls++; __real_netif_poll_all(); }
void __real_sys_check_timeouts(void);
void __wrap_sys_check_timeouts(void)
{ if (polling_snapshot) snapshot_protocol_calls++; __real_sys_check_timeouts(); }
unsigned __real_netif_poll_budget(struct netif *, unsigned);
unsigned __wrap_netif_poll_budget(struct netif *netif, unsigned budget)
{ if (polling_snapshot) snapshot_protocol_calls++; return __real_netif_poll_budget(netif, budget); }
unsigned __real_sys_check_timeouts_budget(unsigned);
unsigned __wrap_sys_check_timeouts_budget(unsigned budget)
{ if (polling_snapshot) snapshot_protocol_calls++; return __real_sys_check_timeouts_budget(budget); }

/* This boot fixture has no scheduled tasks. Reject any attempted blocking;
 * only an empty socket queue's notification may be ignored. */
enum kernel_scheduler_status __wrap_kernel_wait_queue_wake_all(struct kernel_wait_queue *queue)
{
    if (queue->head != 0) __builtin_trap();
    return KERNEL_SCHEDULER_STATUS_OK;
}
static unsigned fail_page;
static unsigned fail_metadata, fail_packet;
static unsigned timer_probe, timer_heap_calls;
enum kernel_heap_status __real_kernel_heap_allocate(struct kernel_heap *, size_t, void **);
enum kernel_heap_status __wrap_kernel_heap_allocate(struct kernel_heap *heap, size_t size, void **out)
{
    if (timer_probe) timer_heap_calls++;
    if (fail_packet && --fail_packet == 0) return KERNEL_HEAP_STATUS_EMPTY;
    return __real_kernel_heap_allocate(heap, size, out);
}
enum kernel_heap_status __real_kernel_heap_allocate_zeroed(struct kernel_heap *, size_t, size_t, void **);
enum kernel_heap_status __wrap_kernel_heap_allocate_zeroed(struct kernel_heap *heap, size_t n, size_t size, void **out)
{
    if (timer_probe) timer_heap_calls++;
    if (fail_packet && --fail_packet == 0) return KERNEL_HEAP_STATUS_EMPTY;
    if (fail_metadata && --fail_metadata == 0) return KERNEL_HEAP_STATUS_EMPTY;
    return __real_kernel_heap_allocate_zeroed(heap, n, size, out);
}
enum kernel_heap_status __real_kernel_heap_resize(struct kernel_heap *, void *, size_t, void **);
enum kernel_heap_status __wrap_kernel_heap_resize(struct kernel_heap *heap, void *old, size_t size, void **out)
{
    if (fail_metadata && --fail_metadata == 0) return KERNEL_HEAP_STATUS_EMPTY;
    return __real_kernel_heap_resize(heap, old, size, out);
}
uint64_t __wrap_riscv_sv39_current_satp(void) { return active_satp; }
enum physical_page_status __real_physical_page_allocate(struct physical_page_allocator *, uint64_t *);
enum physical_page_status __wrap_physical_page_allocate(struct physical_page_allocator *a, uint64_t *p)
{
    if (fail_page && --fail_page == 0) return PHYSICAL_PAGE_STATUS_EMPTY;
    return __real_physical_page_allocate(a, p);
}
static void *access_page(uint64_t address) { return (void *)(uintptr_t)address; }
static int physical(const void *p, uint64_t *out) { *out = (uintptr_t)p; return 1; }
static int dma(const void *p, size_t n, uint64_t *out) { (void)n; return physical(p, out); }
static void check(int good, unsigned test)
{
    if (!good) {
        virt_uart_puts("scale failed: "); virt_uart_put_hex(test); virt_uart_putc('\n');
        sbi_shutdown();
    }
}
static void number(const char *label, uint64_t value)
{
    virt_uart_puts(label); virt_uart_put_hex(value); virt_uart_putc('\n');
}
static void tcp_cost(struct kernel_files *files, struct kernel_mm *mm)
{
    int64_t listener_fd, client_fd, result;
    struct kernel_open_file_description *listener = 0, *client = 0;
    struct kernel_socket *accepted = 0;
    struct kernel_socket_address address = {.family = KERNEL_SOCKET_AF_INET};
    check(kernel_files_socket_create(files, 2, 1, 0, &listener_fd) == KERNEL_FILES_STATUS_OK && listener_fd >= 0 &&
          kernel_files_socket_create(files, 2, 1, 00004000, &client_fd) == KERNEL_FILES_STATUS_OK && client_fd >= 0 &&
          kernel_files_pin(files, listener_fd, &listener, &result) == KERNEL_FILES_STATUS_OK && result == 0 &&
          kernel_files_pin(files, client_fd, &client, &result) == KERNEL_FILES_STATUS_OK && result == 0, 30);
    struct kernel_socket *server_socket = kernel_open_file_socket(listener);
    struct kernel_socket *client_socket = kernel_open_file_socket(client);
    struct kernel_socket_address remote = {.family = KERNEL_SOCKET_AF_INET, .bytes = {127,0,0,1}};
    check(kernel_socket_bind(server_socket, &address) == 0 &&
          kernel_socket_getname(server_socket, &address) == 0 &&
          kernel_socket_listen(server_socket, 1) == 0 &&
          (remote.port = address.port, kernel_socket_connect(client_socket, &remote, 0)) == 0 &&
          kernel_socket_accept(server_socket, &accepted) == 0, 31);
    struct kernel_socket *idle[16] = {0};
    for (unsigned i = 0; i < 16; i++)
        check(kernel_socket_create(files->heap, KERNEL_SOCKET_AF_INET, 1, &idle[i]) == 0, 222);
#if BOAROS_COST_DIAGNOSTICS
    check(kernel_cost_begin(4, cost_frequency, 1, 0) == 0, 236);
#endif
    polling_snapshot = 1;
    for (unsigned i = 0; i < 64; i++)
        check((kernel_socket_poll(client_socket, 0) & KERNEL_POLLOUT) != 0, 220);
    polling_snapshot = 0;
#if BOAROS_COST_DIAGNOSTICS
    uint64_t polls, services, scans;
    check(kernel_cost_end(4, 0) == 0 &&
          kernel_cost_read(0, COST_NETWORK_POLL_CALLS, &polls) == 0 && polls == 64 &&
          kernel_cost_read(0, COST_NETWORK_POLL_SERVICES, &services) == 0 && services == 0 &&
          kernel_cost_read(0, COST_NETWORK_GLOBAL_SCANS, &scans) == 0 && scans == 0, 237);
#endif
    number("poll protocol calls: ", snapshot_protocol_calls);
    check(snapshot_protocol_calls == 0, 221);
    for (unsigned i = 0; i < 16; i++) kernel_socket_destroy(idle[i]);
    struct kernel_socket_statistics before, after;
    kernel_socket_get_statistics(&before);
    uint64_t resolutions = kernel_uaccess_page_resolutions();
    check(kernel_files_write(files, mm, client_fd, BUFFER + 1, 8192, &result) ==
          KERNEL_FILES_STATUS_OK && result == 8192, 32);
    kernel_socket_get_statistics(&after);
    resolutions = kernel_uaccess_page_resolutions() - resolutions;
    number("TCP 8192-byte submissions: ", after.tcp_write_calls - before.tcp_write_calls);
    number("TCP user-page resolutions: ", resolutions);
    check(after.tcp_write_calls - before.tcp_write_calls <= 8 && resolutions <= 8 &&
          after.tcp_written_bytes - before.tcp_written_bytes == 8192, 33);
    struct kernel_open_file_description *reader=0;
    check(kernel_open_file_create_socket(files->heap,accepted,0,&reader)==KERNEL_OPEN_FILE_STATUS_OK, 201);
    uint32_t consumed=0;
    while(consumed<8192){
        struct kernel_socket_read_request request={0};
        int got=kernel_socket_reserve_read(accepted,0,&request,&reader,sizeof(payload),0);
        check(got>0,202);kernel_socket_finish_read(&request,0);consumed+=(uint32_t)got;
    }
    struct kernel_socket_write_request reservation = {0}, competing = {0};
    struct kernel_open_file_description *second_pin = client;
    check(kernel_open_file_acquire(second_pin) == KERNEL_OPEN_FILE_STATUS_OK, 240);
    kernel_socket_stream_begin(&reservation, &client);
    int reserved = kernel_socket_stream_reserve(&reservation, UINT16_MAX, 0);
    check(reserved > 0 && !client && !(kernel_socket_poll(client_socket, 0) & KERNEL_POLLOUT), 241);
    kernel_socket_stream_begin(&competing, &second_pin);
    check(kernel_socket_stream_reserve(&competing, 1, 0) == -KERNEL_EAGAIN, 242);
    resolutions = kernel_uaccess_page_resolutions();
#if BOAROS_COST_DIAGNOSTICS
    check(kernel_cost_begin(5, cost_frequency, 1, 0) == 0, 243);
#endif
    check(kernel_files_write(files, mm, client_fd, BUFFER, 4096, &result) == KERNEL_FILES_STATUS_OK &&
          result == -KERNEL_EAGAIN && kernel_uaccess_page_resolutions() == resolutions, 244);
#if BOAROS_COST_DIAGNOSTICS
    uint64_t denied, copied_blocked, copied_stream;
    check(kernel_cost_end(5, 0) == 0 &&
          kernel_cost_read(0, COST_STREAM_ADMIT_BLOCKED, &denied) == 0 && denied == 1 &&
          kernel_cost_read(0, COST_STREAM_COPY_BLOCKED_BYTES, &copied_blocked) == 0 && !copied_blocked &&
          kernel_cost_read(0, COST_STREAM_COPY, &copied_stream) == 0 && !copied_stream, 245);
#endif
    kernel_socket_stream_cancel(&reservation);
    check(kernel_socket_stream_reserve(&competing, 3, 0) == 3, 246);
    /* 故障/退出走同一个abort；其他请求随后必须重新拿到这份接纳量。 */
    kernel_socket_abort_write(&competing);
    check(kernel_socket_stream_reserve(&reservation, (uint32_t)reserved, 0) == reserved, 247);
    kernel_socket_stream_finish(&reservation);
    check(client && kernel_socket_poll(client_socket, 0) & KERNEL_POLLOUT, 248);
    virt_uart_puts("TCP admission: competing reservations, cancel and zero-copy EAGAIN passed\n");
    uintptr_t core = kernel_socket_protocol_enter();
    check(kernel_socket_write_buffer(client_socket, "batch", 5, 0) == 5, 223);
    kernel_socket_protocol_leave(core);
    check(!(kernel_socket_poll(accepted, 0) & KERNEL_POLLIN), 224);
    struct kernel_socket_service_result service = kernel_socket_service_pending(
        (struct kernel_socket_service_budget){0, 0, 0});
    check(!service.sockets && !service.packets && !service.timers && service.runnable, 225);
    unsigned rounds = 0;
    do {
        service = kernel_socket_service_pending((struct kernel_socket_service_budget){1, 1, 0});
        check(service.sockets <= 1 && service.packets <= 1 && !service.timers && ++rounds < 64, 226);
    } while (service.runnable);
    struct kernel_socket_read_request budget_read = {0};
    char budget_bytes[5];
    check(kernel_socket_reserve_read(accepted, 0, &budget_read, &reader, 5, 0) == 5, 227);
    kernel_socket_copy_read(&budget_read, 0, budget_bytes, 5);
    check(!memcmp(budget_bytes, "batch", 5), 228);
    kernel_socket_finish_read(&budget_read, 0);
    static void *held_segments[MEMP_NUM_TCP_SEG];
    unsigned held = 0;
    core = kernel_socket_protocol_enter();
    while (held < MEMP_NUM_TCP_SEG && (held_segments[held] = memp_malloc(MEMP_TCP_SEG))) held++;
    check(held > 1 && !memp_malloc(MEMP_TCP_SEG), 229);
    frozen_lwip_ms = __real_sys_now(); freeze_lwip_clock = 1;
    memp_free(MEMP_TCP_SEG, held_segments[--held]);
    check(kernel_socket_write_buffer(client_socket, payload, 2 * TCP_MSS, 0) == -KERNEL_EAGAIN &&
          !(kernel_socket_poll(client_socket, 0) & KERNEL_POLLOUT), 230);
    memp_free(MEMP_TCP_SEG, held_segments[--held]);
    kernel_socket_protocol_leave(core);
    /* 不推进任何timer；归还事件必须直接恢复真正的池等待者。 */
    service = kernel_socket_service_pending((struct kernel_socket_service_budget){8, 0, 0});
    check(!service.timers && !service.packets, 231);
    check(kernel_socket_poll(client_socket, 0) & KERNEL_POLLOUT, 232);
    freeze_lwip_clock = 0;
    core = kernel_socket_protocol_enter();
    while (held) memp_free(MEMP_TCP_SEG, held_segments[--held]);
    kernel_socket_protocol_leave(core);
    check(kernel_socket_write_buffer(client_socket, "pool", 4, 0) == 4, 233);
    budget_read = (struct kernel_socket_read_request){0};
    check(kernel_socket_reserve_read(accepted, 0, &budget_read, &reader, 4, 0) == 4, 234);
    kernel_socket_copy_read(&budget_read, 0, budget_bytes, 4);
    check(!memcmp(budget_bytes, "pool", 4), 235);
    kernel_socket_finish_read(&budget_read, 0);
    virt_uart_puts("protocol budget and capacity progress passed\n");
    /* 强制协议保留一次未接纳的 pbuf；IRQ 重试不能重入被中断的堆分配。 */
    fail_packet=1;
    check(kernel_socket_write_buffer(client_socket,"retry",5,0)==5 && fail_packet==0,203);
    uint64_t deadline=kernel_time_monotonic_ns()+UINT64_C(300000000);
    timer_probe=1;
    do{kernel_socket_expire_timers();}while(kernel_time_monotonic_ns()<deadline);
    timer_probe=0;check(timer_heap_calls==0,204);
    struct kernel_socket_read_request request={0};
    check(kernel_socket_reserve_read(accepted,0,&request,&reader,sizeof(payload),0)==5,205);
    char retry[5];kernel_socket_copy_read(&request,0,retry,5);check(!memcmp(retry,"retry",5),206);
    kernel_socket_finish_read(&request,0);
    check(kernel_open_file_release(&reader)==KERNEL_OPEN_FILE_STATUS_OK,207);
    check(kernel_open_file_release(&client) == KERNEL_OPEN_FILE_STATUS_OK &&
          kernel_open_file_release(&listener) == KERNEL_OPEN_FILE_STATUS_OK &&
          kernel_files_close(files, client_fd, &result) == KERNEL_FILES_STATUS_OK && result == 0 &&
          kernel_files_close(files, listener_fd, &result) == KERNEL_FILES_STATUS_OK && result == 0, 34);
}

static void udp_buffer_oom(struct kernel_files *files, struct kernel_mm *mm)
{
    int64_t fd, result;
    struct kernel_open_file_description *pin = 0;
    check(kernel_files_socket_create(files, 2, 2, 00004000, &fd) == KERNEL_FILES_STATUS_OK && fd >= 0 &&
          kernel_files_pin(files, fd, &pin, &result) == KERNEL_FILES_STATUS_OK && result == 0, 36);
    struct kernel_socket *socket = kernel_open_file_socket(pin);
    struct kernel_socket_address address = {.family = KERNEL_SOCKET_AF_INET};
    check(kernel_socket_bind(socket, &address) == 0 && kernel_socket_getname(socket, &address) == 0 &&
          (address.bytes[0] = 127, address.bytes[3] = 1, kernel_socket_sendto(socket, mm, BUFFER, 8192, &address)) == 8192, 37);
    fail_page = 1;
    check(kernel_files_read(files, mm, fd, BUFFER, 8192, &result) == KERNEL_FILES_STATUS_OK &&
          result == -KERNEL_ENOMEM && fail_page == 0, 38);
    /* Failure must leave the complete datagram owned by the socket queue. */
    check(kernel_socket_recvfrom(socket, mm, BUFFER, 8192, &address) == 8192, 39);
    for (unsigned page = 0; page < 2; page++) {
        size_t copied;
        check(kernel_copy_from_user(mm, payload, BUFFER + page * 4096, sizeof(payload), &copied) ==
              KERNEL_UACCESS_STATUS_OK && copied == sizeof(payload), 40);
        for (unsigned i = 0; i < sizeof(payload); i++) check(payload[i] == (unsigned char)(i * 17 + 3), 41);
    }
    check(kernel_open_file_release(&pin) == KERNEL_OPEN_FILE_STATUS_OK &&
          kernel_files_close(files, fd, &result) == KERNEL_FILES_STATUS_OK && result == 0, 42);
}

static void socketpair_scale(struct kernel_files *files, struct kernel_mm *mm)
{
    int64_t result;
    int32_t pair[2];
    size_t copied;

    /* 1. SOCK_STREAM full-duplex */
    check(kernel_files_socketpair_create(files, mm, 1, 0, BUFFER, &result) ==
          KERNEL_FILES_STATUS_OK && result == 0, 80);
    check(kernel_copy_from_user(mm, pair, BUFFER, sizeof(pair), &copied) ==
          KERNEL_UACCESS_STATUS_OK && copied == sizeof(pair), 81);
    check(pair[0] >= 0 && pair[1] >= 0 && pair[0] != pair[1], 82);

    unsigned char test_data[100];
    for (unsigned i = 0; i < sizeof(test_data); i++) test_data[i] = (unsigned char)(i + 0x5a);
    check(kernel_copy_to_user(mm, BUFFER + 64, test_data, sizeof(test_data), &copied) ==
          KERNEL_UACCESS_STATUS_OK && copied == sizeof(test_data), 83);
    check(kernel_files_write(files, mm, pair[0], BUFFER + 64, sizeof(test_data), &result) ==
          KERNEL_FILES_STATUS_OK && result == (int64_t)sizeof(test_data), 84);

    unsigned char recv_data[100];
    check(kernel_files_read(files, mm, pair[1], BUFFER + 256, sizeof(recv_data), &result) ==
          KERNEL_FILES_STATUS_OK && result == (int64_t)sizeof(recv_data), 85);
    check(kernel_copy_from_user(mm, recv_data, BUFFER + 256, sizeof(recv_data), &copied) ==
          KERNEL_UACCESS_STATUS_OK && copied == sizeof(recv_data), 86);
    for (unsigned i = 0; i < sizeof(recv_data); i++) check(recv_data[i] == test_data[i], 87);

    /* Reverse write: pair[1] -> pair[0] */
    check(kernel_files_write(files, mm, pair[1], BUFFER + 64, 40, &result) ==
          KERNEL_FILES_STATUS_OK && result == 40, 88);
    check(kernel_files_read(files, mm, pair[0], BUFFER + 256, 40, &result) ==
          KERNEL_FILES_STATUS_OK && result == 40, 89);

    /* Close pair[0]: pair[1] should observe EOF on read and EPIPE on write */
    check(kernel_files_close(files, pair[0], &result) == KERNEL_FILES_STATUS_OK && result == 0, 90);
    check(kernel_files_read(files, mm, pair[1], BUFFER + 256, 40, &result) ==
          KERNEL_FILES_STATUS_OK && result == 0, 91);
    check(kernel_files_write(files, mm, pair[1], BUFFER + 64, 40, &result) ==
          KERNEL_FILES_STATUS_OK && result == -KERNEL_EPIPE, 92);
    check(kernel_files_close(files, pair[1], &result) == KERNEL_FILES_STATUS_OK && result == 0, 93);

    /* 2. SOCK_DGRAM message boundaries */
    check(kernel_files_socketpair_create(files, mm, 2, 0, BUFFER, &result) ==
          KERNEL_FILES_STATUS_OK && result == 0, 94);
    check(kernel_copy_from_user(mm, pair, BUFFER, sizeof(pair), &copied) ==
          KERNEL_UACCESS_STATUS_OK && copied == sizeof(pair), 95);
    check(kernel_files_write(files, mm, pair[0], BUFFER + 64, 30, &result) ==
          KERNEL_FILES_STATUS_OK && result == 30, 96);
    check(kernel_files_write(files, mm, pair[0], BUFFER + 64, 40, &result) ==
          KERNEL_FILES_STATUS_OK && result == 40, 97);
    /* Short read truncates datagram */
    check(kernel_files_read(files, mm, pair[1], BUFFER + 256, 10, &result) ==
          KERNEL_FILES_STATUS_OK && result == 10, 98);
    /* Next read gets the second datagram */
    check(kernel_files_read(files, mm, pair[1], BUFFER + 256, 100, &result) ==
          KERNEL_FILES_STATUS_OK && result == 40, 99);
    check(kernel_files_close(files, pair[0], &result) == KERNEL_FILES_STATUS_OK && result == 0, 100);
    check(kernel_files_close(files, pair[1], &result) == KERNEL_FILES_STATUS_OK && result == 0, 101);
}

static void unix_datagram_budget(struct kernel_files *files, struct kernel_mm *mm)
{
    int64_t result;
    int32_t pair[2]; size_t copied;
    check(kernel_files_socketpair_create(files, mm, 2, 2048, BUFFER, &result) == KERNEL_FILES_STATUS_OK && result == 0, 180);
    check(kernel_copy_from_user(mm, pair, BUFFER, sizeof(pair), &copied) == KERNEL_UACCESS_STATUS_OK, 181);
    for (unsigned i = 0; i < 100; i++)
        check(kernel_files_write(files, mm, pair[0], BUFFER, 0, &result) == KERNEL_FILES_STATUS_OK && result == 0, 182);
    check(kernel_files_write(files, mm, pair[0], BUFFER, 65536, &result) == KERNEL_FILES_STATUS_OK && result == -KERNEL_EAGAIN, 183);
    for (unsigned i = 0; i < 100; i++)
        check(kernel_files_read(files, mm, pair[1], BUFFER, 1, &result) == KERNEL_FILES_STATUS_OK && result == 0, 184);
    for (unsigned i = 0; i < 4; i++) {
        check(kernel_files_write(files, mm, pair[0], BUFFER, 65536, &result) == KERNEL_FILES_STATUS_OK && result == 65536, 185);
        check(kernel_files_write(files, mm, pair[0], BUFFER, 1, &result) == KERNEL_FILES_STATUS_OK && result == -KERNEL_EAGAIN, 186);
        check(kernel_files_read(files, mm, pair[1], BUFFER, 1, &result) == KERNEL_FILES_STATUS_OK && result == 1, 187);
    }
    for (unsigned ordinal = 1; ordinal <= 2; ordinal++) {
        fail_packet = ordinal;
        check(kernel_files_write(files, mm, pair[0], BUFFER, 8, &result) == KERNEL_FILES_STATUS_OK && result == -KERNEL_ENOMEM, 192);
        fail_packet = 0;
        check(kernel_files_read(files, mm, pair[1], BUFFER, 8, &result) == KERNEL_FILES_STATUS_OK && result == -KERNEL_EAGAIN, 193);
        check(kernel_files_write(files, mm, pair[0], BUFFER, 8, &result) == KERNEL_FILES_STATUS_OK && result == 8 &&
            kernel_files_read(files, mm, pair[1], BUFFER, 8, &result) == KERNEL_FILES_STATUS_OK && result == 8, 194);
    }
    check(kernel_files_write(files, mm, pair[0], BUFFER, 65537, &result) == KERNEL_FILES_STATUS_OK && result == -KERNEL_EMSGSIZE, 188);
    check(kernel_files_write(files, mm, pair[0], BUFFER + MIB + 4094, 4, &result) == KERNEL_FILES_STATUS_OK && result == -KERNEL_EFAULT, 189);
    check(kernel_files_read(files, mm, pair[1], BUFFER, 8, &result) == KERNEL_FILES_STATUS_OK && result == -KERNEL_EAGAIN, 190);
    check(kernel_files_write(files, mm, pair[0], BUFFER, 65536, &result) == KERNEL_FILES_STATUS_OK && result == 65536 &&
        kernel_files_read(files, mm, pair[1], 0x12345000, 8, &result) == KERNEL_FILES_STATUS_OK && result == -KERNEL_EFAULT, 195);
    check(kernel_files_write(files, mm, pair[0], BUFFER, 65536, &result) == KERNEL_FILES_STATUS_OK && result == 65536, 196);
    check(kernel_files_close(files, pair[1], &result) == KERNEL_FILES_STATUS_OK && !result &&
        kernel_files_write(files, mm, pair[0], BUFFER, 8, &result) == KERNEL_FILES_STATUS_OK && result == -KERNEL_EPIPE &&
        kernel_files_close(files, pair[0], &result) == KERNEL_FILES_STATUS_OK && !result, 191);
}

static void sysv_shm_fragments(struct kernel_mm *mm)
{
    for (unsigned scenario = 0; scenario < 6; scenario++) {
        int32_t id;
        uint64_t base, replacement = 0;
        int64_t result;
        struct kernel_shmid64_ds ds;
        struct kernel_vma vma;
        check(kernel_shm_get(0, KERNEL_IPC_PRIVATE, 3 * BOAROS_PAGE_SIZE,
                            0600, &id) == 0 &&
              kernel_shm_at_mm(mm, 1, id, 0, 0, &base) == 0, 140);
        if (scenario == 0) {
            check(kernel_mm_mprotect(mm, base + BOAROS_PAGE_SIZE,
                      BOAROS_PAGE_SIZE, KERNEL_MM_READ) == KERNEL_MM_STATUS_OK &&
                  kernel_shm_ctl_mm(0, id, KERNEL_IPC_STAT,
                      (uintptr_t)&ds, &result) == 0 && ds.shm_nattch == 3, 141);
            struct kernel_mm child = {0};
            check(kernel_mm_fork(&child, mm) == KERNEL_MM_STATUS_OK &&
                  kernel_shm_ctl_mm(0, id, KERNEL_IPC_STAT,
                      (uintptr_t)&ds, &result) == 0 && ds.shm_nattch == 6, 142);
            check(kernel_mm_release(&child) == KERNEL_MM_STATUS_OK, 143);
        } else if (scenario < 3) {
            if (scenario == 1)
                check(kernel_shm_ctl_mm(0, id, KERNEL_IPC_RMID, 0, &result) == 0, 144);
            check(kernel_mm_munmap(mm, base + (scenario == 1 ? BOAROS_PAGE_SIZE : 0),
                      BOAROS_PAGE_SIZE) == KERNEL_MM_STATUS_OK &&
                  kernel_shm_ctl_mm(0, id, KERNEL_IPC_STAT,
                      (uintptr_t)&ds, &result) == 0 &&
                  ds.shm_nattch == (scenario == 1 ? 2U : 1U), 145);
        } else if (scenario < 5) {
            check(kernel_mm_mmap_anonymous(mm,
                      base + (scenario == 4 ? BOAROS_PAGE_SIZE : 0),
                      (scenario == 4 ? 1U : 3U) * BOAROS_PAGE_SIZE,
                      KERNEL_MM_READ | KERNEL_MM_WRITE, KERNEL_MM_MAP_FIXED,
                      &replacement) == KERNEL_MM_STATUS_OK &&
                  kernel_shm_ctl_mm(0, id, KERNEL_IPC_STAT,
                      (uintptr_t)&ds, &result) == 0 &&
                  ds.shm_nattch == (scenario == 4 ? 2U : 0U), 146);
        } else {
            check(kernel_shm_at_mm(mm, 1, id, base + BOAROS_PAGE_SIZE,
                      KERNEL_SHM_REMAP, &replacement) == 0 &&
                  kernel_shm_ctl_mm(0, id, KERNEL_IPC_STAT,
                      (uintptr_t)&ds, &result) == 0 && ds.shm_nattch == 2, 147);
        }
        check(kernel_shm_dt_mm(mm, base) == (scenario == 3 ? -KERNEL_EINVAL : 0), 148);
        if (scenario == 4 || scenario == 5) {
            check(kernel_mm_vma_lookup(mm, replacement, &vma) == KERNEL_MM_STATUS_OK &&
                  vma.kind == (scenario == 4 ? KERNEL_VMA_KIND_ANONYMOUS :
                                               KERNEL_VMA_KIND_SYSV_SHM), 149);
            if (scenario == 5) check(kernel_shm_dt_mm(mm, replacement) == 0, 150);
        }
        if (scenario != 1) {
            check(kernel_shm_ctl_mm(0, id, KERNEL_IPC_STAT,
                      (uintptr_t)&ds, &result) == 0 && ds.shm_nattch == 0 &&
                  kernel_shm_ctl_mm(0, id, KERNEL_IPC_RMID, 0, &result) == 0, 151);
        }
        check(kernel_shm_ctl_mm(0, id, KERNEL_IPC_STAT,
                  (uintptr_t)&ds, &result) == -KERNEL_EINVAL, 152);
        if (scenario == 3 || scenario == 4)
            check(kernel_mm_munmap(mm, replacement,
                      (scenario == 3 ? 3U : 1U) * BOAROS_PAGE_SIZE) == KERNEL_MM_STATUS_OK, 153);
        for (unsigned page = 0; page < 4; page++)
            check(kernel_mm_vma_lookup(mm, base + page * BOAROS_PAGE_SIZE,
                      &vma) == KERNEL_MM_STATUS_NOT_MAPPED, 154);
    }
}

static void sysv_shm_oom(struct kernel_mm *mm)
{
    for (unsigned remap = 0; remap < 2; remap++) {
        for (unsigned ordinal = 1; ; ordinal++) {
            check(ordinal < 32, 155);
            int32_t id;
            uint64_t base = 0, address = 0;
            int64_t result;
            struct kernel_shmid64_ds ds;
            check(kernel_shm_get(0, KERNEL_IPC_PRIVATE, 3 * BOAROS_PAGE_SIZE,
                                0600, &id) == 0, 156);
            if (remap) check(kernel_mm_mmap_anonymous(mm, 0, 5 * BOAROS_PAGE_SIZE,
                KERNEL_MM_READ | KERNEL_MM_WRITE, 0, &base) == KERNEL_MM_STATUS_OK, 157);
            fail_metadata = ordinal;
            int attached = kernel_shm_at_mm(mm, 1, id,
                remap ? base + BOAROS_PAGE_SIZE : 0, remap ? KERNEL_SHM_REMAP : 0, &address);
            fail_metadata = 0;
            if (attached) {
                check(attached == -KERNEL_ENOMEM && address == 0 &&
                    kernel_shm_ctl_mm(0, id, KERNEL_IPC_STAT, (uintptr_t)&ds, &result) == 0 &&
                    ds.shm_nattch == 0, 158);
                if (remap) {
                    struct kernel_vma vma;
                    check(kernel_mm_vma_lookup(mm, base + BOAROS_PAGE_SIZE, &vma) ==
                        KERNEL_MM_STATUS_OK && vma.kind == KERNEL_VMA_KIND_ANONYMOUS, 159);
                }
                check(kernel_shm_at_mm(mm, 1, id, remap ? base + BOAROS_PAGE_SIZE : 0,
                    remap ? KERNEL_SHM_REMAP : 0, &address) == 0, 160);
            }
            check(kernel_shm_dt_mm(mm, address) == 0 &&
                kernel_shm_ctl_mm(0, id, KERNEL_IPC_RMID, 0, &result) == 0, 161);
            if (remap) check(kernel_mm_munmap(mm, base, 5 * BOAROS_PAGE_SIZE) == KERNEL_MM_STATUS_OK, 162);
            if (!attached) break;
        }
    }
    int32_t id;
    uint64_t address;
    int64_t result;
    struct kernel_shmid64_ds ds;
    check(kernel_shm_get(0, KERNEL_IPC_PRIVATE, 3 * BOAROS_PAGE_SIZE, 0600, &id) == 0 &&
        kernel_shm_at_mm(mm, 1, id, 0, 0, &address) == 0 &&
        kernel_mm_mprotect(mm, address + BOAROS_PAGE_SIZE, BOAROS_PAGE_SIZE,
                          KERNEL_MM_READ) == KERNEL_MM_STATUS_OK, 163);
    for (unsigned pages = 0; pages < 2; pages++) {
        for (unsigned ordinal = 1; ; ordinal++) {
            check(ordinal < 128, 164);
            struct kernel_mm child = {0};
            if (pages) fail_page = ordinal; else fail_metadata = ordinal;
            enum kernel_mm_status status = kernel_mm_fork(&child, mm);
            fail_page = fail_metadata = 0;
            if (status == KERNEL_MM_STATUS_OK)
                check(kernel_mm_release(&child) == KERNEL_MM_STATUS_OK, 165);
            else check(status == KERNEL_MM_STATUS_NO_MEMORY, 166);
            check(kernel_shm_ctl_mm(0, id, KERNEL_IPC_STAT, (uintptr_t)&ds, &result) == 0 &&
                  ds.shm_nattch == 3, 167);
            if (status == KERNEL_MM_STATUS_OK) break;
        }
    }
    check(kernel_shm_dt_mm(mm, address) == 0 &&
          kernel_shm_ctl_mm(0, id, KERNEL_IPC_RMID, 0, &result) == 0, 168);
}

static void sysv_shm_scale(struct kernel_mm *mm)
{
    /* 1. IPC_PRIVATE segment allocation */
    int32_t shmid1 = 0;
    check(kernel_shm_get(0, KERNEL_IPC_PRIVATE, 8192, 0666 | KERNEL_IPC_CREAT, &shmid1) == 0 &&
          shmid1 >= 0, 110);

    /* 临时 registry owner 回滚不能消费段表的原 owner。 */
    uint64_t failed_address = UINT64_C(0x55);
    fail_metadata = 1;
    check(kernel_shm_at_mm(mm, 1, shmid1, 0, 0, &failed_address) ==
              -KERNEL_ENOMEM && fail_metadata == 0 &&
          failed_address == UINT64_C(0x55), 132);
    fail_metadata = 0;

    /* 2. Attach segment to mm */
    uint64_t addr1 = 0;
    check(kernel_shm_at_mm(mm, 1, shmid1, 0, 0, &addr1) == 0 && addr1 != 0, 111);

    /* 3. Write data to addr1 and read back */
    unsigned char pattern[32];
    for (int i = 0; i < 32; i++) pattern[i] = (unsigned char)(0xa0 + i);
    size_t copied = 0;
    check(kernel_copy_to_user(mm, addr1, pattern, sizeof(pattern), &copied) == KERNEL_UACCESS_STATUS_OK &&
          copied == sizeof(pattern), 112);
    unsigned char readback[32];
    check(kernel_copy_from_user(mm, readback, addr1, sizeof(readback), &copied) == KERNEL_UACCESS_STATUS_OK &&
          copied == sizeof(readback), 113);
    for (int i = 0; i < 32; i++) check(readback[i] == pattern[i], 114);

    /* 4. Attach second time at different address */
    uint64_t addr2 = 0;
    check(kernel_shm_at_mm(mm, 1, shmid1, 0, 0, &addr2) == 0 && addr2 != 0 && addr2 != addr1, 115);
    unsigned char readback2[32];
    check(kernel_copy_from_user(mm, readback2, addr2, sizeof(readback2), &copied) == KERNEL_UACCESS_STATUS_OK, 116);
    for (int i = 0; i < 32; i++) check(readback2[i] == pattern[i], 117);

    /* 5. IPC_STAT */
    int64_t res = 0;
    struct kernel_shmid64_ds ds = {0};
    check(kernel_shm_ctl_mm(0, shmid1, KERNEL_IPC_STAT, (uint64_t)(uintptr_t)&ds, &res) == 0 && res == 0, 118);
    check(ds.shm_nattch == 2 && ds.shm_segsz == 8192, 119);

    /* 6. IPC_RMID (marked for deletion, active attachments remain usable) */
    check(kernel_shm_ctl_mm(0, shmid1, KERNEL_IPC_RMID, 0, &res) == 0 && res == 0, 120);

    /* Verify still accessible via existing mappings */
    check(kernel_copy_from_user(mm, readback, addr1, sizeof(readback), &copied) == KERNEL_UACCESS_STATUS_OK &&
          readback[0] == 0xa0, 121);

    /* 7. Detach first mapping */
    check(kernel_shm_dt_mm(mm, addr1) == 0, 122);
    check(kernel_shm_ctl_mm(0, shmid1, KERNEL_IPC_STAT, (uint64_t)(uintptr_t)&ds, &res) == 0, 123);
    check(ds.shm_nattch == 1, 124);

    /* 8. Detach second mapping: nattch reaches 0, segment destroyed */
    check(kernel_shm_dt_mm(mm, addr2) == 0, 125);
    check(kernel_shm_ctl_mm(0, shmid1, KERNEL_IPC_STAT, (uint64_t)(uintptr_t)&ds, &res) == -KERNEL_EINVAL, 126);

    /* 9. Test named key conflict and creation */
    int32_t key = 0x5678;
    int32_t shmid_named = 0;
    check(kernel_shm_get(0, key, 4096, 0666 | KERNEL_IPC_CREAT, &shmid_named) == 0, 127);
    int32_t shmid_dup = 0;
    check(kernel_shm_get(0, key, 4096, 0666 | KERNEL_IPC_CREAT | KERNEL_IPC_EXCL, &shmid_dup) == -KERNEL_EEXIST, 128);
    check(kernel_shm_get(0, key, 4096, 0, &shmid_dup) == 0 && shmid_dup == shmid_named, 129);
    check(kernel_shm_ctl_mm(0, shmid_named, KERNEL_IPC_RMID, 0, &res) == 0, 130);
    check(kernel_shm_get(0, key, 4096, 0, &shmid_dup) == -KERNEL_ENOENT, 131);
    sysv_shm_fragments(mm);
    sysv_shm_oom(mm);
}

static struct riscv_mm_statistics mapped_cost(struct kernel_files *files,
    struct kernel_mm *mm, int64_t fd, uint64_t bytes)
{
    int64_t result;
    struct kernel_open_file_description *pin = 0;
    uint64_t address;
    check(kernel_files_ftruncate(files, fd, bytes, &result) == KERNEL_FILES_STATUS_OK && result == 0 &&
          kernel_files_pin(files, fd, &pin, &result) == KERNEL_FILES_STATUS_OK && result == 0 &&
          kernel_mm_mmap_file_private(mm, &pin, 0x20000000, bytes, 0,
              KERNEL_MM_READ | KERNEL_MM_WRITE, KERNEL_MM_MAP_FIXED_NOREPLACE | KERNEL_MM_MAP_SHARED,
              &address) == KERNEL_MM_STATUS_OK && pin == 0, 20);
    uint64_t pages = bytes / 4096;
    for (uint64_t i = 0; i < pages; i++) {
        if (i < 128) {
            for (unsigned failure = 1; failure <= 2; failure++) {
                struct kernel_mm_mapping mapping;
                fail_metadata = failure;
                enum kernel_mm_status status = kernel_mm_resolve_user_fault(mm, address + i * 4096, KERNEL_MM_READ);
                fail_metadata = 0;
                if (status == KERNEL_MM_STATUS_OK) break;
                check(status == KERNEL_MM_STATUS_NO_MEMORY &&
                      kernel_mm_lookup(mm, address + i * 4096, &mapping) == KERNEL_MM_STATUS_NOT_MAPPED, 27);
            }
        }
        struct kernel_mm_mapping mapping;
        if (kernel_mm_lookup(mm, address + i * 4096, &mapping) == KERNEL_MM_STATUS_NOT_MAPPED)
            check(kernel_mm_resolve_user_fault(mm, address + i * 4096, KERNEL_MM_READ) == KERNEL_MM_STATUS_OK, 21);
    }
    struct riscv_mm_statistics before, after;
    if (bytes == 16 * MIB) {
        struct kernel_mm clone = {0};
        check(kernel_mm_fork(&clone, mm) == KERNEL_MM_STATUS_OK, 43);
        riscv_kernel_mm_get_statistics(&clone, &after);
        check(after.resident_probes == 0 && after.protect_visits == 0 &&
              after.protect_address_flushes == 0 && after.protect_global_flushes == 0, 44);
        check(kernel_mm_release(&clone) == KERNEL_MM_STATUS_OK, 45);
    }
#if BOAROS_COST_DIAGNOSTICS
    struct riscv_mm_statistics protect_before, protect_after;
    riscv_kernel_mm_get_statistics(mm, &protect_before);
    check(kernel_cost_begin(2, cost_frequency, 1, 0) == 0, 195);
    check(kernel_mm_mprotect(mm, address, 4096, KERNEL_MM_READ) == KERNEL_MM_STATUS_OK &&
          kernel_mm_mprotect(mm, address, 4096, KERNEL_MM_READ | KERNEL_MM_WRITE) == KERNEL_MM_STATUS_OK, 196);
    check(kernel_cost_end(2, 0) == 0, 197);
    uint64_t observed;
    riscv_kernel_mm_get_statistics(mm, &protect_after);
    check(kernel_cost_read(0, COST_MPROTECT_RESIDENT_VISITS, &observed) == 0 && observed == pages * 2, 198);
    check(kernel_cost_read(0, COST_MPROTECT_PTE_VISITS, &observed) == 0 && observed == protect_after.protect_visits - protect_before.protect_visits, 199);
    check(kernel_cost_read(0, COST_MPROTECT_ADDRESS_TLB, &observed) == 0 && observed == protect_after.protect_address_flushes - protect_before.protect_address_flushes, 200);
#endif
    riscv_kernel_mm_get_statistics(mm, &before);
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t index = (i * 4093) & (pages - 1);
        unsigned char value = (unsigned char)i;
        size_t copied;
        check(kernel_copy_to_user(mm, address + index * 4096, &value, 1, &copied) ==
              KERNEL_UACCESS_STATUS_OK && copied == 1, 22);
    }
    riscv_kernel_mm_get_statistics(mm, &after);
    after.resident_probes -= before.resident_probes;
    after.protect_visits -= before.protect_visits;
    after.protect_address_flushes -= before.protect_address_flushes;
    after.protect_global_flushes -= before.protect_global_flushes;
    number("mapped bytes: ", bytes);
    number("resident probes: ", after.resident_probes);
    number("protect visits: ", after.protect_visits);
    number("address flushes: ", after.protect_address_flushes);
    number("global flushes: ", after.protect_global_flushes);
    for (uint64_t i = 0; i < pages; i++) {
        unsigned char value; size_t copied;
        uint64_t index = (i * 4093) & (pages - 1);
        check(kernel_copy_from_user(mm, &value, address + index * 4096, 1, &copied) ==
              KERNEL_UACCESS_STATUS_OK && value == (unsigned char)i, 23);
    }
    check(kernel_mm_munmap(mm, address, bytes) == KERNEL_MM_STATUS_OK &&
          kernel_files_ftruncate(files, fd, 0, &result) == KERNEL_FILES_STATUS_OK && result == 0, 24);
    return after;
}

#if BOAROS_COST_DIAGNOSTICS
static unsigned char growth_payload[65536];
static void growth_cost(struct kernel_vfs_mount *mount, struct kernel_page_cache *cache)
{
    struct kernel_vfs_file file = {0};
    check(kernel_vfs_create(mount, "/growth", 0600, &file) == 0, 210);
    for (unsigned i = 0; i < sizeof(growth_payload); i++) growth_payload[i] = (unsigned char)(i * 19 + 7);
    const unsigned chunks[] = {4096, 1024, 65536};
    for (uint64_t size = MIB; size <= 64 * MIB; size *= 4) {
        for (unsigned c = 0; c < 3; c++) {
            check(kernel_vfs_ftruncate(&file, 0) == 0, 211);
            check(kernel_cost_begin(3, cost_frequency, 1, 0) == 0, 212);
            for (uint64_t offset = 0; offset < size; offset += chunks[c]) {
                size_t written = 0;
                check(kernel_vfs_pwrite(&file, offset, growth_payload, chunks[c], &written) == 0 && written == chunks[c], 213);
            }
            check(kernel_cost_end(3, 0) == 0, 214);
            uint64_t visits, tails;
            check(kernel_cost_read(0, COST_RESIZE_VISITS, &visits) == 0 &&
                  kernel_cost_read(0, COST_RESIZE_TAIL_PAGES, &tails) == 0, 215);
            number("growth bytes: ", size); number("growth chunk: ", chunks[c]);
            number("growth visits: ", visits); number("growth tail pages: ", tails);
            uint64_t needed = chunks[c] == 1024 ? size / 4096 * 3 : 0;
            check(visits <= needed && tails == needed, 216);
            struct kernel_page_cache_statistics stats;
            kernel_page_cache_get_statistics(cache, &stats);
            check(stats.current_pages >= size / 4096, 217);
            for (uint64_t offset = 0; offset < size; offset += 4096) {
                size_t read = 0;
                check(kernel_vfs_pread(&file, offset, payload, sizeof(payload), &read) == 0 && read == sizeof(payload) &&
                      !memcmp(payload, growth_payload, sizeof(payload)), 218);
            }
        }
    }
    check(kernel_vfs_ftruncate(&file, 0) == 0 && kernel_vfs_close(&file) == 0 &&
          kernel_vfs_unlink(mount, "/growth") == 0, 219);
}
#endif

void kernel_main(unsigned long hart, const void *dtb)
{
    (void)hart;
    struct dtb_boot_info info;
    struct boot_memory_layout layout = {0};
    struct physical_page_allocator allocator;
    struct kernel_heap heap;
    struct riscv_sv39_page_table table = {0};
    struct riscv_sv39_user_space space = {0};
    struct kernel_mm mm = {0};
    struct kernel_page_cache cache = {0};
    struct riscv_virtio_mmio_block device = {0};
    struct kernel_vfs_mount mount = {0};
    struct kernel_files files = {0};
    struct kernel_fs_context fs = {0};
    check(dtb_read_boot_info(dtb, &info) == DTB_STATUS_OK, 1);
    check(kernel_time_init(info.timebase_frequency,0)==KERNEL_TIME_STATUS_OK,208);
    layout.usable_count = 1;
    layout.usable[0].base = (uintptr_t)pool; layout.usable[0].size = sizeof(pool);
    check(physical_page_allocator_init(&allocator, &layout) == PHYSICAL_PAGE_STATUS_OK &&
          physical_page_allocator_bind_access(&allocator, access_page) == PHYSICAL_PAGE_STATUS_OK &&
          physical_page_allocator_finalize(&allocator) == PHYSICAL_PAGE_STATUS_OK &&
          kernel_heap_init(&heap, &allocator, physical) == KERNEL_HEAP_STATUS_OK &&
          riscv_sv39_page_table_init(&table, &allocator) == RISCV_SV39_STATUS_OK, 2);
    /* Bare boot tests normally use physical aliases. lwIP's static tables
     * also contain linked high-half pointers, so map both aliases here. */
    check(riscv_sv39_map_range(&table, 0x80000000, 0x80000000, 256 * MIB,
              RISCV_SV39_READ | RISCV_SV39_WRITE | RISCV_SV39_EXECUTE) == RISCV_SV39_STATUS_OK &&
          riscv_sv39_map_range(&table, UINT64_C(0xffffffff80000000), 0x80200000, 256 * MIB,
              RISCV_SV39_READ | RISCV_SV39_WRITE | RISCV_SV39_EXECUTE) == RISCV_SV39_STATUS_OK &&
          riscv_sv39_map_range(&table, 0x10000000, 0x10000000, 0x200000,
              RISCV_SV39_READ | RISCV_SV39_WRITE) == RISCV_SV39_STATUS_OK &&
          riscv_sv39_activate(&table) == RISCV_SV39_STATUS_OK &&
          kernel_shm_init(&heap, &allocator) == KERNEL_SHM_STATUS_OK, 35);
    uint64_t baseline = physical_page_available(&allocator);
    check(kernel_page_cache_init(&cache, &heap, &allocator) == KERNEL_PAGE_CACHE_STATUS_OK, 3);
    int found = 0;
    for (unsigned i = 0; i < info.virtio_mmio_count; i++) {
        if (riscv_virtio_mmio_block_init(&device, (void *)(uintptr_t)info.virtio_mmio[i].base,
                info.virtio_mmio[i].size, &allocator, dma, info.timebase_frequency) ==
                RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK) { found = 1; break; }
    }
    check(found && kernel_vfs_mount_root(&mount, &device.block, &heap, &cache) == 0 &&
          riscv_sv39_user_space_init(&space, &allocator, &table) == RISCV_SV39_STATUS_OK &&
          riscv_kernel_mm_create(&mm, &space) == KERNEL_MM_STATUS_OK &&
          kernel_mm_vma_enable(&mm, &heap) == KERNEL_MM_STATUS_OK &&
          kernel_mm_brk_initialize(&mm, 0x10000000, 0x70000000) == KERNEL_MM_STATUS_OK &&
          kernel_fs_context_create(&fs, &mount, &heap) == KERNEL_FS_CONTEXT_STATUS_OK &&
          kernel_files_create(&files, &heap) == KERNEL_FILES_STATUS_OK &&
          riscv_kernel_mm_satp(&mm, &active_satp) == KERNEL_MM_STATUS_OK, 4);
    uint64_t address;
    check(kernel_mm_mmap_anonymous(&mm, USER, 4096, KERNEL_MM_READ | KERNEL_MM_WRITE,
              KERNEL_MM_MAP_FIXED_NOREPLACE, &address) == KERNEL_MM_STATUS_OK &&
          kernel_mm_mmap_anonymous(&mm, BUFFER, MIB + 4096, KERNEL_MM_READ | KERNEL_MM_WRITE,
              KERNEL_MM_MAP_FIXED_NOREPLACE, &address) == KERNEL_MM_STATUS_OK, 5);
    size_t copied;
    check(kernel_copy_to_user(&mm, USER, "/scale", 7, &copied) == KERNEL_UACCESS_STATUS_OK, 6);
    int64_t fd, result;
    check(kernel_files_openat(&files, &fs, &mm, -100, USER, 2 | 0100, 0600, &fd) ==
          KERNEL_FILES_STATUS_OK && fd >= 0, 7);
    for (unsigned i = 0; i < sizeof(payload); i++) payload[i] = (unsigned char)(i * 17 + 3);
    for (uint64_t at = 0; at < MIB + 4096; at += 4096)
        check(kernel_copy_to_user(&mm, BUFFER + at, payload, sizeof(payload), &copied) ==
              KERNEL_UACCESS_STATUS_OK && copied == sizeof(payload), 8);
    struct kernel_files_statistics before, after;
    kernel_files_get_statistics(&files, &before);
    uint64_t resolutions = kernel_uaccess_page_resolutions();
#if BOAROS_COST_DIAGNOSTICS
    struct riscv_virtio_mmio_block_statistics device_before, device_after;
    riscv_virtio_mmio_block_get_statistics(&device, &device_before);
    cost_frequency = info.timebase_frequency;
    check(kernel_cost_begin(1, info.timebase_frequency, 1, 0) == 0, 190);
    cost_copy_measuring = 1;
#endif
    check(kernel_files_write(&files, &mm, fd, BUFFER, MIB, &result) == KERNEL_FILES_STATUS_OK &&
          result == MIB, 9);
    resolutions = kernel_uaccess_page_resolutions() - resolutions;
#if BOAROS_COST_DIAGNOSTICS
    cost_copy_measuring = 0;
    check(kernel_cost_end(1, 0) == 0, 191);
    uint64_t metric;
    check(kernel_cost_read(0, COST_COPY_FROM_USER, &metric) == 0 && metric == cost_copy_observed && metric == MIB, 192);
    check(kernel_cost_read(0, COST_USER_RESOLUTIONS, &metric) == 0 && metric == resolutions, 193);
    riscv_virtio_mmio_block_get_statistics(&device, &device_after);
    check(kernel_cost_read(0, COST_DEVICE_OTHER_REQUESTS, &metric) == 0 && metric == device_after.requests - device_before.requests, 194);
    number("cost wrapper copy bytes: ", cost_copy_observed);
#endif
    kernel_files_get_statistics(&files, &after);
    number("file write chunks: ", after.write_chunks - before.write_chunks);
    number("file user-page resolutions: ", resolutions);
    check(after.write_chunks - before.write_chunks <= 512 && resolutions <= 512, 10);
    for (uint64_t at = 0; at < MIB; at += 4096) {
        check(kernel_files_pread(&files, &mm, fd, BUFFER, 4096, at, &result) == KERNEL_FILES_STATUS_OK && result == 4096, 11);
        check(kernel_copy_from_user(&mm, payload, BUFFER, sizeof(payload), &copied) == KERNEL_UACCESS_STATUS_OK, 12);
        for (unsigned i = 0; i < sizeof(payload); i++) check(payload[i] == (unsigned char)(i * 17 + 3), 13);
    }
    fail_page = 1;
    check(kernel_files_pwrite(&files, &mm, fd, BUFFER, 4096, 0, &result) == KERNEL_FILES_STATUS_OK &&
          result == -KERNEL_ENOMEM && fail_page == 0, 14);
#if BOAROS_COST_DIAGNOSTICS
    growth_cost(&mount, &cache);
#endif
    tcp_cost(&files, &mm);
    udp_buffer_oom(&files, &mm);
    socketpair_scale(&files, &mm);
    unix_datagram_budget(&files, &mm);
    struct riscv_mm_statistics small = mapped_cost(&files, &mm, fd, 16 * MIB);
    struct riscv_mm_statistics large = mapped_cost(&files, &mm, fd, 64 * MIB);
    sysv_shm_scale(&mm);
    check(large.resident_probes <= 6 * small.resident_probes, 25);
    check(small.protect_visits <= 6 * (16 * MIB / 4096) &&
          large.protect_visits <= 6 * (64 * MIB / 4096) &&
          small.protect_global_flushes == 0 && large.protect_global_flushes == 0 &&
          small.protect_address_flushes == 16 * MIB / 4096 &&
          large.protect_address_flushes == 64 * MIB / 4096, 26);
    check(kernel_files_close(&files, fd, &result) == KERNEL_FILES_STATUS_OK && result == 0 &&
          kernel_files_release(&files) == KERNEL_FILES_STATUS_OK &&
          kernel_mm_release(&mm) == KERNEL_MM_STATUS_OK &&
          kernel_fs_context_release(&fs) == KERNEL_FS_CONTEXT_STATUS_OK &&
          kernel_vfs_unmount(&mount) == 0 &&
          kernel_page_cache_destroy(&cache) == KERNEL_PAGE_CACHE_STATUS_OK &&
          riscv_virtio_mmio_block_destroy(&device) == RISCV_VIRTIO_MMIO_BLOCK_STATUS_OK, 15);
    check(physical_page_available(&allocator) == baseline, 16);
    virt_uart_puts("BoarOS: scale tests passed\n");
    sbi_shutdown();
}
