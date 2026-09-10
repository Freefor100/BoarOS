#include <kernel/elf64.h>
#include <kernel/random.h>
#include <kernel/read_source.h>

#include <stddef.h>
#include <stdint.h>

#define ELF_HEADER_SIZE 64U
#define PROGRAM_HEADER_SIZE 56U
#define IMAGE_SIZE 0x180U
#define LOAD_OFFSET UINT64_C(0x100)
#define LOAD_VIRTUAL_ADDRESS UINT64_C(0x10100)

struct test_read_context {
    const unsigned char *bytes;
    uint64_t size;
    uint64_t fail_at;
    size_t calls;
    size_t largest_read;
};

static int test_read_at(void *context,
                        uint64_t offset,
                        void *buffer,
                        size_t size)
{
    struct test_read_context *reader = context;
    unsigned char *destination = buffer;
    size_t index;

    reader->calls++;
    if (size > reader->largest_read) {
        reader->largest_read = size;
    }
    if (offset >= reader->fail_at || offset > reader->size ||
        (uint64_t)size > reader->size - offset) {
        return -5;
    }
    for (index = 0U; index < size; index++) {
        destination[index] = reader->bytes[offset + index];
    }
    return 0;
}

static enum kernel_elf64_status open_memory(
    const void *bytes,
    size_t size,
    struct kernel_elf64_image *image)
{
    struct kernel_read_source source;

    if (kernel_read_source_from_memory(bytes, size, &source) != 0) {
        return KERNEL_ELF64_STATUS_INVALID_ARGUMENT;
    }
    return kernel_elf64_open(&source, image);
}

static void put_u16(unsigned char *bytes, size_t offset, uint16_t value)
{
    bytes[offset] = (unsigned char)value;
    bytes[offset + 1U] = (unsigned char)(value >> 8U);
}

static void put_u32(unsigned char *bytes, size_t offset, uint32_t value)
{
    uint32_t index;

    for (index = 0U; index < 4U; index++) {
        bytes[offset + index] = (unsigned char)(value >> (index * 8U));
    }
}

static void put_u64(unsigned char *bytes, size_t offset, uint64_t value)
{
    uint32_t index;

    for (index = 0U; index < 8U; index++) {
        bytes[offset + index] = (unsigned char)(value >> (index * 8U));
    }
}

static void clear_bytes(unsigned char *bytes, size_t size)
{
    size_t index;

    for (index = 0U; index < size; index++) {
        bytes[index] = 0U;
    }
}

static void make_valid_image(unsigned char *bytes)
{
    size_t ph = ELF_HEADER_SIZE;

    clear_bytes(bytes, IMAGE_SIZE);
    bytes[0] = 0x7fU;
    bytes[1] = 'E';
    bytes[2] = 'L';
    bytes[3] = 'F';
    bytes[4] = KERNEL_ELF64_CLASS_64;
    bytes[5] = KERNEL_ELF64_DATA_LITTLE_ENDIAN;
    bytes[6] = KERNEL_ELF64_VERSION_CURRENT;
    put_u16(bytes, 16U, KERNEL_ELF64_TYPE_EXECUTABLE);
    put_u16(bytes, 18U, KERNEL_ELF64_MACHINE_RISCV);
    put_u32(bytes, 20U, KERNEL_ELF64_VERSION_CURRENT);
    put_u64(bytes, 24U, LOAD_VIRTUAL_ADDRESS);
    put_u64(bytes, 32U, ELF_HEADER_SIZE);
    put_u16(bytes, 52U, ELF_HEADER_SIZE);
    put_u16(bytes, 54U, PROGRAM_HEADER_SIZE);
    put_u16(bytes, 56U, 1U);

    put_u32(bytes, ph, KERNEL_ELF64_PROGRAM_LOAD);
    put_u32(bytes, ph + 4U,
            KERNEL_ELF64_FLAG_READ | KERNEL_ELF64_FLAG_EXECUTE);
    put_u64(bytes, ph + 8U, LOAD_OFFSET);
    put_u64(bytes, ph + 16U, LOAD_VIRTUAL_ADDRESS);
    put_u64(bytes, ph + 24U, LOAD_VIRTUAL_ADDRESS);
    put_u64(bytes, ph + 32U, 4U);
    put_u64(bytes, ph + 40U, 8U);
    put_u64(bytes, ph + 48U, 0x100U);
    bytes[LOAD_OFFSET] = 0x13U;
    bytes[LOAD_OFFSET + 1U] = 0U;
    bytes[LOAD_OFFSET + 2U] = 0U;
    bytes[LOAD_OFFSET + 3U] = 0U;
}

