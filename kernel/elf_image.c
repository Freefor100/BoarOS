#include <kernel/elf_image.h>
#include <arch/elf.h>

#include <kernel/mm_backend.h>
#include <kernel/elf64_source.h>
#include <kernel/heap.h>
#include <kernel/mm.h>
#include <kernel/page.h>
#include <kernel/random.h>

#include <stddef.h>
#include <stdint.h>

#define KERNEL_ELF_STACK_RESERVE UINT64_C(0x800000)
#define KERNEL_ELF_STACK_GUARD BOAROS_PAGE_SIZE
#define KERNEL_ELF_STACK_HEADROOM UINT64_C(0x10000)
#define KERNEL_ELF_STACK_IMAGE_LIMIT KERNEL_EXEC_STRING_LIMIT
#define KERNEL_ELF_MMAP_GAP UINT64_C(0x8000000)
#define KERNEL_ELF_BRK_RANDOM_RANGE UINT64_C(0x40000000)
#define KERNEL_ELF_STACK_RANDOM_BITS 18U
#define KERNEL_ELF_LAYOUT_RANDOM_BITS 24U
#define KERNEL_ELF_CLKTCK UINT64_C(100)

struct kernel_elf_layout {
    uint64_t main_bias;
    uint64_t interpreter_bias;
    uint64_t interpreter_end;
    uint64_t main_entry;
    uint64_t interpreter_entry;
    uint64_t main_phdr;
    uint64_t main_end;
    uint64_t brk_start;
    uint64_t brk_limit;
    uint64_t mmap_base;
    uint64_t stack_top;
    uint64_t stack_base;
    uint64_t stack_soft_base;
    uint64_t stack_guard;
    uint64_t vdso;
    uint64_t stack_pointer;
    uint64_t committed_stack_base;
    uint64_t string_address;
    uint64_t executable_address;
    uint64_t random_address;
    size_t random_size;
    size_t argument_count;
    const struct kernel_exec_string *arguments;
    size_t environment_count;
    const struct kernel_exec_string *environment;
    const struct kernel_exec_string *executable;
    int has_interpreter;
    int has_random;
    uint8_t random_data[16];
};

static enum kernel_elf_image_status cleanup_space(
    struct kernel_mm *mm,
    struct arch_mmu_user_space *space)
{
    enum kernel_mm_status mm_status;
    enum arch_mmu_status space_status;

    if (mm->state == KERNEL_MM_LIVE || mm->state == KERNEL_MM_CLEANUP) {
        mm_status = kernel_mm_release(mm);
        if (mm_status != KERNEL_MM_STATUS_OK) {
            return mm_status == KERNEL_MM_STATUS_CLEANUP_REQUIRED
                       ? KERNEL_ELF_IMAGE_STATUS_CLEANUP_REQUIRED
                       : KERNEL_ELF_IMAGE_STATUS_ADDRESS_SPACE;
        }
    }
    if (space->state == ARCH_MMU_USER_SPACE_LIVE) {
        space_status = arch_mmu_user_space_destroy(space);
        if (space_status != ARCH_MMU_STATUS_OK) {
            return KERNEL_ELF_IMAGE_STATUS_ADDRESS_SPACE;
        }
    }
    return KERNEL_ELF_IMAGE_STATUS_OK;
}

static uint64_t align_down_page(uint64_t value)
{
    return value & ~BOAROS_PAGE_MASK;
}

static int align_up_page(uint64_t value, uint64_t *result)
{
    if (value > UINT64_MAX - BOAROS_PAGE_MASK) {
        return 0;
    }
    *result = (value + BOAROS_PAGE_MASK) & ~BOAROS_PAGE_MASK;
    return 1;
}

static int random_pages(uint32_t bits, uint64_t *value)
{
    uint64_t random_value;

    if (value == 0) {
        return 0;
    }
    *value = 0U;
    if (!kernel_random_available()) {
        return 1;
    }
    if (kernel_random_fill(&random_value, sizeof(random_value)) !=
        KERNEL_RANDOM_STATUS_OK) {
        return 0;
    }
    if (bits < 64U) {
        random_value &= (UINT64_C(1) << bits) - 1U;
    }
    *value = random_value;
    return 1;
}

static int range_conflict(uint64_t first_start,
                          uint64_t first_end,
                          uint64_t second_start,
                          uint64_t second_end)
{
    return first_start < second_end && second_start < first_end;
}

static int source_bounds(const struct kernel_elf64_source *source,
                         uint64_t *minimum,
                         uint64_t *maximum,
                         uint64_t *alignment)
{
    uint32_t count = kernel_elf64_source_run_count(source);
    uint32_t index;
    uint64_t low = UINT64_MAX;
    uint64_t high = 0U;
    uint64_t max_alignment = BOAROS_PAGE_SIZE;

    if (count == 0U || minimum == 0 || maximum == 0 || alignment == 0) {
        return 0;
    }
    for (index = 0U; index < count; index++) {
        const struct kernel_elf64_source_run *run =
            kernel_elf64_source_run_at(source, index);

        if (run == 0 || run->start >= run->end) {
            return 0;
        }
        low = run->start < low ? run->start : low;
        high = run->end > high ? run->end : high;
    }
    {
        uint16_t ph_count = kernel_elf64_source_program_header_count(source);

        for (index = 0U; index < ph_count; index++) {
            const struct kernel_elf64_program_header *header =
                kernel_elf64_source_program_header(source, (uint16_t)index);

            if (header == 0 || header->type != KERNEL_ELF64_PROGRAM_LOAD ||
                header->memory_size == 0U) {
                continue;
            }
            if (header->alignment > max_alignment) {
                max_alignment = header->alignment;
            }
        }
    }
    if (low == UINT64_MAX || high <= low ||
        (max_alignment & (max_alignment - 1U)) != 0U) {
        return 0;
    }
    *minimum = low;
    *maximum = high;
    *alignment = max_alignment < BOAROS_PAGE_SIZE
                      ? BOAROS_PAGE_SIZE
                      : max_alignment;
    return 1;
}

