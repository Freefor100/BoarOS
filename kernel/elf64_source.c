#include <kernel/elf64_source.h>
#include <kernel/heap.h>
#include <kernel/open_file.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>
#include <kernel/vfs.h>

#include <stddef.h>
#include <stdint.h>

#define KERNEL_ELF64_SOURCE_MAGIC UINT64_C(0x454c46534f555243)
#define KERNEL_ELF64_INTERPRETER_LIMIT UINT64_C(4096)

enum source_state {
    SOURCE_LIVE = 0,
    SOURCE_CLEANUP,
};

struct kernel_elf64_source {
    uint64_t magic;
    struct kernel_heap *heap;
    struct kernel_open_file_description *file;
    struct kernel_elf64_header header;
    struct kernel_elf64_program_header *program_headers;
    uint16_t program_header_count;
    char *interpreter;
    size_t interpreter_length;
    uint64_t *boundaries;
    struct kernel_elf64_source_run *runs;
    uint32_t run_count;
    uint32_t references;
    uint64_t page_size;
    struct physical_page_allocator *cleanup_page_allocator;
    uint64_t cleanup_page_address;
    uint8_t cleanup_page_valid;
    enum source_state state;
};

static int source_valid(const struct kernel_elf64_source *source)
{
    if (source == 0 || source->magic != KERNEL_ELF64_SOURCE_MAGIC ||
        source->heap == 0 || source->references == UINT32_MAX ||
        source->page_size == 0U ||
        (source->state != SOURCE_LIVE && source->state != SOURCE_CLEANUP)) {
        return 0;
    }
    if (source->state == SOURCE_CLEANUP) {
        return 1;
    }
    return source->program_headers != 0 &&
           source->program_header_count != 0U && source->runs != 0 &&
           source->run_count != 0U;
}

static int source_read_at(void *context,
                          uint64_t offset,
                          void *buffer,
                          size_t size)
{
    size_t bytes_read = 0U;

    return kernel_open_file_pread(context,
                                  offset,
                                  buffer,
                                  size,
                                  &bytes_read) == 0 &&
           bytes_read == size
               ? 0
               : -1;
}

static uint64_t align_down(uint64_t value, uint64_t page_size)
{
    return value & ~(page_size - 1U);
}

static int align_up(uint64_t value, uint64_t page_size, uint64_t *result)
{
    uint64_t mask = page_size - 1U;

    if (value > UINT64_MAX - mask) {
        return 0;
    }
    *result = (value + mask) & ~mask;
    return 1;
}

static int range_overlap(uint64_t left_start,
                         uint64_t left_end,
                         uint64_t right_start,
                         uint64_t right_end)
{
    return left_start < right_end && right_start < left_end;
}

static int insert_boundary(uint64_t *boundaries,
                           uint32_t *count,
                           uint32_t capacity,
                           uint64_t value)
{
    uint32_t index = 0U;

    if (*count == capacity) {
        return 0;
    }
    while (index < *count && boundaries[index] < value) {
        index++;
    }
    if (index < *count && boundaries[index] == value) {
        return 1;
    }
    for (uint32_t current = *count; current > index; current--) {
        boundaries[current] = boundaries[current - 1U];
    }
    boundaries[index] = value;
    (*count)++;
    return 1;
}

static enum kernel_elf64_source_status map_elf_status(
    enum kernel_elf64_status status)
{
    switch (status) {
    case KERNEL_ELF64_STATUS_OK:
        return KERNEL_ELF64_SOURCE_STATUS_OK;
    case KERNEL_ELF64_STATUS_INVALID_ARGUMENT:
        return KERNEL_ELF64_SOURCE_STATUS_INVALID_ARGUMENT;
    case KERNEL_ELF64_STATUS_TRUNCATED:
        return KERNEL_ELF64_SOURCE_STATUS_TRUNCATED;
    case KERNEL_ELF64_STATUS_MALFORMED:
        return KERNEL_ELF64_SOURCE_STATUS_MALFORMED;
    case KERNEL_ELF64_STATUS_UNSUPPORTED:
        return KERNEL_ELF64_SOURCE_STATUS_UNSUPPORTED;
    case KERNEL_ELF64_STATUS_IO:
    default:
        return KERNEL_ELF64_SOURCE_STATUS_IO;
    }
}