static unsigned long image_changed(const struct kernel_elf64_image *image)
{
    return (image->source.context != (void *)(uintptr_t)0x11U ? 1U : 0U) |
           (image->source.size != 0x22U ? 2U : 0U) |
           (image->source.read_at !=
                (kernel_read_at_fn)(uintptr_t)0x33U ? 4U : 0U) |
           (image->header.type != 0x33U ? 8U : 0U) |
           (image->header.machine != 0x44U ? 16U : 0U) |
           (image->header.entry != UINT64_C(0x55) ? 32U : 0U) |
           (image->header.program_header_offset != UINT64_C(0x66) ? 64U : 0U) |
           (image->header.program_header_count != 0x77U ? 128U : 0U);
}

static struct kernel_elf64_image sentinel_image(void)
{
    struct kernel_elf64_image image = {
        .source = {
            .context = (void *)(uintptr_t)0x11U,
            .size = 0x22U,
            .read_at = (kernel_read_at_fn)(uintptr_t)0x33U,
        },
        .header = {
            .type = 0x33U,
            .machine = 0x44U,
            .entry = UINT64_C(0x55),
            .program_header_offset = UINT64_C(0x66),
            .program_header_count = 0x77U,
        },
    };

    return image;
}

static unsigned long run_valid_case(void)
{
    unsigned char bytes[IMAGE_SIZE];
    struct kernel_elf64_image image;
    struct kernel_elf64_program_header header;
    struct kernel_read_source source;

    make_valid_image(bytes);
    if (kernel_read_source_from_memory(bytes, sizeof(bytes), &source) != 0 ||
        kernel_elf64_open(&source, &image) !=
            KERNEL_ELF64_STATUS_OK ||
        image.source.context != bytes ||
        image.source.size != sizeof(bytes) ||
        image.header.type != KERNEL_ELF64_TYPE_EXECUTABLE ||
        image.header.machine != KERNEL_ELF64_MACHINE_RISCV ||
        image.header.entry != LOAD_VIRTUAL_ADDRESS ||
        image.header.program_header_offset != ELF_HEADER_SIZE ||
        image.header.program_header_count != 1U ||
        kernel_elf64_read_program_header(&image, 0U, &header) !=
            KERNEL_ELF64_STATUS_OK ||
        header.type != KERNEL_ELF64_PROGRAM_LOAD ||
        header.flags !=
            (KERNEL_ELF64_FLAG_READ | KERNEL_ELF64_FLAG_EXECUTE) ||
        header.offset != LOAD_OFFSET ||
        header.virtual_address != LOAD_VIRTUAL_ADDRESS ||
        header.physical_address != LOAD_VIRTUAL_ADDRESS ||
        header.file_size != 4U || header.memory_size != 8U ||
        header.alignment != 0x100U) {
        return 1U;
    }
    return 0U;
}

static unsigned long expect_open_status(unsigned char *bytes,
                                        size_t size,
                                        enum kernel_elf64_status expected)
{
    struct kernel_elf64_image image = sentinel_image();

    if (open_memory(bytes, size, &image) != expected ||
        image_changed(&image)) {
        return 1U;
    }
    return 0U;
}

static unsigned long run_argument_and_truncation_cases(void)
{
    unsigned char bytes[IMAGE_SIZE];
    struct kernel_elf64_image image = sentinel_image();
    unsigned long failures = 0U;

    make_valid_image(bytes);
    if (open_memory(0, sizeof(bytes), &image) !=
            KERNEL_ELF64_STATUS_INVALID_ARGUMENT ||
        image_changed(&image) ||
        open_memory(bytes, sizeof(bytes), 0) !=
            KERNEL_ELF64_STATUS_INVALID_ARGUMENT) {
        failures++;
    }
    failures += expect_open_status(bytes, ELF_HEADER_SIZE - 1U,
                                   KERNEL_ELF64_STATUS_TRUNCATED);
    failures += expect_open_status(bytes,
                                   ELF_HEADER_SIZE + PROGRAM_HEADER_SIZE - 1U,
                                   KERNEL_ELF64_STATUS_TRUNCATED);
    failures += expect_open_status(bytes, LOAD_OFFSET + 3U,
                                   KERNEL_ELF64_STATUS_TRUNCATED);
    return failures;
}

