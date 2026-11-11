#ifndef BOAROS_KERNEL_BPF_H
#define BOAROS_KERNEL_BPF_H
#include <stddef.h>
#include <stdint.h>

struct kernel_task;
struct kernel_heap;
struct kernel_mm;
struct kernel_lock_guard;
struct kernel_files;
struct kernel_open_file_description;
struct kernel_bpf_program;
struct kernel_bpf_classic_insn;
struct kernel_vfs_mount;
struct kernel_vfs_path;

int64_t kernel_bpf_syscall(struct kernel_task *, uint32_t command,
    uint64_t attribute, uint32_t size);
int64_t kernel_bpf_perf_open(struct kernel_task *, uint64_t attribute,
    int32_t pid, int32_t cpu, int32_t group, uint64_t flags);
int kernel_bpf_unprivileged_get(void);
int kernel_bpf_unprivileged_set(unsigned);

int kernel_bpf_program_fd(struct kernel_files *, int32_t fd,
    uint32_t type, struct kernel_bpf_program **);
int kernel_bpf_classic_create(struct kernel_heap *,
    const struct kernel_bpf_classic_insn *, uint32_t count,
    struct kernel_bpf_program **);
void kernel_bpf_program_get(struct kernel_bpf_program *);
void kernel_bpf_program_put(struct kernel_bpf_program *);
uint32_t kernel_bpf_program_classic(const struct kernel_bpf_program *,
    const struct kernel_bpf_classic_insn **);

/* The protocol owns the packet and its header positions for the entire run.
 * Reads borrow immutable bytes; returning an error is a packet bounds failure. */
struct kernel_bpf_packet {
    void *object;
    uint32_t length, protocol, ifindex, ingress_ifindex, packet_type, mark, queue, hash;
    uint32_t hardware_type,payload_offset,vlan_tci,vlan_present,vlan_protocol;
    int32_t network_offset, link_offset;
    int (*read)(void *, int64_t offset, void *, uint32_t length);
};
uint32_t kernel_bpf_filter(struct kernel_bpf_program *,
    const struct kernel_bpf_packet *);

void kernel_bpf_trace_enter(struct kernel_task *, uint64_t nr,
    const uint64_t arguments[6]);
void kernel_bpf_trace_exit(struct kernel_task *, uint64_t nr, int64_t result);

/* The MM retains the OFD as its mapping source, independently of the FD. */
/* On success guard holds freeze serialization until VMA publication. */
int kernel_bpf_mmap_begin(struct kernel_open_file_description *, uint64_t,
    uint64_t length, int writable, struct kernel_lock_guard *, uint64_t **write_bytes);
int kernel_bpf_mmap_page(struct kernel_open_file_description *, uint64_t,
    int writable, uint64_t *physical);
int kernel_bpf_fdinfo(const struct kernel_open_file_description *, char *, size_t,
    size_t *);
const char *kernel_bpf_fd_name(const struct kernel_open_file_description *);

int kernel_bpf_initialize(struct kernel_heap *);
int kernel_bpf_shutdown(struct kernel_heap *);
const void *kernel_bpf_kernel_btf(uint32_t *size);
const char *kernel_bpf_filesystem_type(const struct kernel_vfs_mount *);
int kernel_bpffs_mount(struct kernel_heap *, uint64_t flags,
    struct kernel_vfs_mount **);
int kernel_tracefs_mount(struct kernel_heap *, uint64_t flags,
    struct kernel_vfs_mount **);
int kernel_sysfs_mount(struct kernel_heap *, uint64_t flags,
    struct kernel_vfs_mount **);
#endif
