#include <arch/riscv/user_elf.h>
#include <kernel/elf64.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>

#include <stddef.h>
#include <stdint.h>

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

static void copy_bytes(unsigned char *destination,
                       const unsigned char *source,
                       size_t size)
{
    size_t index;

    for (index = 0U; index < size; index++) {
        destination[index] = source[index];
    }
}

static enum riscv_user_elf_status copy_load_segments(
    const struct kernel_elf64_image *image,
    struct riscv_sv39_user_space *space,
    struct physical_page_allocator *allocator)
{
    struct kernel_elf64_program_header segment;
    struct riscv_sv39_mapping mapping;
    uint64_t copied;
    uint64_t current;
    uint64_t chunk;
    uint64_t offset_in_page;
    void *page;
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
                    allocator,
                    mapping.physical_address & ~BOAROS_PAGE_MASK,
                    &page) != PHYSICAL_PAGE_STATUS_OK) {
                return RISCV_USER_ELF_STATUS_ADDRESS_SPACE;
            }
            offset_in_page = current & BOAROS_PAGE_MASK;
            chunk = BOAROS_PAGE_SIZE - offset_in_page;
            if (chunk > segment.file_size - copied) {
                chunk = segment.file_size - copied;
            }
            copy_bytes((unsigned char *)page + offset_in_page,
                       image->bytes + segment.offset + copied,
                       (size_t)chunk);
            copied += chunk;
        }
    }
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

enum riscv_user_elf_status riscv_user_elf_load(
    const void *image_bytes,
    size_t image_size,
    struct physical_page_allocator *allocator,
    const struct riscv_sv39_page_table *kernel_table,
    struct riscv_sv39_user_space *space,
    struct riscv_user_elf_entry *entry)
{
    struct kernel_elf64_image image;
    struct riscv_sv39_user_space working = {0};
    struct riscv_user_elf_entry loaded;
    enum kernel_elf64_status elf_status;
    enum riscv_sv39_status sv39_status;
    enum riscv_user_elf_status status;

    if (image_bytes == 0 || allocator == 0 || kernel_table == 0 ||
        space == 0 || entry == 0 ||
        space->state != RISCV_SV39_USER_SPACE_EMPTY ||
        kernel_table->state != RISCV_SV39_STATE_ACTIVE ||
        kernel_table->allocator != allocator) {
        return RISCV_USER_ELF_STATUS_INVALID_ARGUMENT;
    }
    elf_status = kernel_elf64_open(image_bytes, image_size, &image);
    if (elf_status != KERNEL_ELF64_STATUS_OK) {
        return parser_status(elf_status);
    }
    if (image.header.machine != KERNEL_ELF64_MACHINE_RISCV) {
        return RISCV_USER_ELF_STATUS_WRONG_ARCH;
    }
    if (image.header.type != KERNEL_ELF64_TYPE_EXECUTABLE) {
        return RISCV_USER_ELF_STATUS_UNSUPPORTED;
    }
    status = validate_image_layout(&image,
                                   physical_page_available(allocator));
    if (status != RISCV_USER_ELF_STATUS_OK) {
        return status;
    }

    sv39_status = riscv_sv39_user_space_init(&working,
                                             allocator,
                                             kernel_table);
    if (sv39_status != RISCV_SV39_STATUS_OK) {
        return finish_failure(
            sv39_status == RISCV_SV39_STATUS_NO_MEMORY
                ? RISCV_USER_ELF_STATUS_NO_MEMORY
                : RISCV_USER_ELF_STATUS_ADDRESS_SPACE,
            &working,
            space);
    }
    status = map_load_pages(&image, &working);
    if (status != RISCV_USER_ELF_STATUS_OK) {
        return finish_failure(status, &working, space);
    }
    status = map_zeroed_page(&working,
                             RISCV_USER_ELF_STACK_BASE,
                             RISCV_SV39_READ | RISCV_SV39_WRITE);
    if (status != RISCV_USER_ELF_STATUS_OK) {
        return finish_failure(status, &working, space);
    }
    status = copy_load_segments(&image, &working, allocator);
    if (status != RISCV_USER_ELF_STATUS_OK) {
        return finish_failure(status, &working, space);
    }

    loaded.entry = image.header.entry;
    loaded.stack_pointer = RISCV_USER_ELF_STACK_TOP;
    if (riscv_sv39_user_space_move(space, &working) !=
        RISCV_SV39_STATUS_OK) {
        return finish_failure(RISCV_USER_ELF_STATUS_ADDRESS_SPACE,
                              &working,
                              space);
    }
    *entry = loaded;
    return RISCV_USER_ELF_STATUS_OK;
}