static int release_pointer(struct kernel_elf64_source *source,
                           void **pointer)
{
    if (*pointer == 0) {
        return 1;
    }
    if (kernel_heap_release(source->heap, *pointer) !=
        KERNEL_HEAP_STATUS_OK) {
        return 0;
    }
    *pointer = 0;
    return 1;
}

static enum kernel_elf64_source_status release_pending_page(
    struct kernel_elf64_source *source)
{
    if (source->cleanup_page_valid == 0U) {
        return KERNEL_ELF64_SOURCE_STATUS_OK;
    }
    if (source->cleanup_page_allocator == 0 ||
        physical_page_release(source->cleanup_page_allocator,
                              source->cleanup_page_address) !=
            PHYSICAL_PAGE_STATUS_OK) {
        return KERNEL_ELF64_SOURCE_STATUS_CLEANUP_REQUIRED;
    }
    source->cleanup_page_allocator = 0;
    source->cleanup_page_address = 0U;
    source->cleanup_page_valid = 0U;
    return KERNEL_ELF64_SOURCE_STATUS_OK;
}

static enum kernel_elf64_source_status discard_page(
    struct kernel_elf64_source *source,
    struct physical_page_allocator *allocator,
    uint64_t address,
    enum kernel_elf64_source_status failure)
{
    if (physical_page_release(allocator, address) ==
        PHYSICAL_PAGE_STATUS_OK) {
        return failure;
    }
    if (source->cleanup_page_valid != 0U) {
        return KERNEL_ELF64_SOURCE_STATUS_STATE;
    }
    source->cleanup_page_allocator = allocator;
    source->cleanup_page_address = address;
    source->cleanup_page_valid = 1U;
    return KERNEL_ELF64_SOURCE_STATUS_CLEANUP_REQUIRED;
}

static enum kernel_elf64_source_status source_cleanup(
    struct kernel_elf64_source **owner)
{
    struct kernel_elf64_source *source = *owner;
    int failed = 0;

    source->state = SOURCE_CLEANUP;
    if (release_pending_page(source) != KERNEL_ELF64_SOURCE_STATUS_OK) {
        failed = 1;
    }
    if (source->file != 0) {
        struct kernel_open_file_description *file = source->file;

        if (kernel_open_file_release(&file) != KERNEL_OPEN_FILE_STATUS_OK) {
            failed = 1;
        } else {
            source->file = 0;
        }
    }
    if (!release_pointer(source, (void **)&source->interpreter)) {
        failed = 1;
    }
    if (!release_pointer(source, (void **)&source->boundaries)) {
        failed = 1;
    }
    if (!release_pointer(source, (void **)&source->runs)) {
        failed = 1;
    }
    if (!release_pointer(source, (void **)&source->program_headers)) {
        failed = 1;
    }
    if (source->file != 0 || source->interpreter != 0 ||
        source->boundaries != 0 || source->cleanup_page_valid != 0U ||
        source->runs != 0 || source->program_headers != 0 || failed) {
        return KERNEL_ELF64_SOURCE_STATUS_CLEANUP_REQUIRED;
    }
    source->magic = 0U;
    if (kernel_heap_release(source->heap, source) != KERNEL_HEAP_STATUS_OK) {
        source->magic = KERNEL_ELF64_SOURCE_MAGIC;
        return KERNEL_ELF64_SOURCE_STATUS_CLEANUP_REQUIRED;
    }
    *owner = 0;
    return KERNEL_ELF64_SOURCE_STATUS_OK;
}

static enum kernel_elf64_source_status abort_source(
    struct kernel_elf64_source **owner,
    enum kernel_elf64_source_status failure)
{
    enum kernel_elf64_source_status cleanup_status;

    if (owner == 0 || *owner == 0) {
        return failure;
    }
    (*owner)->references = 0U;
    cleanup_status = source_cleanup(owner);
    return cleanup_status == KERNEL_ELF64_SOURCE_STATUS_OK
               ? failure
               : KERNEL_ELF64_SOURCE_STATUS_CLEANUP_REQUIRED;
}

