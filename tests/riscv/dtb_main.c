#include <arch/riscv/sbi.h>
#include <arch/riscv/virt_uart.h>
#include <kernel/dtb.h>

#include <stdint.h>

#define TEST_BLOB_CAPACITY 1024U
#define TEST_RESERVE_OFFSET 40U
#define TEST_STRUCTURE_OFFSET 56U

#define FDT_MAGIC 0xd00dfeedU
#define FDT_BEGIN_NODE 1U
#define FDT_END_NODE 2U
#define FDT_PROP 3U
#define FDT_END 9U

#define FDT_HEADER_MAGIC 0U
#define FDT_HEADER_TOTAL_SIZE 4U
#define FDT_HEADER_STRUCT_OFFSET 8U
#define FDT_HEADER_STRINGS_OFFSET 12U
#define FDT_HEADER_RESERVE_OFFSET 16U
#define FDT_HEADER_VERSION 20U
#define FDT_HEADER_LAST_COMPATIBLE_VERSION 24U
#define FDT_HEADER_STRINGS_SIZE 32U
#define FDT_HEADER_STRUCT_SIZE 36U

enum property_name_offset {
    NAME_ADDRESS_CELLS = 0U,
    NAME_SIZE_CELLS = sizeof("#address-cells"),
    NAME_DEVICE_TYPE = sizeof("#address-cells") + sizeof("#size-cells"),
    NAME_REG = sizeof("#address-cells") + sizeof("#size-cells") +
               sizeof("device_type"),
};

static const unsigned char property_names[] =
    "#address-cells\0#size-cells\0device_type\0reg";

struct blob_builder {
    unsigned char bytes[TEST_BLOB_CAPACITY];
    uint32_t position;
};

static struct blob_builder test_blob;

static void write_be32(unsigned char *bytes, uint32_t value)
{
    bytes[0] = (unsigned char)(value >> 24);
    bytes[1] = (unsigned char)(value >> 16);
    bytes[2] = (unsigned char)(value >> 8);
    bytes[3] = (unsigned char)value;
}

static void builder_append_byte(struct blob_builder *builder,
                                unsigned char value)
{
    builder->bytes[builder->position] = value;
    builder->position++;
}

static void builder_append_u32(struct blob_builder *builder, uint32_t value)
{
    write_be32(builder->bytes + builder->position, value);
    builder->position += 4U;
}

static void builder_align_u32(struct blob_builder *builder)
{
    while ((builder->position & 3U) != 0U) {
        builder_append_byte(builder, 0U);
    }
}

static void builder_append_c_string(struct blob_builder *builder,
                                    const char *text)
{
    do {
        builder_append_byte(builder, (unsigned char)*text);
    } while (*text++ != '\0');

    builder_align_u32(builder);
}

static void builder_begin_node(struct blob_builder *builder, const char *name)
{
    builder_append_u32(builder, FDT_BEGIN_NODE);
    builder_append_c_string(builder, name);
}

static void builder_end_node(struct blob_builder *builder)
{
    builder_append_u32(builder, FDT_END_NODE);
}

static void builder_property(struct blob_builder *builder,
                             uint32_t name_offset,
                             const unsigned char *value,
                             uint32_t length)
{
    uint32_t index;

    builder_append_u32(builder, FDT_PROP);
    builder_append_u32(builder, length);
    builder_append_u32(builder, name_offset);
    for (index = 0U; index < length; index++) {
        builder_append_byte(builder, value[index]);
    }
    builder_align_u32(builder);
}

static void builder_property_u32(struct blob_builder *builder,
                                 uint32_t name_offset,
                                 uint32_t value)
{
    unsigned char encoded[4];

    write_be32(encoded, value);
    builder_property(builder, name_offset, encoded, sizeof(encoded));
}

static void builder_property_string(struct blob_builder *builder,
                                    uint32_t name_offset,
                                    const char *value,
                                    int include_terminator)
{
    uint32_t length = 0U;

    while (value[length] != '\0') {
        length++;
    }
    if (include_terminator) {
        length++;
    }

    builder_property(builder,
                     name_offset,
                     (const unsigned char *)value,
                     length);
}