static unsigned long run_identification_cases(void)
{
    unsigned char bytes[IMAGE_SIZE];
    unsigned long failures = 0U;

    make_valid_image(bytes);
    bytes[0] = 0U;
    failures += expect_open_status(bytes, sizeof(bytes),
                                   KERNEL_ELF64_STATUS_MALFORMED);
    make_valid_image(bytes);
    bytes[4] = 1U;
    failures += expect_open_status(bytes, sizeof(bytes),
                                   KERNEL_ELF64_STATUS_UNSUPPORTED);
    make_valid_image(bytes);
    bytes[5] = 2U;
    failures += expect_open_status(bytes, sizeof(bytes),
                                   KERNEL_ELF64_STATUS_UNSUPPORTED);
    make_valid_image(bytes);
    bytes[6] = 0U;
    failures += expect_open_status(bytes, sizeof(bytes),
                                   KERNEL_ELF64_STATUS_UNSUPPORTED);
    make_valid_image(bytes);
    put_u32(bytes, 20U, 0U);
    failures += expect_open_status(bytes, sizeof(bytes),
                                   KERNEL_ELF64_STATUS_UNSUPPORTED);
    return failures;
}

static unsigned long run_header_table_cases(void)
{
    unsigned char bytes[IMAGE_SIZE];
    unsigned long failures = 0U;

    make_valid_image(bytes);
    put_u16(bytes, 52U, ELF_HEADER_SIZE - 1U);
    failures += expect_open_status(bytes, sizeof(bytes),
                                   KERNEL_ELF64_STATUS_MALFORMED);
    make_valid_image(bytes);
    put_u16(bytes, 54U, PROGRAM_HEADER_SIZE - 1U);
    failures += expect_open_status(bytes, sizeof(bytes),
                                   KERNEL_ELF64_STATUS_MALFORMED);
    make_valid_image(bytes);
    put_u16(bytes, 56U, 0U);
    failures += expect_open_status(bytes, sizeof(bytes),
                                   KERNEL_ELF64_STATUS_MALFORMED);
    make_valid_image(bytes);
    put_u16(bytes, 56U, KERNEL_ELF64_MAX_PROGRAM_HEADERS + 1U);
    failures += expect_open_status(bytes, sizeof(bytes),
                                   KERNEL_ELF64_STATUS_UNSUPPORTED);
    make_valid_image(bytes);
    put_u64(bytes, 32U, ELF_HEADER_SIZE - 1U);
    failures += expect_open_status(bytes, sizeof(bytes),
                                   KERNEL_ELF64_STATUS_MALFORMED);
    make_valid_image(bytes);
    put_u64(bytes, 32U, UINT64_MAX - 8U);
    failures += expect_open_status(bytes, sizeof(bytes),
                                   KERNEL_ELF64_STATUS_TRUNCATED);
    return failures;
}

static unsigned long run_load_segment_cases(void)
{
    unsigned char bytes[IMAGE_SIZE];
    size_t ph = ELF_HEADER_SIZE;
    unsigned long failures = 0U;

    make_valid_image(bytes);
    put_u64(bytes, ph + 32U, 9U);
    put_u64(bytes, ph + 40U, 8U);
    failures += expect_open_status(bytes, sizeof(bytes),
                                   KERNEL_ELF64_STATUS_MALFORMED);
    make_valid_image(bytes);
    put_u64(bytes, ph + 8U, UINT64_MAX - 1U);
    put_u64(bytes, ph + 32U, 4U);
    failures += expect_open_status(bytes, sizeof(bytes),
                                   KERNEL_ELF64_STATUS_TRUNCATED);
    make_valid_image(bytes);
    put_u64(bytes, ph + 16U, UINT64_MAX - 3U);
    put_u64(bytes, ph + 40U, 8U);
    failures += expect_open_status(bytes, sizeof(bytes),
                                   KERNEL_ELF64_STATUS_MALFORMED);
    make_valid_image(bytes);
    put_u64(bytes, ph + 48U, 3U);
    failures += expect_open_status(bytes, sizeof(bytes),
                                   KERNEL_ELF64_STATUS_MALFORMED);
    make_valid_image(bytes);
    put_u64(bytes, ph + 48U, 0x1000U);
    put_u64(bytes, ph + 16U, LOAD_VIRTUAL_ADDRESS + 0x100U);
    failures += expect_open_status(bytes, sizeof(bytes),
                                   KERNEL_ELF64_STATUS_MALFORMED);
    return failures;
}