static int validate_program_headers(
    const struct kernel_elf64_source *source,
    uint16_t machine,
    uint64_t image_size,
    uint64_t *minimum,
    uint64_t *maximum,
    uint32_t *boundary_capacity)
{
    uint16_t index;
    uint32_t load_count = 0U;
    uint32_t interpreter_count = 0U;
    uint64_t low = UINT64_MAX;
    uint64_t high = 0U;
    uint64_t capacity = 4U;

    if (source->header.type != KERNEL_ELF64_TYPE_EXECUTABLE &&
        source->header.type != KERNEL_ELF64_TYPE_SHARED) {
        return 0;
    }
    if (source->header.machine != machine) {
        return -1;
    }
    for (index = 0U; index < source->program_header_count; index++) {
        const struct kernel_elf64_program_header *header =
            &source->program_headers[index];
        uint64_t end;
        uint64_t page_end;

        if (header->type == KERNEL_ELF64_PROGRAM_INTERPRETER) {
            interpreter_count++;
            if (header->file_size < 2U ||
                header->file_size > KERNEL_ELF64_INTERPRETER_LIMIT ||
                header->file_size > SIZE_MAX ||
                header->offset > image_size ||
                header->file_size > image_size - header->offset) {
                return 0;
            }
            continue;
        }
        if (header->type != KERNEL_ELF64_PROGRAM_LOAD ||
            header->memory_size == 0U) {
            continue;
        }
        if (header->file_size > header->memory_size ||
            header->offset > image_size ||
            header->file_size > image_size - header->offset ||
            header->memory_size > UINT64_MAX - header->virtual_address) {
            return 0;
        }
        end = header->virtual_address + header->memory_size;
        if (!align_up(end, source->page_size, &page_end)) {
            return 0;
        }
        if ((header->flags & KERNEL_ELF64_FLAG_WRITE) != 0U &&
            (header->flags & KERNEL_ELF64_FLAG_EXECUTE) != 0U) {
            return 0;
        }
        low = header->virtual_address < low ? header->virtual_address : low;
        high = page_end > high ? page_end : high;
        load_count++;
        if (capacity > UINT32_MAX - 4U) {
            return 0;
        }
        capacity += 4U;
    }
    if (load_count == 0U || low == UINT64_MAX || high <= low ||
        interpreter_count > 1U) {
        return 0;
    }
    *minimum = align_down(low, source->page_size);
    *maximum = high;
    *boundary_capacity = capacity > UINT32_MAX ? 0U : (uint32_t)capacity;
    return *boundary_capacity != 0U;
}

static int reject_overlapping_loads(
    const struct kernel_elf64_source *source)
{
    uint16_t first;

    for (first = 0U; first < source->program_header_count; first++) {
        const struct kernel_elf64_program_header *left =
            &source->program_headers[first];
        uint16_t second;

        if (left->type != KERNEL_ELF64_PROGRAM_LOAD ||
            left->memory_size == 0U) {
            continue;
        }
        for (second = first + 1U;
             second < source->program_header_count;
             second++) {
            const struct kernel_elf64_program_header *right =
                &source->program_headers[second];
            uint64_t left_end;
            uint64_t right_end;
            uint64_t left_page_end;
            uint64_t right_page_end;
            uint32_t flags;

            if (right->type != KERNEL_ELF64_PROGRAM_LOAD ||
                right->memory_size == 0U) {
                continue;
            }
            left_end = left->virtual_address + left->memory_size;
            right_end = right->virtual_address + right->memory_size;
            if (range_overlap(left->virtual_address,
                              left_end,
                              right->virtual_address,
                              right_end)) {
                return 0;
            }
            if (!align_up(left_end, source->page_size, &left_page_end) ||
                !align_up(right_end, source->page_size, &right_page_end)) {
                return 0;
            }
            if (!range_overlap(align_down(left->virtual_address,
                                          source->page_size),
                               left_page_end,
                               align_down(right->virtual_address,
                                          source->page_size),
                               right_page_end)) {
                continue;
            }
            flags = left->flags | right->flags;
            if ((flags & KERNEL_ELF64_FLAG_WRITE) != 0U &&
                (flags & KERNEL_ELF64_FLAG_EXECUTE) != 0U) {
                return 0;
            }
        }
    }
    return 1;
}