static int source_entry_is_executable(
    const struct kernel_elf64_source *source,
    uint64_t entry)
{
    uint16_t count = kernel_elf64_source_program_header_count(source);
    uint16_t index;

    for (index = 0U; index < count; index++) {
        const struct kernel_elf64_program_header *header =
            kernel_elf64_source_program_header(source, index);

        if (header != 0 && header->type == KERNEL_ELF64_PROGRAM_LOAD &&
            (header->flags & KERNEL_ELF64_FLAG_EXECUTE) != 0U &&
            entry >= header->virtual_address &&
            entry - header->virtual_address < header->file_size) {
            return 1;
        }
    }
    return 0;
}

static int source_program_header_address(
    const struct kernel_elf64_source *source,
    uint64_t bias,
    uint64_t *address)
{
    const struct kernel_elf64_header *header =
        kernel_elf64_source_header(source);
    uint64_t table_size;
    uint16_t count;
    uint16_t index;

    if (header == 0 || address == 0) {
        return 0;
    }
    table_size = (uint64_t)header->program_header_count * UINT64_C(56);
    count = header->program_header_count;
    if (table_size == 0U) {
        *address = 0U;
        return 1;
    }
    for (index = 0U; index < count; index++) {
        const struct kernel_elf64_program_header *segment =
            kernel_elf64_source_program_header(source, index);
        uint64_t relative;

        if (segment == 0 || segment->type != KERNEL_ELF64_PROGRAM_LOAD ||
            header->program_header_offset < segment->offset) {
            continue;
        }
        relative = header->program_header_offset - segment->offset;
        if (relative <= segment->file_size &&
            table_size <= segment->file_size - relative &&
            relative <= segment->memory_size &&
            table_size <= segment->memory_size - relative &&
            segment->virtual_address <= UINT64_MAX - relative &&
            bias <= UINT64_MAX -
                       (segment->virtual_address + relative)) {
            *address = bias + segment->virtual_address + relative;
            return 1;
        }
    }
    *address = 0U;
    return 1;
}

static int source_highest_end(const struct kernel_elf64_source *source,
                              uint64_t bias,
                              uint64_t *end)
{
    uint64_t minimum;
    uint64_t maximum;
    uint64_t alignment;

    if (!source_bounds(source, &minimum, &maximum, &alignment) ||
        bias > UINT64_MAX - maximum) {
        return 0;
    }
    (void)alignment;
    *end = bias + maximum;
    return 1;
}

static int choose_pie_bias(const struct kernel_elf64_source *source,
                           uint64_t mmap_base,
                           uint64_t *bias)
{
    uint64_t minimum;
    uint64_t maximum;
    uint64_t alignment;
    uint64_t target;
    uint64_t lower_bias;
    uint64_t upper_bias;
    uint64_t available;
    uint64_t slots;
    uint64_t random_value;
    uint64_t delta;

    if (!source_bounds(source, &minimum, &maximum, &alignment) ||
        maximum < minimum || mmap_base < maximum ||
        !random_pages(KERNEL_ELF_LAYOUT_RANDOM_BITS, &random_value)) {
        return 0;
    }
    target = (ARCH_MMU_USER_LIMIT / 3U) * 2U;
    if (target < minimum) {
        target = minimum;
    }
    delta = target - minimum;
    if (delta > UINT64_MAX - (alignment - 1U)) {
        return 0;
    }
    lower_bias = (delta + alignment - 1U) & ~(alignment - 1U);
    upper_bias = mmap_base - maximum;
    upper_bias &= ~(alignment - 1U);
    if (lower_bias > upper_bias) {
        return 0;
    }
    available = upper_bias - lower_bias;
    slots = available / alignment;
    if (slots != UINT64_MAX) {
        random_value %= slots + 1U;
    }
    if (random_value > slots ||
        random_value > (UINT64_MAX - lower_bias) / alignment) {
        return 0;
    }
    *bias = lower_bias + random_value * alignment;
    return 1;
}

static int choose_interpreter_bias(const struct kernel_elf64_source *source,
                                   uint64_t upper,
                                   uint64_t lower,
                                   uint64_t *bias)
{
    uint64_t minimum;
    uint64_t maximum;
    uint64_t alignment;
    uint64_t lower_bias;
    uint64_t upper_bias;
    uint64_t available;
    uint64_t slots;
    uint64_t random_value;
    uint64_t delta;

    if (!source_bounds(source, &minimum, &maximum, &alignment) ||
        maximum < minimum || upper <= lower ||
        maximum > upper ||
        !random_pages(KERNEL_ELF_LAYOUT_RANDOM_BITS, &random_value)) {
        return 0;
    }
    if (lower <= minimum) {
        lower_bias = 0U;
    } else {
        delta = lower - minimum;
        if (delta > UINT64_MAX - (alignment - 1U)) {
            return 0;
        }
        lower_bias = (delta + alignment - 1U) & ~(alignment - 1U);
    }
    upper_bias = (upper - maximum) & ~(alignment - 1U);
    if (lower_bias > upper_bias) {
        return 0;
    }
    available = upper_bias - lower_bias;
    slots = available / alignment;
    if (slots != UINT64_MAX) {
        random_value %= slots + 1U;
    }
    if (random_value > slots ||
        random_value > (UINT64_MAX - lower_bias) / alignment) {
        return 0;
    }
    *bias = upper_bias - random_value * alignment;
    return 1;
}

