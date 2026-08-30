#include <arch/riscv/user_elf.h>
#include <arch/riscv/mm.h>
#include <kernel/elf64.h>
#include <kernel/heap.h>
#include <kernel/mm.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>

#include <stddef.h>
#include <stdint.h>

#define RISCV_USER_ELF_AUXILIARY_PAIRS 9U

struct riscv_user_elf_stack_layout {
    const struct kernel_exec_string *arguments;
    size_t argument_count;
    const struct kernel_exec_string *executable;
    uint64_t string_address;
    uint64_t stack_pointer;
    uint64_t committed_base;
};

static enum riscv_user_elf_status parser_status(
    enum kernel_elf64_status status)
{
    switch (status) {
    case KERNEL_ELF64_STATUS_OK:
        return RISCV_USER_ELF_STATUS_OK;
    case KERNEL_ELF64_STATUS_INVALID_ARGUMENT:
        return RISCV_USER_ELF_STATUS_INVALID_ARGUMENT;
    case KERNEL_ELF64_STATUS_TRUNCATED:
        return RISCV_USER_ELF_STATUS_TRUNCATED;
    case KERNEL_ELF64_STATUS_MALFORMED:
        return RISCV_USER_ELF_STATUS_MALFORMED;
    case KERNEL_ELF64_STATUS_UNSUPPORTED:
        return RISCV_USER_ELF_STATUS_UNSUPPORTED;
    case KERNEL_ELF64_STATUS_IO:
        return RISCV_USER_ELF_STATUS_IO;
    }
    return RISCV_USER_ELF_STATUS_MALFORMED;
}

static uint32_t segment_permissions(uint32_t flags)
{
    uint32_t permissions = 0U;

    if ((flags & KERNEL_ELF64_FLAG_READ) != 0U) {
        permissions |= RISCV_SV39_READ;
    }
    if ((flags & KERNEL_ELF64_FLAG_WRITE) != 0U) {
        permissions |= RISCV_SV39_WRITE;
    }
    if ((flags & KERNEL_ELF64_FLAG_EXECUTE) != 0U) {
        permissions |= RISCV_SV39_EXECUTE;
    }
    return permissions;
}

static uint32_t mm_permissions(uint32_t permissions)
{
    uint32_t result = 0U;

    if ((permissions & RISCV_SV39_READ) != 0U) {
        result |= KERNEL_MM_READ;
    }
    if ((permissions & RISCV_SV39_WRITE) != 0U) {
        result |= KERNEL_MM_WRITE;
    }
    if ((permissions & RISCV_SV39_EXECUTE) != 0U) {
        result |= KERNEL_MM_EXECUTE;
    }
    return result;
}

static int unsupported_program_type(uint32_t type)
{
    return type == KERNEL_ELF64_PROGRAM_INTERPRETER ||
           type == KERNEL_ELF64_PROGRAM_DYNAMIC ||
           type == KERNEL_ELF64_PROGRAM_TLS;
}

static uint64_t page_start(uint64_t address)
{
    return address & ~BOAROS_PAGE_MASK;
}

static uint64_t page_end(uint64_t address)
{
    return (address + BOAROS_PAGE_MASK) & ~BOAROS_PAGE_MASK;
}

static int ranges_overlap(uint64_t first_start,
                          uint64_t first_end,
                          uint64_t second_start,
                          uint64_t second_end)
{
    return first_start < second_end && second_start < first_end;
}

static enum riscv_user_elf_status read_program_header(
    const struct kernel_elf64_image *image,
    uint16_t index,
    struct kernel_elf64_program_header *header)
{
    enum kernel_elf64_status status =
        kernel_elf64_read_program_header(image, index, header);

    return status == KERNEL_ELF64_STATUS_OK
               ? RISCV_USER_ELF_STATUS_OK
               : parser_status(status);
}

static enum riscv_user_elf_status validate_segment(
    const struct kernel_elf64_program_header *segment,
    uint64_t available_pages)
{
    uint32_t permissions;
    uint64_t end;
    uint64_t pages;

    if (segment->memory_size == 0U) {
        return RISCV_USER_ELF_STATUS_OK;
    }
    end = segment->virtual_address + segment->memory_size;
    if (segment->virtual_address < BOAROS_PAGE_SIZE ||
        end > RISCV_USER_ELF_STACK_GUARD_BASE) {
        return RISCV_USER_ELF_STATUS_INVALID_LAYOUT;
    }
    permissions = segment_permissions(segment->flags);
    if (permissions == 0U ||
        ((permissions & RISCV_SV39_WRITE) != 0U &&
         (permissions & RISCV_SV39_READ) == 0U) ||
        (permissions & (RISCV_SV39_WRITE | RISCV_SV39_EXECUTE)) ==
            (RISCV_SV39_WRITE | RISCV_SV39_EXECUTE)) {
        return RISCV_USER_ELF_STATUS_INVALID_LAYOUT;
    }
    pages = (page_end(end) - page_start(segment->virtual_address)) /
            BOAROS_PAGE_SIZE;
    if (pages > available_pages) {
        return RISCV_USER_ELF_STATUS_NO_MEMORY;
    }
    return RISCV_USER_ELF_STATUS_OK;
}