static enum kernel_elf64_source_status build_runs(
    struct kernel_elf64_source *source,
    uint64_t minimum,
    uint64_t maximum,
    uint32_t boundary_capacity)
{
    uint32_t boundary_count = 0U;
    uint32_t index;
    uint32_t run_count = 0U;
    enum kernel_heap_status heap_status;

    heap_status = kernel_heap_allocate_zeroed(source->heap,
                                              boundary_capacity,
                                              sizeof(*source->boundaries),
                                              (void **)&source->boundaries);
    if (heap_status != KERNEL_HEAP_STATUS_OK) {
        return heap_status == KERNEL_HEAP_STATUS_EMPTY
                   ? KERNEL_ELF64_SOURCE_STATUS_NO_MEMORY
                   : KERNEL_ELF64_SOURCE_STATUS_STATE;
    }
    if (!insert_boundary(source->boundaries,
                         &boundary_count,
                         boundary_capacity,
                         minimum) ||
        !insert_boundary(source->boundaries,
                         &boundary_count,
                         boundary_capacity,
                         maximum)) {
        return KERNEL_ELF64_SOURCE_STATUS_STATE;
    }
    for (index = 0U; index < source->program_header_count; index++) {
        const struct kernel_elf64_program_header *header =
            &source->program_headers[index];
        uint64_t end;
        uint64_t start_page;
        uint64_t end_page;
        uint64_t file_end_page;

        if (header->type != KERNEL_ELF64_PROGRAM_LOAD ||
            header->memory_size == 0U) {
            continue;
        }
        end = header->virtual_address + header->memory_size;
        start_page = align_down(header->virtual_address, source->page_size);
        if (!align_up(end, source->page_size, &end_page) ||
            !align_up(header->virtual_address + header->file_size,
                      source->page_size,
                      &file_end_page) ||
            !insert_boundary(source->boundaries,
                             &boundary_count,
                             boundary_capacity,
                             start_page) ||
            !insert_boundary(source->boundaries,
                             &boundary_count,
                             boundary_capacity,
                             end_page) ||
            !insert_boundary(source->boundaries,
                             &boundary_count,
                             boundary_capacity,
                             file_end_page)) {
            return KERNEL_ELF64_SOURCE_STATUS_STATE;
        }
    }

    heap_status = kernel_heap_allocate_zeroed(source->heap,
                                              boundary_count,
                                              sizeof(*source->runs),
                                              (void **)&source->runs);
    if (heap_status != KERNEL_HEAP_STATUS_OK) {
        return heap_status == KERNEL_HEAP_STATUS_EMPTY
                   ? KERNEL_ELF64_SOURCE_STATUS_NO_MEMORY
                   : KERNEL_ELF64_SOURCE_STATUS_STATE;
    }
    for (index = 0U; index + 1U < boundary_count; index++) {
        uint64_t page = source->boundaries[index];
        uint64_t next = source->boundaries[index + 1U];
        uint32_t flags = 0U;
        uint32_t contributors = 0U;
        uint32_t file_contributors = 0U;
        uint16_t file_index = 0U;
        enum kernel_elf64_source_run_kind kind;
        uint16_t ph_index;

        if (next <= page) {
            continue;
        }
        for (ph_index = 0U;
             ph_index < source->program_header_count;
             ph_index++) {
            const struct kernel_elf64_program_header *header =
                &source->program_headers[ph_index];
            uint64_t end;
            uint64_t file_end;

            if (header->type != KERNEL_ELF64_PROGRAM_LOAD ||
                header->memory_size == 0U) {
                continue;
            }
            end = header->virtual_address + header->memory_size;
            if (!range_overlap(page, next,
                               align_down(header->virtual_address,
                                          source->page_size),
                               end)) {
                continue;
            }
            contributors++;
            flags |= header->flags;
            file_end = header->virtual_address + header->file_size;
            if (range_overlap(page, page + source->page_size,
                              header->virtual_address,
                              file_end)) {
                file_contributors++;
                file_index = ph_index;
            }
        }
        if (contributors == 0U) {
            continue;
        }
        if (file_contributors == 0U) {
            kind = KERNEL_ELF64_SOURCE_RUN_ZERO;
        } else if (contributors == 1U && file_contributors == 1U &&
                   page >= source->program_headers[file_index].virtual_address &&
                   page <= UINT64_MAX - source->page_size &&
                   page + source->page_size <=
                       source->program_headers[file_index].virtual_address +
                       source->program_headers[file_index].file_size &&
                   source->program_headers[file_index].offset <=
                       UINT64_MAX -
                           (page - source->program_headers[file_index].virtual_address) &&
                   ((source->program_headers[file_index].offset +
                     (page - source->program_headers[file_index].virtual_address)) &
                    (source->page_size - 1U)) == 0U) {
            kind = KERNEL_ELF64_SOURCE_RUN_FILE;
        } else {
            kind = KERNEL_ELF64_SOURCE_RUN_COMPOSITE;
        }
        if (run_count != 0U) {
            struct kernel_elf64_source_run *previous =
                &source->runs[run_count - 1U];
            int contiguous = previous->end == page;
            int offset_contiguous = kind != KERNEL_ELF64_SOURCE_RUN_FILE ||
                (previous->file_offset <= UINT64_MAX -
                 (page - previous->start) &&
                 previous->file_offset + (page - previous->start) ==
                     source->program_headers[file_index].offset +
                     (page - source->program_headers[file_index].virtual_address));

            if (contiguous && previous->kind == kind &&
                previous->flags == flags && offset_contiguous) {
                previous->end = next;
                continue;
            }
        }
        source->runs[run_count] = (struct kernel_elf64_source_run){
            .start = page,
            .end = next,
            .file_offset = kind == KERNEL_ELF64_SOURCE_RUN_FILE
                               ? source->program_headers[file_index].offset +
                                 (page - source->program_headers[file_index].virtual_address)
                               : 0U,
            .flags = flags,
            .kind = kind,
        };
        run_count++;
    }
    if (run_count == 0U) {
        if (!release_pointer(source, (void **)&source->boundaries)) {
            return KERNEL_ELF64_SOURCE_STATUS_CLEANUP_REQUIRED;
        }
        return KERNEL_ELF64_SOURCE_STATUS_MALFORMED;
    }
    if (!release_pointer(source, (void **)&source->boundaries)) {
        return KERNEL_ELF64_SOURCE_STATUS_CLEANUP_REQUIRED;
    }
    source->run_count = run_count;
    return KERNEL_ELF64_SOURCE_STATUS_OK;
}