static enum kernel_elf_image_status prepare_stack_layout(
    const struct kernel_exec_image_request *request,
    const struct kernel_elf_layout *base,
    struct kernel_elf_layout *layout)
{
    static const struct kernel_exec_string empty = {"", 0U};
    uint64_t string_bytes = 0U;
    uint64_t table_words;
    uint64_t table_bytes;
    uint64_t total_bytes;
    uint64_t random_bytes = kernel_random_available() ? 16U : 0U;
    size_t index;

    *layout = *base;
    layout->arguments = request->arguments;
    layout->argument_count = request->argument_count;
    if (layout->argument_count == 0U) {
        layout->arguments = &empty;
        layout->argument_count = 1U;
    }
    layout->environment = request->environment;
    layout->environment_count = request->environment_count;
    layout->executable = &request->executable;
    if (layout->executable->bytes == 0 && layout->executable->length != 0U) {
        return KERNEL_ELF_IMAGE_STATUS_INVALID_ARGUMENT;
    }
    if (layout->executable->bytes == 0 && layout->executable->length == 0U) {
        layout->executable = &empty;
    }
    for (index = 0U; index < layout->argument_count; index++) {
        if (layout->arguments[index].bytes == 0 ||
            layout->arguments[index].length >= KERNEL_ELF_STACK_IMAGE_LIMIT ||
            string_bytes > KERNEL_ELF_STACK_IMAGE_LIMIT -
                               layout->arguments[index].length - 1U) {
            return KERNEL_ELF_IMAGE_STATUS_INVALID_ARGUMENT;
        }
        string_bytes += (uint64_t)layout->arguments[index].length + 1U;
    }
    for (index = 0U; index < layout->environment_count; index++) {
        if (layout->environment[index].bytes == 0 ||
            layout->environment[index].length >= KERNEL_ELF_STACK_IMAGE_LIMIT ||
            string_bytes > KERNEL_ELF_STACK_IMAGE_LIMIT -
                               layout->environment[index].length - 1U) {
            return KERNEL_ELF_IMAGE_STATUS_INVALID_ARGUMENT;
        }
        string_bytes += (uint64_t)layout->environment[index].length + 1U;
    }
    if (layout->executable->length >= KERNEL_ELF_STACK_IMAGE_LIMIT ||
        string_bytes > KERNEL_ELF_STACK_IMAGE_LIMIT -
                           layout->executable->length - 1U) {
        return KERNEL_ELF_IMAGE_STATUS_INVALID_ARGUMENT;
    }
    string_bytes += (uint64_t)layout->executable->length + 1U;
    if (random_bytes > KERNEL_ELF_STACK_IMAGE_LIMIT - string_bytes) {
        return KERNEL_ELF_IMAGE_STATUS_INVALID_ARGUMENT;
    }
    total_bytes = string_bytes + random_bytes;
    if (layout->argument_count > KERNEL_ELF_STACK_IMAGE_LIMIT / 8U ||
        layout->environment_count > KERNEL_ELF_STACK_IMAGE_LIMIT / 8U) {
        return KERNEL_ELF_IMAGE_STATUS_INVALID_ARGUMENT;
    }
    table_words = 1U + (uint64_t)layout->argument_count + 1U +
                  (uint64_t)layout->environment_count + 1U +
                  2U * (16U + (random_bytes != 0U ? 1U : 0U));
    if (table_words > UINT64_MAX / sizeof(uint64_t)) {
        return KERNEL_ELF_IMAGE_STATUS_INVALID_ARGUMENT;
    }
    table_bytes = table_words * sizeof(uint64_t);
    if (table_bytes > KERNEL_ELF_STACK_IMAGE_LIMIT - total_bytes ||
        layout->stack_top < total_bytes + table_bytes) {
        return KERNEL_ELF_IMAGE_STATUS_INVALID_ARGUMENT;
    }
    layout->string_address = layout->stack_top - total_bytes;
    layout->stack_pointer =
        (layout->string_address - table_bytes) & ~UINT64_C(15);
    if (layout->stack_pointer < layout->stack_soft_base)
        return KERNEL_ELF_IMAGE_STATUS_STACK_LIMIT;
    if (layout->stack_top - layout->stack_pointer >
        KERNEL_ELF_STACK_IMAGE_LIMIT) {
        return KERNEL_ELF_IMAGE_STATUS_INVALID_ARGUMENT;
    }
    layout->committed_stack_base =
        align_down_page(layout->stack_pointer - KERNEL_ELF_STACK_HEADROOM);
    if (layout->committed_stack_base < layout->stack_soft_base) {
        layout->committed_stack_base = layout->stack_soft_base;
    }
    layout->random_size = (size_t)random_bytes;
    layout->has_random = random_bytes != 0U;
    layout->executable_address = layout->string_address;
    if (layout->argument_count != 0U) {
        layout->executable_address +=
            (uint64_t)layout->arguments[0].length + 1U;
        for (index = 1U; index < layout->argument_count; index++) {
            layout->executable_address +=
                (uint64_t)layout->arguments[index].length + 1U;
        }
    }
    for (index = 0U; index < layout->environment_count; index++) {
        layout->executable_address +=
            (uint64_t)layout->environment[index].length + 1U;
    }
    layout->random_address = layout->executable_address +
                             (uint64_t)layout->executable->length + 1U;
    return KERNEL_ELF_IMAGE_STATUS_OK;
}