static void builder_property_cells(struct blob_builder *builder,
                                   const uint32_t *cells,
                                   uint32_t cell_count)
{
    unsigned char encoded[32];
    uint32_t index;

    for (index = 0U; index < cell_count; index++) {
        write_be32(encoded + index * 4U, cells[index]);
    }

    builder_property(builder, NAME_REG, encoded, cell_count * 4U);
}

static void builder_start(struct blob_builder *builder)
{
    uint32_t index;

    for (index = 0U; index < TEST_BLOB_CAPACITY; index++) {
        builder->bytes[index] = 0U;
    }

    builder->position = TEST_STRUCTURE_OFFSET;
    builder_begin_node(builder, "");
}

static void builder_finish(struct blob_builder *builder)
{
    uint32_t structure_size;
    uint32_t strings_offset;
    uint32_t index;

    builder_end_node(builder);
    builder_append_u32(builder, FDT_END);
    structure_size = builder->position - TEST_STRUCTURE_OFFSET;
    strings_offset = builder->position;

    for (index = 0U; index < sizeof(property_names); index++) {
        builder_append_byte(builder, property_names[index]);
    }

    write_be32(builder->bytes + FDT_HEADER_MAGIC, FDT_MAGIC);
    write_be32(builder->bytes + FDT_HEADER_TOTAL_SIZE, builder->position);
    write_be32(builder->bytes + FDT_HEADER_STRUCT_OFFSET,
               TEST_STRUCTURE_OFFSET);
    write_be32(builder->bytes + FDT_HEADER_STRINGS_OFFSET, strings_offset);
    write_be32(builder->bytes + FDT_HEADER_RESERVE_OFFSET,
               TEST_RESERVE_OFFSET);
    write_be32(builder->bytes + FDT_HEADER_VERSION, 17U);
    write_be32(builder->bytes + FDT_HEADER_LAST_COMPATIBLE_VERSION, 16U);
    write_be32(builder->bytes + FDT_HEADER_STRINGS_SIZE,
               sizeof(property_names));
    write_be32(builder->bytes + FDT_HEADER_STRUCT_SIZE, structure_size);
}

static void builder_add_root_cells(struct blob_builder *builder,
                                   uint32_t address_cells,
                                   uint32_t size_cells)
{
    builder_property_u32(builder, NAME_ADDRESS_CELLS, address_cells);
    builder_property_u32(builder, NAME_SIZE_CELLS, size_cells);
}

static void builder_add_memory(struct blob_builder *builder,
                               const char *name,
                               const uint32_t *reg_cells,
                               uint32_t reg_cell_count)
{
    builder_begin_node(builder, name);
    builder_property_string(builder, NAME_DEVICE_TYPE, "memory", 1);
    builder_property_cells(builder, reg_cells, reg_cell_count);
    builder_end_node(builder);
}

static void fail_status(unsigned long case_id,
                        enum dtb_status expected,
                        enum dtb_status actual) __attribute__((noreturn));

static void fail_status(unsigned long case_id,
                        enum dtb_status expected,
                        enum dtb_status actual)
{
    virt_uart_puts("BoarOS: DTB parser test failed case=");
    virt_uart_put_hex(case_id);
    virt_uart_puts(" expected=");
    virt_uart_put_hex((unsigned long)expected);
    virt_uart_puts(" actual=");
    virt_uart_put_hex((unsigned long)actual);
    virt_uart_putc('\n');
    sbi_shutdown();
}

static void expect_error(unsigned long case_id, enum dtb_status expected)
{
    struct dtb_memory_range range = {
        .base = 0x1122334455667788ULL,
        .size = 0x8877665544332211ULL,
    };
    enum dtb_status actual =
        dtb_read_first_memory_range(test_blob.bytes, &range);

    if (actual != expected ||
        range.base != 0x1122334455667788ULL ||
        range.size != 0x8877665544332211ULL) {
        fail_status(case_id, expected, actual);
    }
}