enum kernel_elf64_source_status kernel_elf64_source_create(
    struct kernel_heap *heap,
    struct kernel_open_file_description **file,
    uint64_t page_size,
    uint16_t machine,
    struct kernel_elf64_source **source_out)
{
    struct kernel_read_source read_source;
    struct kernel_elf64_image image = {0};
    struct kernel_elf64_source *source = 0;
    uint64_t image_size;
    uint64_t minimum;
    uint64_t maximum;
    uint32_t boundary_capacity;
    uint16_t index;
    uint16_t interpreter_index = UINT16_MAX;
    enum kernel_elf64_source_status status;
    enum kernel_heap_status heap_status;

    if (heap == 0 || file == 0 || *file == 0 || source_out == 0 ||
        *source_out != 0 || page_size != BOAROS_PAGE_SIZE ||
        (page_size & (page_size - 1U)) != 0U ||
        (kernel_open_file_mode(*file) & KERNEL_VFS_S_IFMT) !=
            KERNEL_VFS_S_IFREG) {
        return KERNEL_ELF64_SOURCE_STATUS_INVALID_ARGUMENT;
    }
    image_size = kernel_open_file_size(*file);
    read_source = (struct kernel_read_source){
        .context = *file,
        .size = image_size,
        .read_at = source_read_at,
    };
    heap_status = kernel_heap_allocate_zeroed(heap,
                                              1U,
                                              sizeof(*source),
                                              (void **)&source);
    if (heap_status != KERNEL_HEAP_STATUS_OK) {
        return heap_status == KERNEL_HEAP_STATUS_EMPTY
                   ? KERNEL_ELF64_SOURCE_STATUS_NO_MEMORY
                   : KERNEL_ELF64_SOURCE_STATUS_STATE;
    }
    source->magic = KERNEL_ELF64_SOURCE_MAGIC;
    source->heap = heap;
    source->references = 1U;
    source->page_size = page_size;
    source->state = SOURCE_LIVE;
    *source_out = source;
    heap_status = kernel_heap_allocate_zeroed(
        heap,
        KERNEL_ELF64_MAX_PROGRAM_HEADERS,
        sizeof(*source->program_headers),
        (void **)&source->program_headers);
    if (heap_status != KERNEL_HEAP_STATUS_OK) {
        status = heap_status == KERNEL_HEAP_STATUS_EMPTY
                   ? KERNEL_ELF64_SOURCE_STATUS_NO_MEMORY
                   : KERNEL_ELF64_SOURCE_STATUS_STATE;
        return abort_source(source_out, status);
    }
    image.source = read_source;
    status = map_elf_status(kernel_elf64_open_cached(
        &read_source,
        &image,
        source->program_headers,
        KERNEL_ELF64_MAX_PROGRAM_HEADERS));
    if (status != KERNEL_ELF64_SOURCE_STATUS_OK) {
        return abort_source(source_out, status);
    }
    source->header = image.header;
    source->program_header_count = image.header.program_header_count;
    for (index = 0U; index < source->program_header_count; index++) {
        if (source->program_headers[index].type ==
            KERNEL_ELF64_PROGRAM_INTERPRETER) {
            interpreter_index = index;
        }
    }
    {
        int validation = validate_program_headers(source,
                                                  machine,
                                                  image_size,
                                                  &minimum,
                                                  &maximum,
                                                  &boundary_capacity);

        if (validation < 0) {
            return abort_source(source_out,
                                KERNEL_ELF64_SOURCE_STATUS_WRONG_ARCH);
        }
        if (validation == 0 ||
        !reject_overlapping_loads(source)) {
            return abort_source(source_out,
                                KERNEL_ELF64_SOURCE_STATUS_MALFORMED);
        }
    }
    if (interpreter_index != UINT16_MAX) {
        const struct kernel_elf64_program_header *header =
            &source->program_headers[interpreter_index];
        size_t length = (size_t)header->file_size;

        heap_status = kernel_heap_allocate_zeroed(heap,
                                                  1U,
                                                  length,
                                                  (void **)&source->interpreter);
        if (heap_status != KERNEL_HEAP_STATUS_OK) {
            status = heap_status == KERNEL_HEAP_STATUS_EMPTY
                       ? KERNEL_ELF64_SOURCE_STATUS_NO_MEMORY
                       : KERNEL_ELF64_SOURCE_STATUS_STATE;
            return abort_source(source_out, status);
        }
        if (kernel_read_source_read_exact(&read_source,
                                          header->offset,
                                          source->interpreter,
                                          length) != 0 ||
            source->interpreter[length - 1U] != '\0' ||
            source->interpreter[0] != '/') {
            return abort_source(source_out,
                                KERNEL_ELF64_SOURCE_STATUS_MALFORMED);
        }
        for (size_t byte = 0U; byte + 1U < length; byte++) {
            if (source->interpreter[byte] == '\0') {
                return abort_source(source_out,
                                    KERNEL_ELF64_SOURCE_STATUS_MALFORMED);
            }
        }
        source->interpreter_length = length - 1U;
    }
    status = build_runs(source, minimum, maximum, boundary_capacity);
    if (status != KERNEL_ELF64_SOURCE_STATUS_OK) {
        return abort_source(source_out, status);
    }
    source->file = *file;
    *file = 0;
    return KERNEL_ELF64_SOURCE_STATUS_OK;
}