static enum riscv_user_elf_status validate_pair(
    const struct kernel_elf64_program_header *first,
    const struct kernel_elf64_program_header *second)
{
    uint64_t first_end;
    uint64_t second_end;
    uint32_t combined_permissions;

    if (first->type != KERNEL_ELF64_PROGRAM_LOAD ||
        second->type != KERNEL_ELF64_PROGRAM_LOAD ||
        first->memory_size == 0U || second->memory_size == 0U) {
        return RISCV_USER_ELF_STATUS_OK;
    }
    first_end = first->virtual_address + first->memory_size;
    second_end = second->virtual_address + second->memory_size;
    if (ranges_overlap(first->virtual_address,
                       first_end,
                       second->virtual_address,
                       second_end)) {
        return RISCV_USER_ELF_STATUS_INVALID_LAYOUT;
    }
    if (!ranges_overlap(page_start(first->virtual_address),
                        page_end(first_end),
                        page_start(second->virtual_address),
                        page_end(second_end))) {
        return RISCV_USER_ELF_STATUS_OK;
    }
    combined_permissions = segment_permissions(first->flags) |
                           segment_permissions(second->flags);
    if ((combined_permissions &
         (RISCV_SV39_WRITE | RISCV_SV39_EXECUTE)) ==
        (RISCV_SV39_WRITE | RISCV_SV39_EXECUTE)) {
        return RISCV_USER_ELF_STATUS_INVALID_LAYOUT;
    }
    return RISCV_USER_ELF_STATUS_OK;
}

static enum riscv_user_elf_status validate_image_layout(
    const struct kernel_elf64_image *image,
    uint64_t available_pages)
{
    struct kernel_elf64_program_header first;
    struct kernel_elf64_program_header second;
    uint16_t first_index;
    uint16_t second_index;
    int entry_is_executable = 0;
    int has_load_segment = 0;
    enum riscv_user_elf_status status;

    if ((image->header.entry & 1U) != 0U) {
        return RISCV_USER_ELF_STATUS_INVALID_LAYOUT;
    }
    for (first_index = 0U;
         first_index < image->header.program_header_count;
         first_index++) {
        status = read_program_header(image, first_index, &first);
        if (status != RISCV_USER_ELF_STATUS_OK) {
            return status;
        }
        if (unsupported_program_type(first.type)) {
            return RISCV_USER_ELF_STATUS_UNSUPPORTED;
        }
        if (first.type != KERNEL_ELF64_PROGRAM_LOAD ||
            first.memory_size == 0U) {
            continue;
        }
        has_load_segment = 1;
        status = validate_segment(&first, available_pages);
        if (status != RISCV_USER_ELF_STATUS_OK) {
            return status;
        }
        if ((first.flags & KERNEL_ELF64_FLAG_EXECUTE) != 0U &&
            image->header.entry >= first.virtual_address &&
            image->header.entry - first.virtual_address <
                first.file_size) {
            entry_is_executable = 1;
        }
        for (second_index = first_index + 1U;
             second_index < image->header.program_header_count;
             second_index++) {
            status = read_program_header(image, second_index, &second);
            if (status != RISCV_USER_ELF_STATUS_OK) {
                return status;
            }
            status = validate_pair(&first, &second);
            if (status != RISCV_USER_ELF_STATUS_OK) {
                return status;
            }
        }
    }
    if (!has_load_segment || !entry_is_executable) {
        return RISCV_USER_ELF_STATUS_INVALID_LAYOUT;
    }
    return RISCV_USER_ELF_STATUS_OK;
}

static enum riscv_user_elf_status permissions_for_page(
    const struct kernel_elf64_image *image,
    uint64_t virtual_page,
    uint32_t *permissions)
{
    struct kernel_elf64_program_header segment;
    uint32_t result = 0U;
    uint16_t index;
    enum riscv_user_elf_status status;

    for (index = 0U;
         index < image->header.program_header_count;
         index++) {
        status = read_program_header(image, index, &segment);
        if (status != RISCV_USER_ELF_STATUS_OK) {
            return status;
        }
        if (segment.type != KERNEL_ELF64_PROGRAM_LOAD ||
            segment.memory_size == 0U ||
            !ranges_overlap(virtual_page,
                            virtual_page + BOAROS_PAGE_SIZE,
                            segment.virtual_address,
                            segment.virtual_address +
                                segment.memory_size)) {
            continue;
        }
        result |= segment_permissions(segment.flags);
    }
    if (result == 0U) {
        return RISCV_USER_ELF_STATUS_INVALID_LAYOUT;
    }
    *permissions = result;
    return RISCV_USER_ELF_STATUS_OK;
}