static enum kernel_elf_image_status map_stack_pages(
    struct arch_mmu_user_space *space,
    const struct kernel_elf_layout *layout)
{
    uint64_t address;
    enum arch_mmu_status status;

    for (address = layout->committed_stack_base;
         address < layout->stack_top;
         address += BOAROS_PAGE_SIZE) {
        status = arch_mmu_user_map_zeroed_page(
            space,
            address,
            ARCH_MMU_READ | ARCH_MMU_WRITE);
        if (status != ARCH_MMU_STATUS_OK) {
            return status == ARCH_MMU_STATUS_NO_MEMORY
                       ? KERNEL_ELF_IMAGE_STATUS_NO_MEMORY
                       : KERNEL_ELF_IMAGE_STATUS_ADDRESS_SPACE;
        }
    }
    return KERNEL_ELF_IMAGE_STATUS_OK;
}

static enum kernel_elf_image_status map_vdso_page(
    struct arch_mmu_user_space *space,
    uint64_t address)
{
    enum arch_mmu_status status;

    status = arch_mmu_user_map_zeroed_page(
        space,
        address,
        ARCH_MMU_READ | ARCH_MMU_EXECUTE);
    if (status != ARCH_MMU_STATUS_OK) {
        return status == ARCH_MMU_STATUS_NO_MEMORY
                   ? KERNEL_ELF_IMAGE_STATUS_NO_MEMORY
                   : KERNEL_ELF_IMAGE_STATUS_ADDRESS_SPACE;
    }
    status = arch_mmu_user_space_populate(space,
                                            address,
                                            arch_elf_trampoline_start,
                                            (size_t)(arch_elf_trampoline_end-arch_elf_trampoline_start));
    if (status != ARCH_MMU_STATUS_OK) {
        return KERNEL_ELF_IMAGE_STATUS_ADDRESS_SPACE;
    }
    /* The page is executable as soon as the image is published. */
    arch_mmu_sync_instructions();
    return KERNEL_ELF_IMAGE_STATUS_OK;
}

static enum kernel_elf_image_status populate_bytes(
    struct arch_mmu_user_space *space,
    uint64_t address,
    const void *bytes,
    size_t size)
{
    static const uint8_t terminator = 0U;
    enum arch_mmu_status status;

    if (size != 0U) {
        status = arch_mmu_user_space_populate(space, address, bytes, size);
        if (status != ARCH_MMU_STATUS_OK) {
            return KERNEL_ELF_IMAGE_STATUS_ADDRESS_SPACE;
        }
    }
    status = arch_mmu_user_space_populate(space,
                                            address + size,
                                            &terminator,
                                            sizeof(terminator));
    return status == ARCH_MMU_STATUS_OK
               ? KERNEL_ELF_IMAGE_STATUS_OK
               : KERNEL_ELF_IMAGE_STATUS_ADDRESS_SPACE;
}

static enum kernel_elf_image_status populate_word(
    struct arch_mmu_user_space *space,
    uint64_t address,
    uint64_t value)
{
    return arch_mmu_user_space_populate(space,
                                          address,
                                          &value,
                                          sizeof(value)) ==
                   ARCH_MMU_STATUS_OK
               ? KERNEL_ELF_IMAGE_STATUS_OK
               : KERNEL_ELF_IMAGE_STATUS_ADDRESS_SPACE;
}

static enum kernel_elf_image_status build_stack(
    struct arch_mmu_user_space *space,
    const struct kernel_elf64_source *main_source,
    const struct kernel_elf_layout *layout)
{
    uint64_t word = layout->stack_pointer;
    uint64_t string = layout->string_address;
    uint64_t phdr;
    size_t index;
    enum kernel_elf_image_status status;

    status = populate_word(space, word, layout->argument_count);
    if (status != KERNEL_ELF_IMAGE_STATUS_OK) {
        return status;
    }
    word += sizeof(uint64_t);
    for (index = 0U; index < layout->argument_count; index++) {
        status = populate_word(space, word, string);
        if (status != KERNEL_ELF_IMAGE_STATUS_OK ||
            populate_bytes(space,
                           string,
                           layout->arguments[index].bytes,
                           layout->arguments[index].length) !=
                KERNEL_ELF_IMAGE_STATUS_OK) {
            return KERNEL_ELF_IMAGE_STATUS_ADDRESS_SPACE;
        }
        word += sizeof(uint64_t);
        string += (uint64_t)layout->arguments[index].length + 1U;
    }
    status = populate_word(space, word, 0U);
    if (status != KERNEL_ELF_IMAGE_STATUS_OK) {
        return status;
    }
    word += sizeof(uint64_t);
    for (index = 0U; index < layout->environment_count; index++) {
        status = populate_word(space, word, string);
        if (status != KERNEL_ELF_IMAGE_STATUS_OK ||
            populate_bytes(space,
                           string,
                           layout->environment[index].bytes,
                           layout->environment[index].length) !=
                KERNEL_ELF_IMAGE_STATUS_OK) {
            return KERNEL_ELF_IMAGE_STATUS_ADDRESS_SPACE;
        }
        word += sizeof(uint64_t);
        string += (uint64_t)layout->environment[index].length + 1U;
    }
    status = populate_word(space, word, 0U);
    if (status != KERNEL_ELF_IMAGE_STATUS_OK ||
        populate_bytes(space,
                       layout->executable_address,
                       layout->executable->bytes,
                       layout->executable->length) !=
            KERNEL_ELF_IMAGE_STATUS_OK) {
        return KERNEL_ELF_IMAGE_STATUS_ADDRESS_SPACE;
    }
    word += sizeof(uint64_t);
    if (!source_program_header_address(main_source,
                                       layout->main_bias,
                                       &phdr)) {
        return KERNEL_ELF_IMAGE_STATUS_MALFORMED;
    }
#define POPULATE_AUXILIARY(type, value) \
    do { \
        status = populate_word(space, word, (type)); \
        if (status == KERNEL_ELF_IMAGE_STATUS_OK) { \
            status = populate_word(space, word + sizeof(uint64_t), (value)); \
        } \
        if (status != KERNEL_ELF_IMAGE_STATUS_OK) { \
            return status; \
        } \
        word += 2U * sizeof(uint64_t); \
    } while (0)
    POPULATE_AUXILIARY(6U, BOAROS_PAGE_SIZE);
    POPULATE_AUXILIARY(3U, phdr);
    POPULATE_AUXILIARY(4U, UINT64_C(56));
    POPULATE_AUXILIARY(5U,
                       (uint64_t)kernel_elf64_source_program_header_count(
                           main_source));
    POPULATE_AUXILIARY(7U,
                       layout->has_interpreter
                           ? layout->interpreter_bias
                           : 0U);
    POPULATE_AUXILIARY(8U, 0U);
    POPULATE_AUXILIARY(9U, layout->main_entry);
    POPULATE_AUXILIARY(16U, ARCH_ELF_HWCAP);
    POPULATE_AUXILIARY(17U, KERNEL_ELF_CLKTCK);
    POPULATE_AUXILIARY(11U, 0U);
    POPULATE_AUXILIARY(12U, 0U);
    POPULATE_AUXILIARY(13U, 0U);
    POPULATE_AUXILIARY(14U, 0U);
    POPULATE_AUXILIARY(23U, 0U);
    if (layout->has_random) {
        POPULATE_AUXILIARY(25U, layout->random_address);
    }
    POPULATE_AUXILIARY(31U, layout->executable_address);
    POPULATE_AUXILIARY(0U, 0U);
#undef POPULATE_AUXILIARY
    if (layout->has_random) {
        if (arch_mmu_user_space_populate(space,
                                           layout->random_address,
                                           layout->random_data,
                                           sizeof(layout->random_data)) !=
            ARCH_MMU_STATUS_OK) {
            return KERNEL_ELF_IMAGE_STATUS_ADDRESS_SPACE;
        }
    }
    return KERNEL_ELF_IMAGE_STATUS_OK;
}