enum kernel_elf64_source_status kernel_elf64_source_acquire(
    struct kernel_elf64_source *source)
{
    if (!source_valid(source) || source->state != SOURCE_LIVE ||
        source->references == 0U || source->references == UINT32_MAX) {
        return KERNEL_ELF64_SOURCE_STATUS_STATE;
    }
    source->references++;
    return KERNEL_ELF64_SOURCE_STATUS_OK;
}

enum kernel_elf64_source_status kernel_elf64_source_release(
    struct kernel_elf64_source **owner)
{
    struct kernel_elf64_source *source;

    if (owner == 0 || *owner == 0 || !source_valid(*owner)) {
        return KERNEL_ELF64_SOURCE_STATUS_INVALID_ARGUMENT;
    }
    source = *owner;
    if (source->state == SOURCE_LIVE) {
        if (source->references == 0U) {
            return KERNEL_ELF64_SOURCE_STATUS_STATE;
        }
        if (source->references > 1U) {
            source->references--;
            *owner = 0;
            return KERNEL_ELF64_SOURCE_STATUS_OK;
        }
        source->references = 0U;
    }
    return source_cleanup(owner);
}

const struct kernel_elf64_header *kernel_elf64_source_header(
    const struct kernel_elf64_source *source)
{
    return source_valid(source) ? &source->header : 0;
}