static enum riscv_user_elf_status map_zeroed_page(
    struct riscv_sv39_user_space *space,
    uint64_t virtual_address,
    uint32_t permissions)
{
    struct riscv_sv39_mapping existing;
    enum riscv_sv39_status sv39_status;

    sv39_status = riscv_sv39_user_lookup(space,
                                         virtual_address,
                                         &existing);
    if (sv39_status == RISCV_SV39_STATUS_OK) {
        return existing.permissions ==
                       (permissions | RISCV_SV39_USER)
                   ? RISCV_USER_ELF_STATUS_OK
                   : RISCV_USER_ELF_STATUS_ADDRESS_SPACE;
    }
    if (sv39_status != RISCV_SV39_STATUS_NOT_MAPPED) {
        return RISCV_USER_ELF_STATUS_ADDRESS_SPACE;
    }

    sv39_status = riscv_sv39_user_map_zeroed_page(space,
                                                  virtual_address,
                                                  permissions);
    return sv39_status == RISCV_SV39_STATUS_OK
               ? RISCV_USER_ELF_STATUS_OK
               : sv39_status == RISCV_SV39_STATUS_NO_MEMORY
                     ? RISCV_USER_ELF_STATUS_NO_MEMORY
                     : RISCV_USER_ELF_STATUS_ADDRESS_SPACE;
}

static enum riscv_user_elf_status map_load_pages(
    const struct kernel_elf64_image *image,
    struct riscv_sv39_user_space *space)
{
    struct kernel_elf64_program_header segment;
    uint64_t current;
    uint64_t end;
    uint32_t permissions;
    uint16_t index;
    enum riscv_user_elf_status status;

    for (index = 0U;
         index < image->header.program_header_count;
         index++) {
        status = read_program_header(image, index, &segment);
        if (status != RISCV_USER_ELF_STATUS_OK) {
            return status;
        }
        if (segment.type != KERNEL_ELF64_PROGRAM_LOAD ||
            segment.memory_size == 0U) {
            continue;
        }
        current = page_start(segment.virtual_address);
        end = page_end(segment.virtual_address + segment.memory_size);
        while (current < end) {
            status = permissions_for_page(image, current, &permissions);
            if (status != RISCV_USER_ELF_STATUS_OK) {
                return status;
            }
            status = map_zeroed_page(space,
                                     current,
                                     permissions);
            if (status != RISCV_USER_ELF_STATUS_OK) {
                return status;
            }
            current += BOAROS_PAGE_SIZE;
        }
    }
    return RISCV_USER_ELF_STATUS_OK;
}

static enum riscv_user_elf_status mm_status(
    enum kernel_mm_status status)
{
    switch (status) {
    case KERNEL_MM_STATUS_OK:
        return RISCV_USER_ELF_STATUS_OK;
    case KERNEL_MM_STATUS_INVALID_ARGUMENT:
        return RISCV_USER_ELF_STATUS_INVALID_ARGUMENT;
    case KERNEL_MM_STATUS_NO_MEMORY:
        return RISCV_USER_ELF_STATUS_NO_MEMORY;
    case KERNEL_MM_STATUS_CONFLICT:
        return RISCV_USER_ELF_STATUS_INVALID_LAYOUT;
    case KERNEL_MM_STATUS_CLEANUP_REQUIRED:
        return RISCV_USER_ELF_STATUS_CLEANUP_REQUIRED;
    case KERNEL_MM_STATUS_NOT_MAPPED:
    case KERNEL_MM_STATUS_PAGE_ACCESS:
    case KERNEL_MM_STATUS_PAGE_RELEASE:
    case KERNEL_MM_STATUS_ADDRESS_SPACE:
    case KERNEL_MM_STATUS_STATE:
    default:
        return RISCV_USER_ELF_STATUS_ADDRESS_SPACE;
    }
}

static enum riscv_user_elf_status register_elf_vma_page(
    struct kernel_mm *mm,
    uint64_t virtual_address,
    uint32_t sv39_permissions)
{
    struct kernel_mm_mapping mapping;
    struct kernel_vma existing;
    enum kernel_mm_status status;
    uint32_t permissions = mm_permissions(sv39_permissions);

    status = kernel_mm_lookup(mm, virtual_address, &mapping);
    if (status != KERNEL_MM_STATUS_OK) {
        return mm_status(status);
    }
    if (mapping.permissions != (permissions | KERNEL_MM_USER)) {
        return RISCV_USER_ELF_STATUS_ADDRESS_SPACE;
    }
    status = kernel_mm_vma_lookup(mm, virtual_address, &existing);
    if (status == KERNEL_MM_STATUS_NOT_MAPPED) {
        return mm_status(kernel_mm_vma_insert_anon(
            mm,
            virtual_address,
            virtual_address + BOAROS_PAGE_SIZE,
            permissions,
            KERNEL_VMA_ROLE_ELF));
    }
    if (status != KERNEL_MM_STATUS_OK) {
        return mm_status(status);
    }
    return existing.permissions == permissions &&
                   existing.kind == KERNEL_VMA_KIND_ANONYMOUS &&
                   existing.role == KERNEL_VMA_ROLE_ELF
               ? RISCV_USER_ELF_STATUS_OK
               : RISCV_USER_ELF_STATUS_ADDRESS_SPACE;
}

