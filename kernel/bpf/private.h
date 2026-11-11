#ifndef BOAROS_BPF_PRIVATE_H
#define BOAROS_BPF_PRIVATE_H
#include <kernel/bpf.h>
#include <kernel/bpf_map.h>
#include <kernel/bpf_vm.h>
#include <kernel/heap.h>
#include <kernel/sync.h>
#include <kernel/raw_lock.h>
#include <kernel/wait_internal.h>
#include "abi.h"

enum bpf_object_kind { BPF_OBJECT_PROGRAM=0, BPF_OBJECT_MAP,
    BPF_OBJECT_BTF, BPF_OBJECT_LINK, BPF_OBJECT_PERF, BPF_OBJECT_KINDS };
struct bpf_object {
    struct bpf_object *left,*right;
    struct kernel_heap *heap;
    uint32_t references,id,uid;
    uint8_t height,kind,published;
    char name[16];
};
struct bpf_btf {
    struct bpf_object object;
    unsigned char *data;
    uint32_t size,type_count;
    uint32_t *offsets;
    uint8_t kernel;
};
struct bpf_map_object {
    struct bpf_object object;
    struct bpf_map *storage;
    struct bpf_btf *btf;
    uint32_t key_type,value_type,vmlinux_type;
    uint64_t writable_bytes;
    struct kernel_mutex mapping_lock;
};
struct bpf_program_binding {
    uint32_t pc;
    struct bpf_map_object *map;
    void *value;
};
struct kernel_bpf_program {
    struct bpf_object object;
    struct kernel_bpf_insn *instructions;
    struct kernel_bpf_classic_insn *classic;
    struct bpf_program_binding *bindings;
    struct bpf_map_object **maps;
    struct bpf_btf *btf;
    struct kernel_bpf_vm_env environment;
    uint64_t loaded_at,run_time,run_count,recursion_misses;
    uint32_t type,count,classic_count,binding_count;
    uint32_t map_count,max_context_size;
    uint8_t gpl,prepared;
    unsigned char tag[8];
    char *verification_log;
    uint32_t log_capacity,log_length;
    uint64_t log_total;
    void *execution;
    size_t execution_stride;
    uint32_t execution_cpus,arena_size;
    void *function_info,*line_info;
    uint32_t function_count,function_size,line_count,line_size;
};
struct bpf_perf {
    struct bpf_object object;
    struct bpf_perf *next,*previous;
    struct kernel_bpf_program *program;
    struct kernel_task *target_task;
    struct bpf_link *link; /* weak; protected by trace lock */
    uint64_t cookie,count,read_format;
    int32_t pid,cpu;
    uint32_t event;
    uint8_t enabled,registered,file_closed;
};
struct bpf_link {
    struct bpf_object object;
    struct bpf_perf *event;
    struct kernel_bpf_program *program;
    uint64_t cookie;
    uint32_t type;
    uint8_t detached;
};
void bpf_object_initialize(struct bpf_object *, struct kernel_heap *, unsigned kind);
int bpf_object_publish(struct bpf_object *);
void bpf_object_get(struct bpf_object *);
void bpf_object_put(struct bpf_object *);
struct bpf_object *bpf_object_by_id(unsigned kind,uint32_t);
int bpf_object_next_id(unsigned kind,uint32_t,uint32_t *);
struct bpf_object *bpf_object_from_file(const struct kernel_open_file_description *);
struct bpf_object *bpf_object_fd(struct kernel_files *,int32_t,unsigned kind,int *error);
enum {BPF_FD_READ=1,BPF_FD_WRITE=2};
struct bpf_map_object *bpf_map_fd(struct kernel_files *,int32_t,unsigned access,int *error);
int bpf_object_install(struct kernel_files *,struct bpf_object *,uint32_t flags,uint32_t fd_flags);
int bpf_object_file(struct kernel_heap *,struct bpf_object *,uint32_t flags,
    struct kernel_open_file_description **);
int bpf_user_copy(struct kernel_mm *,uint64_t,void *,size_t,int from_user);
int bpf_btf_kernel_initialize(struct kernel_heap *,const void *,uint32_t);
struct bpf_btf *bpf_btf_kernel(void);
void bpf_btf_kernel_retire(void);
int bpf_btf_parse(struct kernel_heap *,unsigned char *,uint32_t,struct bpf_btf **);
int bpf_btf_type_size(const struct bpf_btf *,uint32_t,uint32_t *);
int bpf_btf_record(const struct bpf_btf *,uint32_t,const uint32_t **);
int bpf_runtime_prepare(struct kernel_bpf_program *);
void bpf_runtime_release(struct kernel_bpf_program *);
uint64_t bpf_runtime_run(struct kernel_bpf_program *,struct kernel_task *,void *,
    uint32_t context_size,const struct kernel_bpf_packet *,uint64_t cookie);
/* The supported __sk_buff metadata prefix, owned by one packet invocation. */
struct bpf_skb_context {
    uint32_t length,packet_type,mark,queue,protocol,vlan_present,vlan_tci,vlan_protocol;
    uint32_t priority,ingress_ifindex,ifindex,tc_index,cb[5],hash;
};
void bpf_runtime_packet_context(struct bpf_skb_context *,const struct kernel_bpf_packet *);
void bpf_runtime_environment(struct kernel_bpf_program *);
int64_t bpf_program_load(struct kernel_task *,struct kernel_files *,struct kernel_mm *,
    uint64_t,uint32_t,union bpf_attribute *);
int64_t bpf_program_test(struct kernel_task *,struct kernel_files *,struct kernel_mm *,
    uint64_t,uint32_t,union bpf_attribute *);
int bpf_program_trace_context(struct kernel_bpf_program *,uint32_t event);
void bpf_program_calculate_tag(struct kernel_bpf_program *);
int bpf_object_information(struct kernel_mm *,struct bpf_object *,uint64_t,uint32_t *);
int bpf_link_detach(struct bpf_link *);
void bpf_perf_destroy(struct bpf_perf *);
void bpf_perf_close_file(struct bpf_perf *);
int bpf_perf_ioctl(struct bpf_perf *,struct kernel_task *,struct kernel_files *,
    uint64_t command,uint64_t argument);
int64_t bpf_link_create(struct kernel_files *,union bpf_attribute *);
int bpf_perf_read(struct bpf_perf *,void *,size_t,size_t *);
int bpf_pin_object(const struct kernel_fs_context *,const char *,struct bpf_object *);
int bpf_get_pinned(const struct kernel_fs_context *,const char *,uint32_t,
    struct bpf_object **);
#endif
