#ifndef BOAROS_KERNEL_ELF64_H
#define BOAROS_KERNEL_ELF64_H

#include <kernel/read_source.h>

#include <stddef.h>
#include <stdint.h>

#define KERNEL_ELF64_CLASS_64 2U
#define KERNEL_ELF64_DATA_LITTLE_ENDIAN 1U
#define KERNEL_ELF64_VERSION_CURRENT 1U

#define KERNEL_ELF64_TYPE_EXECUTABLE 2U
#define KERNEL_ELF64_TYPE_SHARED 3U

#define KERNEL_ELF64_MACHINE_RISCV 243U
#define KERNEL_ELF64_MACHINE_LOONGARCH 258U

#define KERNEL_ELF64_PROGRAM_NULL 0U
#define KERNEL_ELF64_PROGRAM_LOAD 1U
#define KERNEL_ELF64_PROGRAM_DYNAMIC 2U
#define KERNEL_ELF64_PROGRAM_INTERPRETER 3U
#define KERNEL_ELF64_PROGRAM_NOTE 4U
#define KERNEL_ELF64_PROGRAM_HEADER 6U
#define KERNEL_ELF64_PROGRAM_TLS 7U

#define KERNEL_ELF64_FLAG_EXECUTE (UINT32_C(1) << 0U)
#define KERNEL_ELF64_FLAG_WRITE (UINT32_C(1) << 1U)
#define KERNEL_ELF64_FLAG_READ (UINT32_C(1) << 2U)

#define KERNEL_ELF64_MAX_PROGRAM_HEADERS 128U

enum kernel_elf64_status {
    KERNEL_ELF64_STATUS_OK = 0,
    KERNEL_ELF64_STATUS_INVALID_ARGUMENT,
    KERNEL_ELF64_STATUS_TRUNCATED,
    KERNEL_ELF64_STATUS_MALFORMED,
    KERNEL_ELF64_STATUS_UNSUPPORTED,
    KERNEL_ELF64_STATUS_IO,
};

struct kernel_elf64_header {
    uint16_t type;
    uint16_t machine;
    uint64_t entry;
    uint64_t program_header_offset;
    uint16_t program_header_count;
};

struct kernel_elf64_program_header {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t virtual_address;
    uint64_t physical_address;
    uint64_t file_size;
    uint64_t memory_size;
    uint64_t alignment;
};

struct kernel_elf64_image {
    struct kernel_read_source source;
    struct kernel_elf64_header header;
};

enum kernel_elf64_status kernel_elf64_open(
    const struct kernel_read_source *source,
    struct kernel_elf64_image *image);

/* Parses and validates program headers into caller-owned storage once. */
enum kernel_elf64_status kernel_elf64_open_cached(
    const struct kernel_read_source *source,
    struct kernel_elf64_image *image,
    struct kernel_elf64_program_header *program_headers,
    uint16_t program_header_capacity);

enum kernel_elf64_status kernel_elf64_read_program_header(
    const struct kernel_elf64_image *image,
    uint16_t index,
    struct kernel_elf64_program_header *header);

#endif