uint16_t kernel_elf64_source_program_header_count(
    const struct kernel_elf64_source *source)
{
    return source_valid(source) ? source->program_header_count : 0U;
}

const struct kernel_elf64_program_header *kernel_elf64_source_program_header(
    const struct kernel_elf64_source *source,
    uint16_t index)
{
    return source_valid(source) && index < source->program_header_count
               ? &source->program_headers[index]
               : 0;
}

const char *kernel_elf64_source_interpreter(
    const struct kernel_elf64_source *source,
    size_t *length)
{
    if (length != 0) {
        *length = 0U;
    }
    if (!source_valid(source) || source->interpreter == 0) {
        return 0;
    }
    if (length != 0) {
        *length = source->interpreter_length;
    }
    return source->interpreter;
}

uint32_t kernel_elf64_source_run_count(
    const struct kernel_elf64_source *source)
{
    return source_valid(source) ? source->run_count : 0U;
}

const struct kernel_elf64_source_run *kernel_elf64_source_run_at(
    const struct kernel_elf64_source *source,
    uint32_t index)
{
    return source_valid(source) && index < source->run_count
               ? &source->runs[index]
               : 0;
}

static const struct kernel_elf64_source_run *find_run(
    const struct kernel_elf64_source *source,
    uint64_t virtual_offset)
{
    uint32_t low = 0U;
    uint32_t high = source->run_count;

    while (low < high) {
        uint32_t middle = low + (high - low) / 2U;
        const struct kernel_elf64_source_run *run =
            &source->runs[middle];

        if (virtual_offset < run->start) {
            high = middle;
        } else if (virtual_offset >= run->end) {
            low = middle + 1U;
        } else {
            return run;
        }
    }
    return 0;
}

static void zero_page(void *page, uint64_t page_size)
{
    unsigned char *bytes = page;
    uint64_t index;

    for (index = 0U; index < page_size; index++) {
        bytes[index] = 0U;
    }
}