static unsigned long run_program_header_argument_cases(void)
{
    unsigned char bytes[IMAGE_SIZE];
    struct kernel_elf64_image image;
    struct kernel_elf64_image malformed;
    struct kernel_elf64_program_header header = {
        .type = 0x11U,
        .flags = 0x22U,
        .offset = UINT64_C(0x33),
    };

    make_valid_image(bytes);
    if (open_memory(bytes, sizeof(bytes), &image) !=
        KERNEL_ELF64_STATUS_OK) {
        return 1U;
    }
    if (kernel_elf64_read_program_header(0, 0U, &header) !=
            KERNEL_ELF64_STATUS_INVALID_ARGUMENT ||
        kernel_elf64_read_program_header(&image, 0U, 0) !=
            KERNEL_ELF64_STATUS_INVALID_ARGUMENT ||
        kernel_elf64_read_program_header(&image, 1U, &header) !=
            KERNEL_ELF64_STATUS_INVALID_ARGUMENT ||
        header.type != 0x11U || header.flags != 0x22U ||
        header.offset != UINT64_C(0x33)) {
        return 1U;
    }

    malformed = image;
    malformed.header.program_header_offset = UINT64_MAX - 8U;
    malformed.header.program_header_count = 2U;
    if (kernel_elf64_read_program_header(&malformed, 1U, &header) !=
            KERNEL_ELF64_STATUS_TRUNCATED ||
        header.type != 0x11U || header.flags != 0x22U ||
        header.offset != UINT64_C(0x33)) {
        return 1U;
    }
    return 0U;
}

static unsigned long run_read_source_cases(void)
{
    unsigned char bytes[IMAGE_SIZE];
    struct test_read_context context;
    struct kernel_read_source source;
    struct kernel_elf64_image image = sentinel_image();

    make_valid_image(bytes);
    context.bytes = bytes;
    context.size = sizeof(bytes);
    context.fail_at = UINT64_MAX;
    context.calls = 0U;
    context.largest_read = 0U;
    source.context = &context;
    source.size = sizeof(bytes);
    source.read_at = test_read_at;
    if (kernel_elf64_open(&source, &image) != KERNEL_ELF64_STATUS_OK ||
        context.calls != 2U || context.largest_read > ELF_HEADER_SIZE) {
        return 1U;
    }

    image = sentinel_image();
    context.fail_at = ELF_HEADER_SIZE;
    context.calls = 0U;
    if (kernel_elf64_open(&source, &image) != KERNEL_ELF64_STATUS_IO ||
        context.calls != 2U || image_changed(&image)) {
        return 1U;
    }
    return 0U;
}

static unsigned long run_chacha20_vector_case(void)
{
    static const uint8_t expected[64] = {
        0x10U, 0xf1U, 0xe7U, 0xe4U, 0xd1U, 0x3bU, 0x59U, 0x15U,
        0x50U, 0x0fU, 0xddU, 0x1fU, 0xa3U, 0x20U, 0x71U, 0xc4U,
        0xc7U, 0xd1U, 0xf4U, 0xc7U, 0x33U, 0xc0U, 0x68U, 0x03U,
        0x04U, 0x22U, 0xaaU, 0x9aU, 0xc3U, 0xd4U, 0x6cU, 0x4eU,
        0xd2U, 0x82U, 0x64U, 0x46U, 0x07U, 0x9fU, 0xaaU, 0x09U,
        0x14U, 0xc2U, 0xd7U, 0x05U, 0xd9U, 0x8bU, 0x02U, 0xa2U,
        0xb5U, 0x12U, 0x9cU, 0xd1U, 0xdeU, 0x16U, 0x4eU, 0xb9U,
        0xcbU, 0xd0U, 0x83U, 0xe8U, 0xa2U, 0x50U, 0x3cU, 0x4eU,
    };
    uint8_t key[32];
    uint8_t nonce[12] = {
        0x00U, 0x00U, 0x00U, 0x09U, 0x00U, 0x00U,
        0x00U, 0x4aU, 0x00U, 0x00U, 0x00U, 0x00U,
    };
    uint8_t output[64];
    uint32_t index;

    for (index = 0U; index < sizeof(key); index++) {
        key[index] = (uint8_t)index;
    }
    kernel_random_chacha20_block(key, nonce, 1U, output);
    for (index = 0U; index < sizeof(output); index++) {
        if (output[index] != expected[index]) {
            return 1U;
        }
    }
    return 0U;
}

unsigned long run_elf64_cases(void)
{
    unsigned long failures = run_valid_case();

    failures += run_argument_and_truncation_cases();
    failures += run_identification_cases();
    failures += run_header_table_cases();
    failures += run_load_segment_cases();
    failures += run_program_header_argument_cases();
    failures += run_read_source_cases();
    failures += run_chacha20_vector_case();
    return failures;
}