static enum riscv_user_elf_status register_load_vmas(
    const struct kernel_elf64_image *image,
    struct kernel_mm *mm)
{
    struct kernel_elf64_program_header segment;
    uint64_t current;
    uint64_t end;
    uint32_t permissions;
    uint16_t index;
    enum riscv_user_elf_status status;

    for (index = 0U;
         index < image->header.program_header_count;
         index++) {
        status = read_program_header(image, index, &segment);
        if (status != RISCV_USER_ELF_STATUS_OK) {
            return status;
        }
        if (segment.type != KERNEL_ELF64_PROGRAM_LOAD ||
            segment.memory_size == 0U) {
            continue;
        }
        current = page_start(segment.virtual_address);
        end = page_end(segment.virtual_address + segment.memory_size);
        while (current < end) {
            status = permissions_for_page(image, current, &permissions);
            if (status != RISCV_USER_ELF_STATUS_OK) {
                return status;
            }
            status = register_elf_vma_page(mm, current, permissions);
            if (status != RISCV_USER_ELF_STATUS_OK) {
                return status;
            }
            current += BOAROS_PAGE_SIZE;
        }
    }
    return RISCV_USER_ELF_STATUS_OK;
}

static enum riscv_user_elf_status register_stack_vma(
    const struct riscv_user_elf_stack_layout *layout,
    struct kernel_mm *mm)
{
    struct kernel_mm_mapping mapping;
    const uint32_t permissions = KERNEL_MM_READ | KERNEL_MM_WRITE;
    enum kernel_mm_status status;

    status = kernel_mm_lookup(mm, layout->committed_base, &mapping);
    if (status != KERNEL_MM_STATUS_OK) {
        return mm_status(status);
    }
    if (mapping.permissions != (permissions | KERNEL_MM_USER)) {
        return RISCV_USER_ELF_STATUS_ADDRESS_SPACE;
    }
    return mm_status(kernel_mm_vma_insert_anon(
        mm,
        RISCV_USER_ELF_STACK_RESERVE_BASE,
        RISCV_USER_ELF_STACK_TOP,
        permissions,
        KERNEL_VMA_ROLE_STACK));
}

static enum riscv_user_elf_status copy_load_segments(
    const struct kernel_elf64_image *image,
    struct riscv_sv39_user_space *space)
{
    struct kernel_elf64_program_header segment;
    struct riscv_sv39_mapping mapping;
    unsigned char *page;
    void *pointer;
    uint64_t copied;
    uint64_t current;
    uint64_t chunk;
    uint16_t index;
    enum riscv_user_elf_status status;

    for (index = 0U;
         index < image->header.program_header_count;
         index++) {
        status = read_program_header(image, index, &segment);
        if (status != RISCV_USER_ELF_STATUS_OK) {
            return status;
        }
        if (segment.type != KERNEL_ELF64_PROGRAM_LOAD ||
            segment.file_size == 0U) {
            continue;
        }
        copied = 0U;
        while (copied < segment.file_size) {
            current = segment.virtual_address + copied;
            if (riscv_sv39_user_lookup(space, current, &mapping) !=
                    RISCV_SV39_STATUS_OK ||
                physical_page_resolve(
                    space->allocator,
                    mapping.physical_address & ~BOAROS_PAGE_MASK,
                    &pointer) != PHYSICAL_PAGE_STATUS_OK) {
                return RISCV_USER_ELF_STATUS_ADDRESS_SPACE;
            }
            page = pointer;
            chunk = BOAROS_PAGE_SIZE - (current & BOAROS_PAGE_MASK);
            if (chunk > segment.file_size - copied) {
                chunk = segment.file_size - copied;
            }
            if (kernel_read_source_read_exact(
                    &image->source,
                    segment.offset + copied,
                    page + (current & BOAROS_PAGE_MASK),
                    (size_t)chunk) != 0) {
                return RISCV_USER_ELF_STATUS_IO;
            }
            copied += chunk;
        }
    }
    return RISCV_USER_ELF_STATUS_OK;
}