static void expect_memory(unsigned long case_id,
                          uint64_t expected_base,
                          uint64_t expected_size)
{
    struct dtb_memory_range range;
    enum dtb_status actual =
        dtb_read_first_memory_range(test_blob.bytes, &range);

    if (actual != DTB_STATUS_OK ||
        range.base != expected_base ||
        range.size != expected_size) {
        fail_status(case_id, DTB_STATUS_OK, actual);
    }
}

static void test_default_cells(void)
{
    static const uint32_t reg[] = {0U, 0x80000000U, 0x20000000U};

    builder_start(&test_blob);
    builder_add_memory(&test_blob, "memory@80000000", reg, 3U);
    builder_finish(&test_blob);
    expect_memory(1U, 0x80000000ULL, 0x20000000ULL);
}

static void test_one_cell_range(void)
{
    static const uint32_t reg[] = {0x80000000U, 0x10000000U};

    builder_start(&test_blob);
    builder_add_root_cells(&test_blob, 1U, 1U);
    builder_add_memory(&test_blob, "memory@80000000", reg, 2U);
    builder_finish(&test_blob);
    expect_memory(2U, 0x80000000ULL, 0x10000000ULL);
}

static void test_first_nonzero_range(void)
{
    static const uint32_t reg[] = {
        0U, 0x70000000U, 0U, 0U,
        0U, 0x80000000U, 0U, 0x20000000U,
    };

    builder_start(&test_blob);
    builder_add_root_cells(&test_blob, 2U, 2U);
    builder_add_memory(&test_blob, "memory@70000000", reg, 8U);
    builder_finish(&test_blob);
    expect_memory(3U, 0x80000000ULL, 0x20000000ULL);
}

static void test_rejects_reserve_block_in_header(void)
{
    static const uint32_t reg[] = {0U, 0x80000000U, 0x20000000U};

    builder_start(&test_blob);
    builder_add_memory(&test_blob, "memory@80000000", reg, 3U);
    builder_finish(&test_blob);
    write_be32(test_blob.bytes + FDT_HEADER_RESERVE_OFFSET, 0U);
    expect_error(4U, DTB_STATUS_INVALID);
}

static void test_rejects_late_root_property(void)
{
    static const uint32_t reg[] = {0U, 0x80000000U, 0x20000000U};

    builder_start(&test_blob);
    builder_add_memory(&test_blob, "memory@80000000", reg, 3U);
    builder_property_u32(&test_blob, NAME_ADDRESS_CELLS, 2U);
    builder_finish(&test_blob);
    expect_error(5U, DTB_STATUS_INVALID);
}

static void test_rejects_duplicate_root_property(void)
{
    static const uint32_t reg[] = {
        0U, 0x80000000U, 0U, 0x20000000U,
    };

    builder_start(&test_blob);
    builder_property_u32(&test_blob, NAME_ADDRESS_CELLS, 2U);
    builder_property_u32(&test_blob, NAME_ADDRESS_CELLS, 2U);
    builder_property_u32(&test_blob, NAME_SIZE_CELLS, 2U);
    builder_add_memory(&test_blob, "memory@80000000", reg, 4U);
    builder_finish(&test_blob);
    expect_error(6U, DTB_STATUS_INVALID);
}

static void test_rejects_duplicate_memory_property(void)
{
    static const uint32_t reg[] = {0U, 0x80000000U, 0x20000000U};

    builder_start(&test_blob);
    builder_begin_node(&test_blob, "memory@80000000");
    builder_property_string(&test_blob, NAME_DEVICE_TYPE, "memory", 1);
    builder_property_cells(&test_blob, reg, 3U);
    builder_property_cells(&test_blob, reg, 3U);
    builder_end_node(&test_blob);
    builder_finish(&test_blob);
    expect_error(7U, DTB_STATUS_INVALID);
}

