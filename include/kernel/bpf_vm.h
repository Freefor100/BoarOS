#ifndef BOAROS_KERNEL_BPF_VM_H
#define BOAROS_KERNEL_BPF_VM_H
#include <stddef.h>
#include <stdint.h>

#define KERNEL_BPF_STACK_SIZE 512u
#define KERNEL_BPF_MAX_FRAMES 16u
#define KERNEL_BPF_COMPLEXITY_LIMIT 1000000u
#define KERNEL_BPF_CLASSIC_MAX_INSNS 4096u
#define KERNEL_BPF_VM_CLASSIC 1u
#define KERNEL_BPF_MAP_READONLY 1u
#define KERNEL_BPF_MAP_WRITEONLY 2u
/* Direct values are immutable only when user writes are frozen and program
 * writes are forbidden. The owner keeps value live throughout verification. */
#define KERNEL_BPF_MAP_CONSTANT_VALUE 4u

struct kernel_bpf_insn {
    uint8_t code;
    uint8_t dst_reg : 4;
    uint8_t src_reg : 4;
    int16_t off;
    int32_t imm;
};
struct kernel_bpf_classic_insn { uint16_t code; uint8_t jt, jf; uint32_t k; };

/* The program owner retains maps and global-data allocations from verification
 * through program destruction. Map values borrowed by a helper remain live for
 * the complete invocation, including across update/delete helper calls. */
struct kernel_bpf_vm_map {
    void *object;
    void *value;
    uint32_t key_size, value_size, flags, type;
};
enum kernel_bpf_vm_arg_kind {
    KERNEL_BPF_ARG_NONE, KERNEL_BPF_ARG_SCALAR, KERNEL_BPF_ARG_CONST,
    /* Only for a trusted nofault helper's unsafe source address. Accepts an
     * initialized scalar or pointer; the helper must validate/read it safely.
     * This does not establish readable memory or initialized source bytes. */
    KERNEL_BPF_ARG_ANY,
    KERNEL_BPF_ARG_CONTEXT, KERNEL_BPF_ARG_MAP,
    KERNEL_BPF_ARG_MAP_KEY, KERNEL_BPF_ARG_MAP_VALUE,
    KERNEL_BPF_ARG_MEM_READ, KERNEL_BPF_ARG_MEM_WRITE,
    KERNEL_BPF_ARG_RINGBUF,
};
struct kernel_bpf_vm_arg {
    enum kernel_bpf_vm_arg_kind kind;
    /* 1..5 selects a register, zero uses fixed_size. */
    uint8_t size_arg;
    uint32_t fixed_size;
};
enum kernel_bpf_vm_return_kind {
    KERNEL_BPF_RETURN_SCALAR,
    KERNEL_BPF_RETURN_MAP_VALUE_OR_NULL,
    KERNEL_BPF_RETURN_RINGBUF_OR_NULL,
};
struct kernel_bpf_vm_helper {
    struct kernel_bpf_vm_arg args[5];
    enum kernel_bpf_vm_return_kind result;
    uint8_t map_arg, size_arg;
    uint8_t map_read, map_write;
    /* Zero permits any map type; otherwise bit N permits Linux map type N. */
    uint64_t map_types;
    uint32_t fixed_size;
    /* submit/discard consumes its KERNEL_BPF_ARG_RINGBUF argument. */
    uint8_t releases_ringbuf;
};
enum kernel_bpf_vm_context_kind {
    KERNEL_BPF_CONTEXT_SCALAR,
    KERNEL_BPF_CONTEXT_PACKET,
    KERNEL_BPF_CONTEXT_PACKET_END,
};
struct kernel_bpf_vm_context_access {
    enum kernel_bpf_vm_context_kind kind;
    /* Scalar range; zero/zero means full unsigned range. */
    uint64_t min, max;
};
struct kernel_bpf_vm_env {
    void *opaque;
    uint32_t program_type, flags;
    void *(*alloc)(void *, size_t);
    void (*free)(void *, void *);
    /* Called with original immutable instruction and PC. Return zero on success;
     * resolve pseudo-map-FD (src=1), pseudo-map-value (src=2), and loader-supplied
     * map-index forms (src=5/6). Runtime uses the same retained binding. */
    int (*map)(void *, uint32_t pc, const struct kernel_bpf_insn *,
               struct kernel_bpf_vm_map *);
    /* Unknown helper: -ENOENT; known but forbidden for type: -EACCES. */
    int (*helper)(void *, uint32_t id, struct kernel_bpf_vm_helper *);
    int (*context)(void *, int32_t offset, uint32_t size, int write,
                   struct kernel_bpf_vm_context_access *);
    void (*log)(void *, const char *, size_t);
};
struct kernel_bpf_vm_run {
    void *opaque;
    void *context;
    uint32_t context_size;
    /* Optional UAPI-field translation (e.g. __sk_buff data/data_end loads
     * materialize native pointers despite their 32-bit UAPI field width). */
    uint64_t (*context_read)(void *, int32_t offset, uint32_t size);
    uint64_t (*helper)(void *, uint32_t id, const uint64_t args[5]);
    /* Big-endian wire value. Negative ancillary offsets retain Linux's SKF_AD
     * encoding. Return nonzero on OOB: ABS/IND then returns zero, as Linux does. */
    int (*packet)(void *, int64_t offset, uint32_t size, uint32_t *value);
    uint32_t (*packet_length)(void *);
    /* Classic SKF_AD selector (0..60), with A/X for NLATTR searches. */
    int (*ancillary)(void *, uint32_t selector, uint32_t a, uint32_t x,
                     uint32_t *value);
};
struct kernel_bpf_vm_frame {
    uint64_t stack[KERNEL_BPF_STACK_SIZE / 8];
    uint64_t saved[4];
    uint32_t return_pc;
};
struct kernel_bpf_vm_workspace {
    uint64_t registers[11];
    struct kernel_bpf_vm_frame frames[KERNEL_BPF_MAX_FRAMES];
};

int kernel_bpf_verify(const struct kernel_bpf_insn *, uint32_t count,
                      const struct kernel_bpf_vm_env *);
/* Only verified, immutable programs may enter run. No execution timeout: the
 * verifier proves termination under its instruction-complexity budget. */
int kernel_bpf_vm_execute(const struct kernel_bpf_insn *, uint32_t count,
                          const struct kernel_bpf_vm_env *,
                          const struct kernel_bpf_vm_run *,
                          struct kernel_bpf_vm_workspace *, uint64_t *result);
/* Validate first, then convert. Pass output=NULL to obtain required count.
 * Conversion preserves original classic instructions in the owning socket for
 * SO_GET_FILTER; this layer neither mutates nor retains their storage.
 * Conversion emits one internal opcode, LD|LEN (0x80), for packet length.
 * Its destination is A (R0) or X (R7); all other instructions use eBPF ISA.
 * Execute converted programs with env.flags containing VM_CLASSIC. */
int kernel_bpf_classic_convert(const struct kernel_bpf_classic_insn *,
                               uint32_t count, struct kernel_bpf_insn *output,
                               uint32_t capacity, uint32_t *output_count,
                               const struct kernel_bpf_vm_env *);
#endif