static enum riscv_user_elf_status populate_u64(
    struct riscv_sv39_user_space *space,
    uint64_t virtual_address,
    uint64_t value)
{
    return riscv_sv39_user_space_populate(space,
                                          virtual_address,
                                          &value,
                                          sizeof(value)) ==
                   RISCV_SV39_STATUS_OK
               ? RISCV_USER_ELF_STATUS_OK
               : RISCV_USER_ELF_STATUS_ADDRESS_SPACE;
}

static enum riscv_user_elf_status populate_string(
    struct riscv_sv39_user_space *space,
    uint64_t virtual_address,
    const struct kernel_exec_string *string)
{
    static const char terminator = '\0';

    if (string->length != 0U &&
        riscv_sv39_user_space_populate(space,
                                       virtual_address,
                                       string->bytes,
                                       string->length) !=
            RISCV_SV39_STATUS_OK) {
        return RISCV_USER_ELF_STATUS_ADDRESS_SPACE;
    }
    return riscv_sv39_user_space_populate(
               space,
               virtual_address + string->length,
               &terminator,
               sizeof(terminator)) == RISCV_SV39_STATUS_OK
               ? RISCV_USER_ELF_STATUS_OK
               : RISCV_USER_ELF_STATUS_ADDRESS_SPACE;
}

static enum riscv_user_elf_status populate_auxiliary(
    struct riscv_sv39_user_space *space,
    uint64_t *word_address,
    uint64_t type,
    uint64_t value)
{
    enum riscv_user_elf_status status =
        populate_u64(space, *word_address, type);

    if (status != RISCV_USER_ELF_STATUS_OK) {
        return status;
    }
    status = populate_u64(space,
                          *word_address + sizeof(uint64_t),
                          value);
    if (status == RISCV_USER_ELF_STATUS_OK) {
        *word_address += 2U * sizeof(uint64_t);
    }
    return status;
}

static enum riscv_user_elf_status program_header_address(
    const struct kernel_elf64_image *image,
    uint64_t *address)
{
    struct kernel_elf64_program_header segment;
    uint64_t table_bytes =
        (uint64_t)image->header.program_header_count * UINT64_C(56);
    uint64_t relative;
    uint16_t index;
    enum riscv_user_elf_status status;

    *address = 0U;
    for (index = 0U;
         index < image->header.program_header_count;
         index++) {
        status = read_program_header(image, index, &segment);
        if (status != RISCV_USER_ELF_STATUS_OK) {
            return status;
        }
        if (segment.type != KERNEL_ELF64_PROGRAM_LOAD ||
            image->header.program_header_offset < segment.offset) {
            continue;
        }
        relative = image->header.program_header_offset - segment.offset;
        if (relative <= segment.file_size &&
            table_bytes <= segment.file_size - relative) {
            *address = segment.virtual_address + relative;
            return RISCV_USER_ELF_STATUS_OK;
        }
    }
    return RISCV_USER_ELF_STATUS_OK;
}

static enum riscv_user_elf_status validate_strings(
    const struct kernel_exec_string *strings,
    size_t count,
    uint64_t *total_bytes)
{
    size_t index;
    size_t byte_index;

    for (index = 0U; index < count; index++) {
        if (strings[index].bytes == 0) {
            return RISCV_USER_ELF_STATUS_INVALID_ARGUMENT;
        }
        if ((uint64_t)strings[index].length >=
                RISCV_USER_ELF_STACK_IMAGE_LIMIT ||
            (uint64_t)strings[index].length + 1U >
                RISCV_USER_ELF_STACK_IMAGE_LIMIT - *total_bytes) {
            return RISCV_USER_ELF_STATUS_ARGUMENT_TOO_LARGE;
        }
        for (byte_index = 0U;
             byte_index < strings[index].length;
             byte_index++) {
            if (strings[index].bytes[byte_index] == '\0') {
                return RISCV_USER_ELF_STATUS_INVALID_ARGUMENT;
            }
        }
        *total_bytes += (uint64_t)strings[index].length + 1U;
    }
    return RISCV_USER_ELF_STATUS_OK;
}

static enum riscv_user_elf_status calculate_stack_layout(
    const struct riscv_user_elf_request *request,
    struct riscv_user_elf_stack_layout *layout)
{
    static const struct kernel_exec_string empty = {"", 0U};
    const struct kernel_exec_string *arguments = request->arguments;
    const struct kernel_exec_string *executable = &request->executable;
    size_t argument_count = request->argument_count;
    uint64_t string_bytes = 0U;
    uint64_t table_bytes;
    uint64_t table_words;
    enum riscv_user_elf_status status;

