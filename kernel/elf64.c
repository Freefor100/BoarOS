#include <kernel/elf64.h>

#include <stddef.h>
#include <stdint.h>

#define ELF64_HEADER_SIZE UINT64_C(64)
#define ELF64_PROGRAM_HEADER_SIZE UINT64_C(56)

static uint16_t read_u16(const unsigned char *bytes)
{
    return (uint16_t)bytes[0] |
           ((uint16_t)bytes[1] << 8U);
}

static uint32_t read_u32(const unsigned char *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

static uint64_t read_u64(const unsigned char *bytes)
{
    uint64_t value = 0U;
    uint32_t index;

    for (index = 0U; index < 8U; index++) {
        value |= (uint64_t)bytes[index] << (index * 8U);
    }
    return value;
}

static int range_within(uint64_t size, uint64_t offset, uint64_t length)
{
    return offset <= size && length <= size - offset;
}

static void decode_program_header(
    const unsigned char *bytes,
    struct kernel_elf64_program_header *header)
{
    header->type = read_u32(bytes);
    header->flags = read_u32(bytes + 4U);
    header->offset = read_u64(bytes + 8U);
    header->virtual_address = read_u64(bytes + 16U);
    header->physical_address = read_u64(bytes + 24U);
    header->file_size = read_u64(bytes + 32U);
    header->memory_size = read_u64(bytes + 40U);
    header->alignment = read_u64(bytes + 48U);
}

static enum kernel_elf64_status validate_load_segment(
    const struct kernel_elf64_program_header *header,
    uint64_t image_size)
{
    uint64_t alignment_mask;

    if (header->file_size > header->memory_size) {
        return KERNEL_ELF64_STATUS_MALFORMED;
    }
    if (!range_within(image_size, header->offset, header->file_size)) {
        return KERNEL_ELF64_STATUS_TRUNCATED;
    }
    if (header->memory_size > UINT64_MAX - header->virtual_address) {
        return KERNEL_ELF64_STATUS_MALFORMED;
    }
    if (header->alignment > 1U) {
        alignment_mask = header->alignment - 1U;
        if ((header->alignment & alignment_mask) != 0U ||
            (header->virtual_address & alignment_mask) !=
                (header->offset & alignment_mask)) {
            return KERNEL_ELF64_STATUS_MALFORMED;
        }
    }
    return KERNEL_ELF64_STATUS_OK;
}

enum kernel_elf64_status kernel_elf64_open(
    const struct kernel_read_source *source,
    struct kernel_elf64_image *image)
{
    unsigned char data[ELF64_HEADER_SIZE];
    struct kernel_elf64_image decoded;
    struct kernel_elf64_program_header program_header;
    uint64_t program_header_bytes;
    uint64_t program_header_offset;
    uint16_t index;
    enum kernel_elf64_status status;

    if (source == 0 || source->read_at == 0 || image == 0) {
        return KERNEL_ELF64_STATUS_INVALID_ARGUMENT;
    }
    if (source->size < ELF64_HEADER_SIZE) {
        return KERNEL_ELF64_STATUS_TRUNCATED;
    }
    if (kernel_read_source_read_exact(source,
                                      0U,
                                      data,
                                      sizeof(data)) != 0) {
        return KERNEL_ELF64_STATUS_IO;
    }
    if (data[0] != 0x7fU || data[1] != 'E' || data[2] != 'L' ||
        data[3] != 'F') {
        return KERNEL_ELF64_STATUS_MALFORMED;
    }
    if (data[4] != KERNEL_ELF64_CLASS_64 ||
        data[5] != KERNEL_ELF64_DATA_LITTLE_ENDIAN ||
        data[6] != KERNEL_ELF64_VERSION_CURRENT ||
        read_u32(data + 20U) != KERNEL_ELF64_VERSION_CURRENT) {
        return KERNEL_ELF64_STATUS_UNSUPPORTED;
    }
    if (read_u16(data + 52U) != ELF64_HEADER_SIZE ||
        read_u16(data + 54U) != ELF64_PROGRAM_HEADER_SIZE) {
        return KERNEL_ELF64_STATUS_MALFORMED;
    }

    decoded.header.type = read_u16(data + 16U);
    decoded.header.machine = read_u16(data + 18U);
    decoded.header.entry = read_u64(data + 24U);
    decoded.header.program_header_offset = read_u64(data + 32U);
    decoded.header.program_header_count = read_u16(data + 56U);
    if (decoded.header.program_header_count == 0U) {
        return KERNEL_ELF64_STATUS_MALFORMED;
    }
    if (decoded.header.program_header_count >
        KERNEL_ELF64_MAX_PROGRAM_HEADERS) {
        return KERNEL_ELF64_STATUS_UNSUPPORTED;
    }
    if (decoded.header.program_header_offset < ELF64_HEADER_SIZE) {
        return KERNEL_ELF64_STATUS_MALFORMED;
    }
    program_header_bytes =
        (uint64_t)decoded.header.program_header_count *
        ELF64_PROGRAM_HEADER_SIZE;
    if (!range_within(source->size,
                      decoded.header.program_header_offset,
                      program_header_bytes)) {
        return KERNEL_ELF64_STATUS_TRUNCATED;
    }

    decoded.source = *source;

    for (index = 0U;
         index < decoded.header.program_header_count;
         index++) {
        program_header_offset =
            decoded.header.program_header_offset +
            (uint64_t)index * ELF64_PROGRAM_HEADER_SIZE;
        if (kernel_read_source_read_exact(source,
                                          program_header_offset,
                                          data,
                                          ELF64_PROGRAM_HEADER_SIZE) != 0) {
            return KERNEL_ELF64_STATUS_IO;
        }
        decode_program_header(data, &program_header);
        if (program_header.type != KERNEL_ELF64_PROGRAM_LOAD) {
            continue;
        }
        status = validate_load_segment(&program_header, source->size);
        if (status != KERNEL_ELF64_STATUS_OK) {
            return status;
        }
    }

    *image = decoded;
    return KERNEL_ELF64_STATUS_OK;
}

enum kernel_elf64_status kernel_elf64_read_program_header(
    const struct kernel_elf64_image *image,
    uint16_t index,
    struct kernel_elf64_program_header *header)
{
    struct kernel_elf64_program_header decoded;
    unsigned char data[ELF64_PROGRAM_HEADER_SIZE];
    uint64_t offset;
    uint64_t relative_offset;

    if (image == 0 || header == 0 || image->source.read_at == 0 ||
        index >= image->header.program_header_count) {
        return KERNEL_ELF64_STATUS_INVALID_ARGUMENT;
    }
    relative_offset = (uint64_t)index * ELF64_PROGRAM_HEADER_SIZE;
    if (image->header.program_header_offset >
        UINT64_MAX - relative_offset) {
        return KERNEL_ELF64_STATUS_TRUNCATED;
    }
    offset = image->header.program_header_offset + relative_offset;
    if (!range_within(image->source.size,
                      offset,
                      ELF64_PROGRAM_HEADER_SIZE)) {
        return KERNEL_ELF64_STATUS_TRUNCATED;
    }
    if (kernel_read_source_read_exact(&image->source,
                                      offset,
                                      data,
                                      sizeof(data)) != 0) {
        return KERNEL_ELF64_STATUS_IO;
    }
    decode_program_header(data, &decoded);
    *header = decoded;
    return KERNEL_ELF64_STATUS_OK;
}