static void test_rejects_unterminated_device_type(void)
{
    static const uint32_t reg[] = {0U, 0x80000000U, 0x20000000U};

    builder_start(&test_blob);
    builder_begin_node(&test_blob, "memory@80000000");
    builder_property_string(&test_blob, NAME_DEVICE_TYPE, "memory", 0);
    builder_property_cells(&test_blob, reg, 3U);
    builder_end_node(&test_blob);
    builder_finish(&test_blob);
    expect_error(8U, DTB_STATUS_INVALID);
}

static void test_rejects_empty_unit_address(void)
{
    static const uint32_t reg[] = {0U, 0x80000000U, 0x20000000U};

    builder_start(&test_blob);
    builder_add_memory(&test_blob, "memory@", reg, 3U);
    builder_finish(&test_blob);
    expect_error(9U, DTB_STATUS_INVALID);
}

static void test_reports_invalid_magic(void)
{
    builder_start(&test_blob);
    builder_finish(&test_blob);
    write_be32(test_blob.bytes + FDT_HEADER_MAGIC, 0U);
    expect_error(10U, DTB_STATUS_INVALID);
}

static void test_reports_unsupported_version(void)
{
    builder_start(&test_blob);
    builder_finish(&test_blob);
    write_be32(test_blob.bytes + FDT_HEADER_VERSION, 16U);
    expect_error(11U, DTB_STATUS_UNSUPPORTED);
}

static void test_reports_unsupported_cell_count(void)
{
    builder_start(&test_blob);
    builder_add_root_cells(&test_blob, 3U, 1U);
    builder_finish(&test_blob);
    expect_error(12U, DTB_STATUS_UNSUPPORTED);
}

static void test_reports_missing_memory(void)
{
    builder_start(&test_blob);
    builder_begin_node(&test_blob, "chosen");
    builder_end_node(&test_blob);
    builder_finish(&test_blob);
    expect_error(13U, DTB_STATUS_NOT_FOUND);
}

static void test_rejects_memory_without_device_type(void)
{
    static const uint32_t reg[] = {0U, 0x80000000U, 0x20000000U};

    builder_start(&test_blob);
    builder_begin_node(&test_blob, "memory@80000000");
    builder_property_cells(&test_blob, reg, 3U);
    builder_end_node(&test_blob);
    builder_finish(&test_blob);
    expect_error(14U, DTB_STATUS_INVALID);
}

static void test_rejects_wrong_memory_device_type(void)
{
    static const uint32_t reg[] = {0U, 0x80000000U, 0x20000000U};

    builder_start(&test_blob);
    builder_begin_node(&test_blob, "memory@80000000");
    builder_property_string(&test_blob, NAME_DEVICE_TYPE, "cpu", 1);
    builder_property_cells(&test_blob, reg, 3U);
    builder_end_node(&test_blob);
    builder_finish(&test_blob);
    expect_error(15U, DTB_STATUS_INVALID);
}

static void test_rejects_memory_without_reg(void)
{
    builder_start(&test_blob);
    builder_begin_node(&test_blob, "memory@80000000");
    builder_property_string(&test_blob, NAME_DEVICE_TYPE, "memory", 1);
    builder_end_node(&test_blob);
    builder_finish(&test_blob);
    expect_error(16U, DTB_STATUS_INVALID);
}

void kernel_main(unsigned long hart_id, const void *dtb)
{
    (void)hart_id;
    (void)dtb;

    test_default_cells();
    test_one_cell_range();
    test_first_nonzero_range();
    test_rejects_reserve_block_in_header();
    test_rejects_late_root_property();
    test_rejects_duplicate_root_property();
    test_rejects_duplicate_memory_property();
    test_rejects_unterminated_device_type();
    test_rejects_empty_unit_address();
    test_reports_invalid_magic();
    test_reports_unsupported_version();
    test_reports_unsupported_cell_count();
    test_reports_missing_memory();
    test_rejects_memory_without_device_type();
    test_rejects_wrong_memory_device_type();
    test_rejects_memory_without_reg();

    virt_uart_puts("BoarOS: DTB parser tests passed\n");
    sbi_shutdown();
}