    if (argument_count == 0U) {
        arguments = &empty;
        argument_count = 1U;
        string_bytes = 1U;
    }
    if (executable->bytes == 0 && executable->length == 0U) {
        executable = &empty;
    }
    if (argument_count >
            RISCV_USER_ELF_STACK_IMAGE_LIMIT / sizeof(uint64_t) ||
        request->environment_count >
            RISCV_USER_ELF_STACK_IMAGE_LIMIT / sizeof(uint64_t)) {
        return RISCV_USER_ELF_STATUS_ARGUMENT_TOO_LARGE;
    }
    table_words = 1U + (uint64_t)argument_count + 1U +
                  (uint64_t)request->environment_count + 1U +
                  2U * RISCV_USER_ELF_AUXILIARY_PAIRS;
    if (table_words >
        RISCV_USER_ELF_STACK_IMAGE_LIMIT / sizeof(uint64_t)) {
        return RISCV_USER_ELF_STATUS_ARGUMENT_TOO_LARGE;
    }
    table_bytes = table_words * sizeof(uint64_t);
    if (request->argument_count != 0U) {
        status = validate_strings(arguments,
                                  argument_count,
                                  &string_bytes);
        if (status != RISCV_USER_ELF_STATUS_OK) {
            return status;
        }
    }
    status = validate_strings(request->environment,
                              request->environment_count,
                              &string_bytes);
    if (status != RISCV_USER_ELF_STATUS_OK) {
        return status;
    }
    status = validate_strings(executable, 1U, &string_bytes);
    if (status != RISCV_USER_ELF_STATUS_OK) {
        return status;
    }
    if (table_bytes >
        RISCV_USER_ELF_STACK_IMAGE_LIMIT - string_bytes) {
        return RISCV_USER_ELF_STATUS_ARGUMENT_TOO_LARGE;
    }

    layout->arguments = arguments;
    layout->argument_count = argument_count;
    layout->executable = executable;
    layout->string_address = RISCV_USER_ELF_STACK_TOP - string_bytes;
    layout->stack_pointer =
        (layout->string_address - table_bytes) & ~UINT64_C(15);
    if (RISCV_USER_ELF_STACK_TOP - layout->stack_pointer >
        RISCV_USER_ELF_STACK_IMAGE_LIMIT) {
        return RISCV_USER_ELF_STATUS_ARGUMENT_TOO_LARGE;
    }
    layout->committed_base =
        page_start(layout->stack_pointer -
                   RISCV_USER_ELF_STACK_INITIAL_HEADROOM);
    return RISCV_USER_ELF_STATUS_OK;
}

static enum riscv_user_elf_status map_stack_pages(
    const struct riscv_user_elf_stack_layout *layout,
    struct riscv_sv39_user_space *space)
{
    uint64_t current;
    enum riscv_user_elf_status status;

    for (current = layout->committed_base;
         current < RISCV_USER_ELF_STACK_TOP;
         current += BOAROS_PAGE_SIZE) {
        status = map_zeroed_page(space,
                                 current,
                                 RISCV_SV39_READ |
                                     RISCV_SV39_WRITE);
        if (status != RISCV_USER_ELF_STATUS_OK) {
            return status;
        }
    }
    return RISCV_USER_ELF_STATUS_OK;
}

