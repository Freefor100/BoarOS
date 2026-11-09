#ifndef BOAROS_BPF_ABI_H
#define BOAROS_BPF_ABI_H
#include <stdint.h>
#include <stddef.h>

/* Native 64-bit Linux UAPI. The fixed source is references/linux/include/uapi/linux/bpf.h. */
enum bpf_command {
    BPF_MAP_CREATE=0, BPF_MAP_LOOKUP_ELEM, BPF_MAP_UPDATE_ELEM,
    BPF_MAP_DELETE_ELEM, BPF_MAP_GET_NEXT_KEY, BPF_PROG_LOAD, BPF_OBJ_PIN,
    BPF_OBJ_GET, BPF_PROG_ATTACH, BPF_PROG_DETACH, BPF_PROG_TEST_RUN,
    BPF_PROG_GET_NEXT_ID, BPF_MAP_GET_NEXT_ID, BPF_PROG_GET_FD_BY_ID,
    BPF_MAP_GET_FD_BY_ID, BPF_OBJ_GET_INFO_BY_FD, BPF_PROG_QUERY,
    BPF_RAW_TRACEPOINT_OPEN, BPF_BTF_LOAD, BPF_BTF_GET_FD_BY_ID,
    BPF_TASK_FD_QUERY, BPF_MAP_LOOKUP_AND_DELETE_ELEM, BPF_MAP_FREEZE,
    BPF_BTF_GET_NEXT_ID, BPF_MAP_LOOKUP_BATCH, BPF_MAP_LOOKUP_AND_DELETE_BATCH,
    BPF_MAP_UPDATE_BATCH, BPF_MAP_DELETE_BATCH, BPF_LINK_CREATE, BPF_LINK_UPDATE,
    BPF_LINK_GET_FD_BY_ID, BPF_LINK_GET_NEXT_ID, BPF_ENABLE_STATS,
    BPF_ITER_CREATE, BPF_LINK_DETACH, BPF_PROG_BIND_MAP,
};
enum { BPF_SOCKET_FILTER=1, BPF_KPROBE=2, BPF_TRACEPOINT=5, BPF_RAW_TRACEPOINT=17 };
union bpf_attribute {
    struct {
        uint32_t type,key_size,value_size,max_entries,flags,inner_fd,numa;
        char name[16];
        uint32_t ifindex,btf_fd,key_type,value_type,vmlinux_type;
        uint64_t extra;
        int32_t value_btf_fd,token_fd;
        uint64_t exclusive_hash;
        uint32_t exclusive_hash_size;
    } map;
    struct { uint32_t fd,pad; uint64_t key,value,flags; } element;
    struct {
        uint64_t in_batch,out_batch,keys,values;
        uint32_t count,map_fd;
        uint64_t element_flags,flags;
    } batch;
    struct {
        uint32_t type,count;
        uint64_t insns,license;
        uint32_t log_level,log_size;
        uint64_t log;
        uint32_t version,flags;
        char name[16];
        uint32_t ifindex,attach_type,btf_fd,func_record_size;
        uint64_t funcs;
        uint32_t func_count,line_record_size;
        uint64_t lines;
        uint32_t line_count,attach_btf_id,attach_object_fd,core_count;
        uint64_t fd_array,core_relocations;
        uint32_t core_record_size,log_true_size;
        int32_t token_fd;
        uint32_t fd_array_count;
        uint64_t signature;
        uint32_t signature_size;
        int32_t keyring;
    } program;
    struct { uint64_t path; uint32_t fd,flags; int32_t path_fd; } pin;
    struct {
        uint32_t fd,result,data_in_size,data_out_size;
        uint64_t data_in,data_out;
        uint32_t repeat,duration,context_in_size,context_out_size;
        uint64_t context_in,context_out;
        uint32_t flags,cpu,batch_size;
    } test;
    struct { uint32_t id,next_id,flags; int32_t token_fd; } id;
    struct { uint32_t fd,length; uint64_t data; } info;
    struct { uint64_t name; uint32_t fd,pad; uint64_t cookie; } raw;
    struct {
        uint64_t data,log;
        uint32_t size,log_size,log_level,log_true_size,flags;
        int32_t token_fd;
    } btf;
    struct {
        uint32_t program_fd,target_fd,attach_type,flags;
        uint64_t cookie;
        uint64_t extra[6];
    } link;
    struct { uint32_t fd; } detach;
    struct { uint32_t prog_fd,map_fd,flags; } bind;
    uint64_t padding[32];
};
struct bpf_map_information {
    uint32_t type,id,key_size,value_size,max_entries,flags;
    char name[16];
    uint32_t ifindex,btf_vmlinux_value_type_id;
    uint64_t netns_dev,netns_ino;
    uint32_t btf_id,btf_key_type_id,btf_value_type_id,pad;
    uint64_t extra;
};
struct bpf_program_information {
    uint32_t type,id;
    unsigned char tag[8];
    uint32_t jited_length,translated_length;
    uint64_t jited,translated,load_time;
    uint32_t uid,map_count;
    uint64_t map_ids;
    char name[16];
    uint32_t ifindex,flags;
    uint64_t netns_dev,netns_ino;
    uint32_t jited_symbol_count,jited_function_count;
    uint64_t jited_symbols,jited_functions;
    uint32_t btf_id,func_record_size;
    uint64_t funcs;
    uint32_t func_count,line_count;
    uint64_t lines,jited_lines;
    uint32_t jited_line_count,line_record_size,jited_line_record_size,tag_count;
    uint64_t tags,run_time,run_count,recursion_misses;
    uint32_t verified_instructions,attach_btf_object_id,attach_btf_id,pad;
};
struct bpf_btf_information {
    uint64_t data;
    uint32_t size,id;
    uint64_t name;
    uint32_t name_length,kernel;
};
struct bpf_link_information {
    uint32_t type,id,program_id,pad;
    uint64_t payload[6];
};
#define BPF_END(type,field) (offsetof(type,field)+sizeof(((type *)0)->field))
_Static_assert(offsetof(union bpf_attribute,program.funcs)==80,"BPF func_info offset");
_Static_assert(offsetof(union bpf_attribute,map.extra)==64,"BPF map_extra offset");
_Static_assert(sizeof(struct bpf_map_information)==88,"BPF map_info layout");
#endif