static int choose_vdso(const struct kernel_elf_layout *layout,
                       uint64_t *address)
{
    uint64_t starts[2];
    uint64_t ends[2];
    uint32_t blocked_count = 0U;
    uint64_t cursor;
    uint64_t free_pages = 0U;
    uint64_t random_value;
    uint32_t index;
    uint64_t lower = BOAROS_PAGE_SIZE;
    uint64_t upper = layout->mmap_base;

    if (upper <= lower + BOAROS_PAGE_SIZE || address == 0 ||
        !random_pages(KERNEL_ELF_LAYOUT_RANDOM_BITS, &random_value)) {
        return 0;
    }

    if (range_conflict(layout->main_bias,
                       layout->main_end,
                       lower,
                       upper)) {
        starts[blocked_count] = layout->main_bias < lower
                                    ? lower
                                    : layout->main_bias;
        ends[blocked_count] = layout->main_end > upper
                                  ? upper
                                  : layout->main_end;
        if (starts[blocked_count] < ends[blocked_count]) {
            blocked_count++;
        }
    }
    if (layout->has_interpreter &&
        range_conflict(layout->interpreter_bias,
                       layout->interpreter_end,
                       lower,
                       upper)) {
        starts[blocked_count] = layout->interpreter_bias < lower
                                    ? lower
                                    : layout->interpreter_bias;
        ends[blocked_count] = layout->interpreter_end > upper
                                  ? upper
                                  : layout->interpreter_end;
        if (starts[blocked_count] < ends[blocked_count]) {
            blocked_count++;
        }
    }
    if (blocked_count == 2U && starts[1] < starts[0]) {
        uint64_t value = starts[0];

        starts[0] = starts[1];
        starts[1] = value;
        value = ends[0];
        ends[0] = ends[1];
        ends[1] = value;
    }
    cursor = lower;
    for (index = 0U; index < blocked_count; index++) {
        if (starts[index] > cursor) {
            free_pages += (starts[index] - cursor) / BOAROS_PAGE_SIZE;
        }
        if (ends[index] > cursor) {
            cursor = ends[index];
        }
    }
    if (cursor < upper) {
        free_pages += (upper - cursor) / BOAROS_PAGE_SIZE;
    }
    if (free_pages == 0U) {
        return 0;
    }
    random_value %= free_pages;
    cursor = lower;
    for (index = 0U; index < blocked_count; index++) {
        uint64_t gap_pages = starts[index] > cursor
                                 ? (starts[index] - cursor) /
                                       BOAROS_PAGE_SIZE
                                 : 0U;

        if (random_value < gap_pages) {
            *address = cursor + random_value * BOAROS_PAGE_SIZE;
            return 1;
        }
        random_value -= gap_pages;
        if (ends[index] > cursor) {
            cursor = ends[index];
        }
    }
    if (cursor < upper &&
        random_value < (upper - cursor) / BOAROS_PAGE_SIZE) {
        *address = cursor + random_value * BOAROS_PAGE_SIZE;
        return 1;
    }
    return 0;
}