static enum riscv_user_elf_status build_argument_stack(
    const struct riscv_user_elf_request *request,
    const struct kernel_elf64_image *image,
    const struct riscv_user_elf_stack_layout *layout,
    struct riscv_sv39_user_space *space,
    uint64_t *stack_pointer)
{
    const struct kernel_exec_string *arguments = layout->arguments;
    size_t argument_count = layout->argument_count;
    uint64_t string_address = layout->string_address;
    uint64_t word_address;
    uint64_t phdr_address;
    size_t index;
    enum riscv_user_elf_status status;

    word_address = layout->stack_pointer;

    status = populate_u64(space,
                          word_address,
                          (uint64_t)argument_count);
    if (status != RISCV_USER_ELF_STATUS_OK) {
        return status;
    }
    word_address += sizeof(uint64_t);
    for (index = 0U; index < argument_count; index++) {
        status = populate_u64(space, word_address, string_address);
        if (status != RISCV_USER_ELF_STATUS_OK) {
            return status;
        }
        status = populate_string(space,
                                 string_address,
                                 &arguments[index]);
        if (status != RISCV_USER_ELF_STATUS_OK) {
            return status;
        }
        word_address += sizeof(uint64_t);
        string_address += (uint64_t)arguments[index].length + 1U;
    }
    status = populate_u64(space, word_address, 0U);
    if (status != RISCV_USER_ELF_STATUS_OK) {
        return status;
    }
    word_address += sizeof(uint64_t);
    for (index = 0U; index < request->environment_count; index++) {
        status = populate_u64(space, word_address, string_address);
        if (status != RISCV_USER_ELF_STATUS_OK) {
            return status;
        }
        status = populate_string(space,
                                 string_address,
                                 &request->environment[index]);
        if (status != RISCV_USER_ELF_STATUS_OK) {
            return status;
        }
        word_address += sizeof(uint64_t);
        string_address +=
            (uint64_t)request->environment[index].length + 1U;
    }
    status = populate_u64(space, word_address, 0U);
    if (status != RISCV_USER_ELF_STATUS_OK) {
        return status;
    }
    word_address += sizeof(uint64_t);

    {
        uint64_t executable_address = string_address;

        status = populate_string(space,
                                 executable_address,
                                 layout->executable);
        if (status != RISCV_USER_ELF_STATUS_OK) {
            return status;
        }

        status = program_header_address(image, &phdr_address);
        if (status != RISCV_USER_ELF_STATUS_OK) {
            return status;
        }
        status = populate_auxiliary(space, &word_address, 6U,
                                    BOAROS_PAGE_SIZE);
        if (status == RISCV_USER_ELF_STATUS_OK) {
            status = populate_auxiliary(space, &word_address, 3U,
                                        phdr_address);
        }
        if (status == RISCV_USER_ELF_STATUS_OK) {
            status = populate_auxiliary(space, &word_address, 4U, 56U);
        }
        if (status == RISCV_USER_ELF_STATUS_OK) {
            status = populate_auxiliary(
                space,
                &word_address,
                5U,
                image->header.program_header_count);
        }
        if (status == RISCV_USER_ELF_STATUS_OK) {
            status = populate_auxiliary(space, &word_address, 7U, 0U);
        }
        if (status == RISCV_USER_ELF_STATUS_OK) {
            status = populate_auxiliary(space, &word_address, 8U, 0U);
        }
        if (status == RISCV_USER_ELF_STATUS_OK) {
            status = populate_auxiliary(space,
                                        &word_address,
                                        9U,
                                        image->header.entry);
        }
        if (status == RISCV_USER_ELF_STATUS_OK) {
            status = populate_auxiliary(space,
                                        &word_address,
                                        31U,
                                        executable_address);
        }
        if (status == RISCV_USER_ELF_STATUS_OK) {
            status = populate_auxiliary(space, &word_address, 0U, 0U);
        }
    }
    if (status != RISCV_USER_ELF_STATUS_OK) {
        return status;
    }
    *stack_pointer = layout->stack_pointer;
    return RISCV_USER_ELF_STATUS_OK;
}

static enum riscv_user_elf_status finish_failure(
    enum riscv_user_elf_status failure,
    struct riscv_sv39_user_space *working,
    struct riscv_sv39_user_space *output)
{
    if (working->state != RISCV_SV39_USER_SPACE_LIVE &&
        working->state != RISCV_SV39_USER_SPACE_CLEANUP) {
        return failure;
    }
    if (riscv_sv39_user_space_destroy(working) ==
        RISCV_SV39_STATUS_OK) {
        return failure;
    }
    if (riscv_sv39_user_space_move(output, working) !=
        RISCV_SV39_STATUS_OK) {
        return RISCV_USER_ELF_STATUS_ADDRESS_SPACE;
    }
    return RISCV_USER_ELF_STATUS_CLEANUP_REQUIRED;
}

enum riscv_user_elf_status riscv_user_elf_load_detailed(
    const struct riscv_user_elf_request *request,
    struct physical_page_allocator *allocator,
    const struct riscv_sv39_page_table *kernel_table,
    struct riscv_sv39_user_space *space,
    struct riscv_user_elf_entry *entry,
    enum riscv_user_elf_status *image_failure)
{
    struct kernel_elf64_image image;
    struct riscv_sv39_user_space working = {0};
    struct riscv_user_elf_entry loaded;
    struct riscv_user_elf_stack_layout stack_layout;
    enum kernel_elf64_status elf_status;
    enum riscv_sv39_status sv39_status;
    enum riscv_user_elf_status status;

    if (request == 0 || request->source.read_at == 0 ||
        (request->argument_count != 0U && request->arguments == 0) ||
        (request->environment_count != 0U &&
         request->environment == 0) ||
        allocator == 0 || kernel_table == 0 ||
        space == 0 || entry == 0 || image_failure == 0 ||
        space->state != RISCV_SV39_USER_SPACE_EMPTY ||
        kernel_table->state != RISCV_SV39_STATE_ACTIVE ||
        kernel_table->allocator != allocator) {
        return RISCV_USER_ELF_STATUS_INVALID_ARGUMENT;
    }
    *image_failure = RISCV_USER_ELF_STATUS_INVALID_ARGUMENT;
    elf_status = kernel_elf64_open(&request->source, &image);
    if (elf_status != KERNEL_ELF64_STATUS_OK) {
        *image_failure = parser_status(elf_status);
        return *image_failure;
    }
    if (image.header.machine != KERNEL_ELF64_MACHINE_RISCV) {
        *image_failure = RISCV_USER_ELF_STATUS_WRONG_ARCH;
        return RISCV_USER_ELF_STATUS_WRONG_ARCH;
    }
    if (image.header.type != KERNEL_ELF64_TYPE_EXECUTABLE) {
        *image_failure = RISCV_USER_ELF_STATUS_UNSUPPORTED;
        return RISCV_USER_ELF_STATUS_UNSUPPORTED;
    }
    status = calculate_stack_layout(request, &stack_layout);
    if (status != RISCV_USER_ELF_STATUS_OK) {
        *image_failure = status;
        return status;
    }
    status = validate_image_layout(&image,
                                   physical_page_available(allocator));
    if (status != RISCV_USER_ELF_STATUS_OK) {
        *image_failure = status;
        return status;
    }