enum kernel_elf64_source_status kernel_elf64_source_page(
    struct kernel_elf64_source *source,
    struct physical_page_allocator *allocator,
    uint64_t virtual_offset,
    uint64_t *physical_address,
    int *shared)
{
    const struct kernel_elf64_source_run *run;
    uint64_t address;
    void *page;
    enum physical_page_status page_status;
    enum kernel_elf64_source_status cleanup_status;

    if (!source_valid(source) || source->state != SOURCE_LIVE ||
        source->file == 0 || allocator == 0 || physical_address == 0 || shared == 0 ||
        virtual_offset % source->page_size != 0U) {
        return KERNEL_ELF64_SOURCE_STATUS_INVALID_ARGUMENT;
    }
    cleanup_status = release_pending_page(source);
    if (cleanup_status != KERNEL_ELF64_SOURCE_STATUS_OK) {
        return cleanup_status;
    }
    run = find_run(source, virtual_offset);
    if (run == 0) {
        return KERNEL_ELF64_SOURCE_STATUS_MALFORMED;
    }
    if (run->kind == KERNEL_ELF64_SOURCE_RUN_FILE) {
        uint64_t file_offset;
        size_t valid_bytes;
        enum kernel_page_cache_status cache_status;

        if (run->file_offset > UINT64_MAX - (virtual_offset - run->start) ||
            ((run->file_offset + (virtual_offset - run->start)) &
             (source->page_size - 1U)) != 0U) {
            return KERNEL_ELF64_SOURCE_STATUS_MALFORMED;
        }
        file_offset = run->file_offset + (virtual_offset - run->start);
        cache_status = kernel_open_file_get_page(
            source->file,
            file_offset >> BOAROS_PAGE_SHIFT,
            physical_address,
            &valid_bytes);
        if (cache_status != KERNEL_PAGE_CACHE_STATUS_OK) {
            return cache_status == KERNEL_PAGE_CACHE_STATUS_NO_MEMORY
                       ? KERNEL_ELF64_SOURCE_STATUS_NO_MEMORY
                       : cache_status == KERNEL_PAGE_CACHE_STATUS_IO ||
                                 cache_status == KERNEL_PAGE_CACHE_STATUS_OUT_OF_RANGE
                             ? KERNEL_ELF64_SOURCE_STATUS_IO
                             : KERNEL_ELF64_SOURCE_STATUS_STATE;
        }
        *shared = 1;
        return KERNEL_ELF64_SOURCE_STATUS_OK;
    }
    page_status = physical_page_allocate(allocator, &address);
    if (page_status != PHYSICAL_PAGE_STATUS_OK) {
        return page_status == PHYSICAL_PAGE_STATUS_EMPTY
                   ? KERNEL_ELF64_SOURCE_STATUS_NO_MEMORY
                   : KERNEL_ELF64_SOURCE_STATUS_STATE;
    }
    if (physical_page_resolve(allocator, address, &page) !=
        PHYSICAL_PAGE_STATUS_OK) {
        return discard_page(source,
                            allocator,
                            address,
                            KERNEL_ELF64_SOURCE_STATUS_STATE);
    }
    zero_page(page, source->page_size);
    if (run->kind == KERNEL_ELF64_SOURCE_RUN_COMPOSITE) {
        uint16_t index;
        uint64_t page_end;

        if (virtual_offset > UINT64_MAX - source->page_size) {
            return discard_page(source,
                                allocator,
                                address,
                                KERNEL_ELF64_SOURCE_STATUS_MALFORMED);
        }
        page_end = virtual_offset + source->page_size;

        for (index = 0U; index < source->program_header_count; index++) {
            const struct kernel_elf64_program_header *header =
                &source->program_headers[index];
            uint64_t file_end;
            uint64_t copy_start;
            uint64_t copy_end;
            size_t bytes_read = 0U;

            if (header->type != KERNEL_ELF64_PROGRAM_LOAD ||
                header->file_size == 0U) {
                continue;
            }
            file_end = header->virtual_address + header->file_size;
            copy_start = header->virtual_address > virtual_offset
                              ? header->virtual_address
                              : virtual_offset;
            copy_end = file_end < page_end ? file_end : page_end;
            if (copy_start >= copy_end) {
                continue;
            }
            if (header->offset > UINT64_MAX -
                                     (copy_start - header->virtual_address) ||
                kernel_open_file_pread(
                    source->file,
                    header->offset + (copy_start - header->virtual_address),
                    (unsigned char *)page + (copy_start - virtual_offset),
                    (size_t)(copy_end - copy_start),
                    &bytes_read) != 0 ||
                bytes_read != (size_t)(copy_end - copy_start)) {
                return discard_page(source,
                                    allocator,
                                    address,
                                    KERNEL_ELF64_SOURCE_STATUS_IO);
            }
        }
    }
    *physical_address = address;
    *shared = 0;
    return KERNEL_ELF64_SOURCE_STATUS_OK;
}

uint64_t kernel_elf64_source_file_size(
    const struct kernel_elf64_source *source)
{
    return source_valid(source) && source->file != 0
               ? kernel_open_file_size(source->file)
               : 0U;
}

struct kernel_open_file_description *kernel_elf64_source_file(
    const struct kernel_elf64_source *source)
{
    return source_valid(source) ? source->file : 0;
}