enum kernel_elf_image_status kernel_elf_image_build(
    const struct kernel_exec_image_request *request,
    struct kernel_heap *heap,
    struct physical_page_allocator *allocator,
    const struct arch_mmu_page_table *kernel_table,
    struct kernel_exec_image *image)
{
    struct kernel_elf_layout base = {0};
    struct kernel_elf_layout layout;
    struct arch_mmu_user_space space = {0};
    const struct kernel_elf64_header *main_header;
    const struct kernel_elf64_header *interpreter_header;
    uint64_t main_minimum;
    uint64_t main_maximum;
    uint64_t main_alignment;
    uint64_t interpreter_minimum;
    uint64_t interpreter_maximum;
    uint64_t interpreter_alignment;
    uint64_t random_value;
    uint64_t stack_limit;
    uint8_t stack_random[16];
    uint64_t image_ceiling;
    enum kernel_elf_image_status status;
    enum arch_mmu_status backend_status;
    enum kernel_mm_status mm_status;

    if (request == 0 || request->executable_source == 0 || heap == 0 ||
        allocator == 0 || kernel_table == 0 || image == 0 ||
        image->mm.state != KERNEL_MM_EMPTY || image->arch_private != 0 ||
        kernel_table->allocator != allocator ||
        kernel_table->state != ARCH_MMU_STATE_ACTIVE) {
        return KERNEL_ELF_IMAGE_STATUS_INVALID_ARGUMENT;
    }
    stack_limit = request->stack_limit_valid != 0U
                      ? request->stack_limit : KERNEL_ELF_STACK_RESERVE;
    if (stack_limit > KERNEL_ELF_STACK_RESERVE)
        stack_limit = KERNEL_ELF_STACK_RESERVE;
    /* Initial stack pages and the fault-time limit are page-granular. */
    stack_limit &= ~BOAROS_PAGE_MASK;
    main_header = kernel_elf64_source_header(request->executable_source);
    if (main_header == 0 ||
        main_header->machine != ARCH_ELF_MACHINE ||
        (main_header->type != KERNEL_ELF64_TYPE_EXECUTABLE &&
         main_header->type != KERNEL_ELF64_TYPE_SHARED) ||
        !source_bounds(request->executable_source,
                       &main_minimum,
                       &main_maximum,
                       &main_alignment) ||
        !source_entry_is_executable(request->executable_source,
                                     main_header->entry)) {
        return main_header != 0 &&
                       main_header->machine != ARCH_ELF_MACHINE
                   ? KERNEL_ELF_IMAGE_STATUS_WRONG_ARCH
                   : KERNEL_ELF_IMAGE_STATUS_MALFORMED;
    }
    if (request->interpreter_source != 0) {
        interpreter_header = kernel_elf64_source_header(
            request->interpreter_source);
        if (interpreter_header == 0 ||
            interpreter_header->machine != ARCH_ELF_MACHINE ||
            (interpreter_header->type != KERNEL_ELF64_TYPE_EXECUTABLE &&
             interpreter_header->type != KERNEL_ELF64_TYPE_SHARED) ||
            kernel_elf64_source_interpreter(request->interpreter_source, 0) !=
                0 ||
            !source_bounds(request->interpreter_source,
                           &interpreter_minimum,
                           &interpreter_maximum,
                           &interpreter_alignment) ||
            !source_entry_is_executable(request->interpreter_source,
                                         interpreter_header->entry)) {
            return interpreter_header != 0 &&
                           interpreter_header->machine !=
                               ARCH_ELF_MACHINE
                       ? KERNEL_ELF_IMAGE_STATUS_WRONG_ARCH
                       : KERNEL_ELF_IMAGE_STATUS_MALFORMED;
        }
    } else {
        interpreter_header = 0;
    }
    /* RV64 IALIGN is two bytes; a dynamic linker may later use AT_ENTRY. */
    if ((main_header->entry & UINT64_C(1)) != 0U ||
        (interpreter_header != 0 &&
         (interpreter_header->entry & UINT64_C(1)) != 0U)) {
        return KERNEL_ELF_IMAGE_STATUS_MALFORMED;
    }
    if ((main_header->type == KERNEL_ELF64_TYPE_EXECUTABLE &&
         main_minimum < BOAROS_PAGE_SIZE) ||
        (interpreter_header != 0 &&
         interpreter_header->type == KERNEL_ELF64_TYPE_EXECUTABLE &&
         interpreter_minimum < BOAROS_PAGE_SIZE)) {
        return KERNEL_ELF_IMAGE_STATUS_MALFORMED;
    }
    if (!random_pages(KERNEL_ELF_STACK_RANDOM_BITS, &random_value)) {
        return KERNEL_ELF_IMAGE_STATUS_ADDRESS_SPACE;
    }
    base.stack_top = ARCH_MMU_USER_LIMIT -
                     random_value * BOAROS_PAGE_SIZE;
    if (base.stack_top < KERNEL_ELF_STACK_RESERVE + KERNEL_ELF_STACK_GUARD +
                             BOAROS_PAGE_SIZE) {
        base.stack_top = ARCH_MMU_USER_LIMIT -
                         KERNEL_ELF_STACK_RESERVE - KERNEL_ELF_STACK_GUARD;
    }
    base.stack_base = base.stack_top - KERNEL_ELF_STACK_RESERVE;
    base.stack_soft_base = base.stack_top - stack_limit;
    base.stack_guard = base.stack_base - KERNEL_ELF_STACK_GUARD;
    if (base.stack_guard <= BOAROS_PAGE_SIZE + KERNEL_ELF_MMAP_GAP) {
        return KERNEL_ELF_IMAGE_STATUS_ADDRESS_SPACE;
    }
    if (!random_pages(KERNEL_ELF_LAYOUT_RANDOM_BITS, &random_value) ||
        random_value > (base.stack_guard - KERNEL_ELF_MMAP_GAP) /
                           BOAROS_PAGE_SIZE) {
        random_value = 0U;
    }
    base.mmap_base = base.stack_guard - KERNEL_ELF_MMAP_GAP -
                     random_value * BOAROS_PAGE_SIZE;
    /* Leave a nonempty heap interval and room for an ET_DYN interpreter.
     * Independent random draws must select legal holes, not occasionally
     * consume the space required by the next part of the same image. */
    image_ceiling = base.mmap_base - BOAROS_PAGE_SIZE;
    if (interpreter_header != 0 &&
        interpreter_header->type == KERNEL_ELF64_TYPE_SHARED) {
        uint64_t reserve = interpreter_maximum - interpreter_minimum;
        if (reserve > UINT64_MAX - interpreter_alignment ||
            reserve + interpreter_alignment >= image_ceiling)
            return KERNEL_ELF_IMAGE_STATUS_ADDRESS_SPACE;
        image_ceiling -= reserve + interpreter_alignment;
    }
    if (main_header->type == KERNEL_ELF64_TYPE_EXECUTABLE) {
        base.main_bias = 0U;
    } else if (!choose_pie_bias(request->executable_source,
                                image_ceiling,
                                &base.main_bias)) {
        return KERNEL_ELF_IMAGE_STATUS_ADDRESS_SPACE;
    }
    if (!source_highest_end(request->executable_source,
                            base.main_bias,
                            &base.main_end) ||
        base.main_end >= base.stack_guard) {
        return KERNEL_ELF_IMAGE_STATUS_ADDRESS_SPACE;
    }
    base.has_interpreter = request->interpreter_source != 0;
    if (base.has_interpreter) {
        if (interpreter_header->type == KERNEL_ELF64_TYPE_EXECUTABLE) {
            base.interpreter_bias = 0U;
        } else if (!choose_interpreter_bias(request->interpreter_source,
                                            base.mmap_base,
                                            base.main_end + BOAROS_PAGE_SIZE,
                                            &base.interpreter_bias)) {
            return KERNEL_ELF_IMAGE_STATUS_ADDRESS_SPACE;
        }
        if (!source_highest_end(request->interpreter_source,
                                base.interpreter_bias,
                                &base.interpreter_end) ||
            base.interpreter_end >= base.stack_guard ||
            range_conflict(base.main_bias,
                           base.main_end,
                           base.interpreter_bias,
                           base.interpreter_end)) {
            return KERNEL_ELF_IMAGE_STATUS_ADDRESS_SPACE;
        }
        if (base.interpreter_bias > UINT64_MAX -
                                       interpreter_header->entry) {
            return KERNEL_ELF_IMAGE_STATUS_MALFORMED;
        }
        base.interpreter_entry = base.interpreter_bias +
                                 interpreter_header->entry;
    }
    if (base.main_bias > UINT64_MAX - main_header->entry) {
        return KERNEL_ELF_IMAGE_STATUS_MALFORMED;
    }
    base.main_entry = base.main_bias + main_header->entry;
    if (base.main_entry >= ARCH_MMU_USER_LIMIT ||
        (base.has_interpreter &&
         base.interpreter_entry >= ARCH_MMU_USER_LIMIT) ||
        !source_program_header_address(request->executable_source,
                                       base.main_bias,
                                       &base.main_phdr)) {
        return KERNEL_ELF_IMAGE_STATUS_ADDRESS_SPACE;
    }
    if ((main_header->type == KERNEL_ELF64_TYPE_SHARED ||
         base.has_interpreter) &&
        base.main_phdr == 0U) {
        return KERNEL_ELF_IMAGE_STATUS_MALFORMED;
    }
    base.brk_limit = base.mmap_base;
    if (base.has_interpreter &&
        base.interpreter_bias + interpreter_minimum > base.main_end &&
        base.interpreter_bias + interpreter_minimum < base.brk_limit)
        base.brk_limit = base.interpreter_bias + interpreter_minimum;
    if (base.main_end >= base.brk_limit)
        return KERNEL_ELF_IMAGE_STATUS_ADDRESS_SPACE;
    if (!random_pages(18U, &random_value)) {
        return KERNEL_ELF_IMAGE_STATUS_ADDRESS_SPACE;
    }
    random_value %= (base.brk_limit - base.main_end - 1U) /
                         BOAROS_PAGE_SIZE + 1U;
    if (base.main_end > UINT64_MAX - random_value * BOAROS_PAGE_SIZE) {
        return KERNEL_ELF_IMAGE_STATUS_ADDRESS_SPACE;
    }
    base.brk_start = align_down_page(
        base.main_end + random_value * BOAROS_PAGE_SIZE);
    if (base.brk_start < base.main_end) {
        base.brk_start = align_up_page(base.main_end, &base.brk_start)
                             ? base.brk_start
                             : 0U;
    }
    /* Keep the growing brk interval below the independent mmap window. */
    if (base.brk_start < BOAROS_PAGE_SIZE ||
        base.brk_start >= base.brk_limit ||
        base.mmap_base <= base.brk_start) {
        return KERNEL_ELF_IMAGE_STATUS_ADDRESS_SPACE;
    }
    if (!choose_vdso(&base, &base.vdso)) {
        return KERNEL_ELF_IMAGE_STATUS_ADDRESS_SPACE;
    }
    /* Keep the heap below the independently selected VDSO when necessary. */
    if (base.vdso > base.brk_start && base.vdso < base.brk_limit) {
        base.brk_limit = base.vdso;
    }
    status = prepare_stack_layout(request, &base, &layout);
    if (status != KERNEL_ELF_IMAGE_STATUS_OK) {
        return status;
    }
    if (layout.has_random &&
        kernel_random_fill(stack_random, sizeof(stack_random)) !=
            KERNEL_RANDOM_STATUS_OK) {
        return KERNEL_ELF_IMAGE_STATUS_ADDRESS_SPACE;
    }
    backend_status = arch_mmu_user_space_init(&space,
                                             allocator,
                                             kernel_table);
    if (backend_status != ARCH_MMU_STATUS_OK) {
        return backend_status == ARCH_MMU_STATUS_NO_MEMORY
                   ? KERNEL_ELF_IMAGE_STATUS_NO_MEMORY
                   : KERNEL_ELF_IMAGE_STATUS_ADDRESS_SPACE;
    }
    status = map_stack_pages(&space, &layout);
    if (status == KERNEL_ELF_IMAGE_STATUS_OK) {
        status = map_vdso_page(&space, layout.vdso);
    }
    if (status != KERNEL_ELF_IMAGE_STATUS_OK) {
        (void)cleanup_space(&image->mm, &space);
        return status;
    }
    if (layout.has_random) {
        for (uint32_t random_index = 0U;
             random_index < sizeof(stack_random);
             random_index++) {
            layout.random_data[random_index] = stack_random[random_index];
        }
    }
    status = build_stack(&space, request->executable_source, &layout);
    if (status != KERNEL_ELF_IMAGE_STATUS_OK) {
        (void)cleanup_space(&image->mm, &space);
        return status;
    }
    mm_status = kernel_mm_create(&image->mm, &space);
    if (mm_status != KERNEL_MM_STATUS_OK) {
        (void)cleanup_space(&image->mm, &space);
        return mm_status == KERNEL_MM_STATUS_NO_MEMORY
                   ? KERNEL_ELF_IMAGE_STATUS_NO_MEMORY
                   : KERNEL_ELF_IMAGE_STATUS_ADDRESS_SPACE;
    }
    mm_status = kernel_mm_vma_enable(&image->mm, heap);
    if (mm_status == KERNEL_MM_STATUS_OK) {
        mm_status = kernel_mm_brk_initialize(&image->mm,
                                             layout.brk_start,
                                             layout.brk_limit);
    }
    if (mm_status == KERNEL_MM_STATUS_OK) {
        mm_status = kernel_mm_vma_insert_anon(
            &image->mm,
            layout.stack_base,
            layout.stack_top,
            KERNEL_MM_READ | KERNEL_MM_WRITE,
            KERNEL_VMA_ROLE_STACK,
            KERNEL_VMA_FAULT_DEMAND_ZERO);
    }
    if (mm_status == KERNEL_MM_STATUS_OK) {
        mm_status = kernel_mm_vma_insert_anon(
            &image->mm,
            layout.vdso,
            layout.vdso + BOAROS_PAGE_SIZE,
            KERNEL_MM_READ | KERNEL_MM_EXECUTE,
            KERNEL_VMA_ROLE_VDSO,
            KERNEL_VMA_FAULT_RESIDENT_REQUIRED);
    }
    if (mm_status == KERNEL_MM_STATUS_OK) {
        mm_status = kernel_mm_map_elf_source(&image->mm,
                                             request->executable_source,
                                             layout.main_bias);
    }
    if (mm_status == KERNEL_MM_STATUS_OK && request->interpreter_source != 0) {
        mm_status = kernel_mm_map_elf_source(&image->mm,
                                             request->interpreter_source,
                                             layout.interpreter_bias);
    }
    if (mm_status == KERNEL_MM_STATUS_OK) {
        mm_status = kernel_mm_mmap_base_initialize(&image->mm,
                                                   layout.mmap_base);
    }
    if (mm_status == KERNEL_MM_STATUS_OK) {
        mm_status = kernel_mm_vdso_set_address(&image->mm, layout.vdso);
    }
    if (mm_status == KERNEL_MM_STATUS_OK &&
        kernel_elf64_source_file(request->executable_source)) {
        mm_status = kernel_mm_set_executable(&image->mm,
            kernel_elf64_source_file(request->executable_source));
    }
    if (mm_status == KERNEL_MM_STATUS_OK) {
        uint64_t start_code = UINT64_MAX, end_code = 0U;
        uint16_t count = kernel_elf64_source_program_header_count(
            request->executable_source);
        /* 固定主 ELF 的文件字节边界；解释器和后续 mprotect 不改初始布局。 */
        for (uint16_t index = 0; index < count; index++) {
            const struct kernel_elf64_program_header *segment =
                kernel_elf64_source_program_header(request->executable_source,
                                                   index);
            if (segment->type != KERNEL_ELF64_PROGRAM_LOAD ||
                !(segment->flags & KERNEL_ELF64_FLAG_EXECUTE)) continue;
            uint64_t start = segment->virtual_address + layout.main_bias;
            uint64_t end = start + segment->file_size;
            if (start < start_code) start_code = start;
            if (end > end_code) end_code = end;
        }
        mm_status = kernel_mm_set_exec_layout(&image->mm,
            start_code == UINT64_MAX ? 0U : start_code, end_code,
            layout.stack_pointer);
    }
    if (mm_status != KERNEL_MM_STATUS_OK) {
        status = cleanup_space(&image->mm, &space);
        return status == KERNEL_ELF_IMAGE_STATUS_OK
                   ? KERNEL_ELF_IMAGE_STATUS_ADDRESS_SPACE
                   : status;
    }
    image->entry = (uintptr_t)(layout.has_interpreter
                                   ? layout.interpreter_entry
                                   : layout.main_entry);
    image->stack_pointer = (uintptr_t)layout.stack_pointer;
    image->thread_pointer = 0U;
    return KERNEL_ELF_IMAGE_STATUS_OK;
}