    sv39_status = riscv_sv39_user_space_init(&working,
                                             allocator,
                                             kernel_table);
    if (sv39_status != RISCV_SV39_STATUS_OK) {
        status = sv39_status == RISCV_SV39_STATUS_NO_MEMORY
                     ? RISCV_USER_ELF_STATUS_NO_MEMORY
                     : RISCV_USER_ELF_STATUS_ADDRESS_SPACE;
        *image_failure = status;
        return finish_failure(status, &working, space);
    }
    status = map_load_pages(&image, &working);
    if (status != RISCV_USER_ELF_STATUS_OK) {
        *image_failure = status;
        return finish_failure(status, &working, space);
    }
    status = map_stack_pages(&stack_layout, &working);
    if (status != RISCV_USER_ELF_STATUS_OK) {
        *image_failure = status;
        return finish_failure(status, &working, space);
    }
    status = copy_load_segments(&image, &working);
    if (status != RISCV_USER_ELF_STATUS_OK) {
        *image_failure = status;
        return finish_failure(status, &working, space);
    }

    loaded.entry = image.header.entry;
    status = build_argument_stack(request,
                                  &image,
                                  &stack_layout,
                                  &working,
                                  &loaded.stack_pointer);
    if (status != RISCV_USER_ELF_STATUS_OK) {
        *image_failure = status;
        return finish_failure(status, &working, space);
    }
    if (riscv_sv39_user_space_move(space, &working) !=
        RISCV_SV39_STATUS_OK) {
        *image_failure = RISCV_USER_ELF_STATUS_ADDRESS_SPACE;
        return finish_failure(RISCV_USER_ELF_STATUS_ADDRESS_SPACE,
                              &working,
                              space);
    }
    *entry = loaded;
    *image_failure = RISCV_USER_ELF_STATUS_OK;
    return RISCV_USER_ELF_STATUS_OK;
}

enum riscv_user_elf_status riscv_user_elf_load(
    const struct riscv_user_elf_request *request,
    struct physical_page_allocator *allocator,
    const struct riscv_sv39_page_table *kernel_table,
    struct riscv_sv39_user_space *space,
    struct riscv_user_elf_entry *entry)
{
    enum riscv_user_elf_status image_failure;

    return riscv_user_elf_load_detailed(request,
                                        allocator,
                                        kernel_table,
                                        space,
                                        entry,
                                        &image_failure);
}

enum riscv_user_elf_status riscv_user_elf_register_static_vmas(
    const struct riscv_user_elf_request *request,
    struct kernel_mm *mm,
    struct kernel_heap *heap)
{
    struct kernel_elf64_image image;
    struct riscv_user_elf_stack_layout stack_layout;
    enum kernel_elf64_status elf_status;
    enum riscv_user_elf_status status;

    if (request == 0 || request->source.read_at == 0 || mm == 0 ||
        heap == 0 ||
        (request->argument_count != 0U && request->arguments == 0) ||
        (request->environment_count != 0U && request->environment == 0)) {
        return RISCV_USER_ELF_STATUS_INVALID_ARGUMENT;
    }
    elf_status = kernel_elf64_open(&request->source, &image);
    if (elf_status != KERNEL_ELF64_STATUS_OK) {
        return parser_status(elf_status);
    }
    if (image.header.machine != KERNEL_ELF64_MACHINE_RISCV) {
        return RISCV_USER_ELF_STATUS_WRONG_ARCH;
    }
    if (image.header.type != KERNEL_ELF64_TYPE_EXECUTABLE) {
        return RISCV_USER_ELF_STATUS_UNSUPPORTED;
    }
    status = calculate_stack_layout(request, &stack_layout);
    if (status != RISCV_USER_ELF_STATUS_OK) {
        return status;
    }
    status = validate_image_layout(&image, UINT64_MAX);
    if (status != RISCV_USER_ELF_STATUS_OK) {
        return status;
    }
    status = mm_status(kernel_mm_vma_enable(mm, heap));
    if (status != RISCV_USER_ELF_STATUS_OK) {
        return status;
    }
    status = register_load_vmas(&image, mm);
    if (status != RISCV_USER_ELF_STATUS_OK) {
        return status;
    }
    return register_stack_vma(&stack_layout, mm);
}
