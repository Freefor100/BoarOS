#include <kernel/dtb.h>

#include <stddef.h>
#include <stdint.h>

#define FDT_MAGIC 0xd00dfeedU
#define FDT_HEADER_SIZE 40U
#define FDT_VERSION 17U

#define FDT_BEGIN_NODE 1U
#define FDT_END_NODE 2U
#define FDT_PROP 3U
#define FDT_NOP 4U
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

static uint32_t read_be32(const unsigned char *bytes)
{
    return ((uint32_t)bytes[0] << 24) |
           ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) |
           (uint32_t)bytes[3];
}

static int reserve_entry_is_terminator(const unsigned char *entry)
{
    return read_be32(entry) == 0U &&
           read_be32(entry + 4U) == 0U &&
           read_be32(entry + 8U) == 0U &&
           read_be32(entry + 12U) == 0U;
}

static int range_is_inside(uint32_t total_size,
                           uint32_t offset,
                           uint32_t size)
{
    return offset <= total_size && size <= total_size - offset;
}

static int align_to_u32(uint32_t value, uint32_t *aligned)
{
    if (value > UINT32_MAX - 3U) {
        return 0;
    }

    *aligned = (value + 3U) & ~3U;
    return 1;
}

static int bounded_string_length(const unsigned char *bytes,
                                 uint32_t available,
                                 uint32_t *length)
{
    uint32_t index;

    for (index = 0U; index < available; index++) {
        if (bytes[index] == '\0') {
            *length = index;
            return 1;
        }
    }

    return 0;
}

static int bytes_equal_string(const unsigned char *bytes,
                              uint32_t length,
                              const char *expected)
{
    uint32_t index;

    for (index = 0U; index < length; index++) {
        if (expected[index] == '\0' ||
            bytes[index] != (unsigned char)expected[index]) {
            return 0;
        }
    }

    return expected[length] == '\0';
}

static int property_value_is_one_string(const unsigned char *value,
                                        uint32_t length)
{
    uint32_t string_length;

    return bounded_string_length(value, length, &string_length) &&
           string_length + 1U == length;
}

static int classify_memory_node_name(const unsigned char *name,
                                     uint32_t length)
{
    static const char prefix[] = "memory";
    uint32_t index;

    if (length < sizeof(prefix) - 1U) {
        return 0;
    }

    for (index = 0U; index < sizeof(prefix) - 1U; index++) {
        if (name[index] != (unsigned char)prefix[index]) {
            return 0;
        }
    }

    if (length == sizeof(prefix) - 1U) {
        return 1;
    }
    if (name[sizeof(prefix) - 1U] != '@') {
        return 0;
    }

    return length > sizeof(prefix) ? 1 : -1;
}

static enum dtb_status property_name(const unsigned char *strings,
                                     uint32_t strings_size,
                                     uint32_t name_offset,
                                     const unsigned char **name,
                                     uint32_t *name_length)
{
    if (name_offset >= strings_size ||
        !bounded_string_length(strings + name_offset,
                               strings_size - name_offset,
                               name_length)) {
        return DTB_STATUS_INVALID;
    }

    *name = strings + name_offset;
    return DTB_STATUS_OK;
}

static uint64_t read_cells(const unsigned char *value, uint32_t cell_count)
{
    uint64_t result = 0U;
    uint32_t index;

    for (index = 0U; index < cell_count; index++) {
        result = (result << 32) + read_be32(value + index * 4U);
    }

    return result;
}

static enum dtb_status read_memory_reg(const unsigned char *value,
                                       uint32_t length,
                                       uint32_t address_cells,
                                       uint32_t size_cells,
                                       struct dtb_memory_range *range,
                                       int *found)
{
    uint32_t tuple_cells = address_cells + size_cells;
    uint32_t tuple_size = tuple_cells * 4U;
    uint32_t offset;

    if (tuple_size == 0U || length == 0U || length % tuple_size != 0U) {
        return DTB_STATUS_INVALID;
    }

    for (offset = 0U; offset < length; offset += tuple_size) {
        uint64_t size = read_cells(value + offset + address_cells * 4U,
                                   size_cells);

        if (!*found && size != 0U) {
            range->base = read_cells(value + offset, address_cells);
            range->size = size;
            *found = 1;
        }
    }

    return DTB_STATUS_OK;
}

enum dtb_status dtb_read_first_memory_range(
    const void *dtb,
    struct dtb_memory_range *range)
{
    const unsigned char *blob = dtb;
    const unsigned char *structure;
    const unsigned char *strings;
    const unsigned char *memory_reg = NULL;
    uint32_t memory_reg_length = 0U;
    uint32_t total_size;
    uint32_t structure_offset;
    uint32_t structure_size;
    uint32_t strings_offset;
    uint32_t strings_size;
    uint32_t reserve_offset;
    uint32_t reserve_position;
    uint32_t version;
    uint32_t last_compatible_version;
    uint32_t position = 0U;
    uint32_t depth = 0U;
    uint32_t property_depth = 0U;
    uint32_t address_cells = 2U;
    uint32_t size_cells = 1U;
    struct dtb_memory_range result;
    int saw_root = 0;
    int address_cells_seen = 0;
    int size_cells_seen = 0;
    int memory_name = 0;
    int memory_type = 0;
    int memory_type_seen = 0;
    int found = 0;
    int memory_reg_seen = 0;

    if (blob == NULL || range == NULL) {
        return DTB_STATUS_INVALID;
    }

    if (read_be32(blob + FDT_HEADER_MAGIC) != FDT_MAGIC) {
        return DTB_STATUS_INVALID;
    }

    total_size = read_be32(blob + FDT_HEADER_TOTAL_SIZE);
    if (total_size < FDT_HEADER_SIZE) {
        return DTB_STATUS_INVALID;
    }

    structure_offset = read_be32(blob + FDT_HEADER_STRUCT_OFFSET);
    strings_offset = read_be32(blob + FDT_HEADER_STRINGS_OFFSET);
    reserve_offset = read_be32(blob + FDT_HEADER_RESERVE_OFFSET);
    version = read_be32(blob + FDT_HEADER_VERSION);
    last_compatible_version =
        read_be32(blob + FDT_HEADER_LAST_COMPATIBLE_VERSION);
    strings_size = read_be32(blob + FDT_HEADER_STRINGS_SIZE);
    structure_size = read_be32(blob + FDT_HEADER_STRUCT_SIZE);

    if (version < FDT_VERSION || last_compatible_version > FDT_VERSION) {
        return DTB_STATUS_UNSUPPORTED;
    }

    if (reserve_offset < FDT_HEADER_SIZE ||
        structure_offset < FDT_HEADER_SIZE ||
        strings_offset < FDT_HEADER_SIZE ||
        reserve_offset > structure_offset ||
        structure_offset > strings_offset ||
        (structure_offset & 3U) != 0U ||
        (structure_size & 3U) != 0U ||
        (reserve_offset & 7U) != 0U ||
        structure_size > strings_offset - structure_offset ||
        !range_is_inside(total_size, strings_offset, strings_size)) {
        return DTB_STATUS_INVALID;
    }

    reserve_position = reserve_offset;
    for (;;) {
        if (reserve_position > structure_offset ||
            structure_offset - reserve_position < 16U) {
            return DTB_STATUS_INVALID;
        }

        if (reserve_entry_is_terminator(blob + reserve_position)) {
            break;
        }
        reserve_position += 16U;
    }

    structure = blob + structure_offset;
    strings = blob + strings_offset;

    while (position < structure_size) {
        uint32_t token;

        if (structure_size - position < 4U) {
            return DTB_STATUS_INVALID;
        }

        token = read_be32(structure + position);
        position += 4U;

        if (token == FDT_BEGIN_NODE) {
            const unsigned char *name = structure + position;
            uint32_t name_length;
            uint32_t next_position;

            if (!bounded_string_length(name,
                                       structure_size - position,
                                       &name_length) ||
                !align_to_u32(position + name_length + 1U,
                              &next_position) ||
                next_position > structure_size) {
                return DTB_STATUS_INVALID;
            }

            if (depth == 0U) {
                if (saw_root || name_length != 0U) {
                    return DTB_STATUS_INVALID;
                }
                saw_root = 1;
            }

            depth++;
            property_depth = depth;
            position = next_position;

            if (depth == 2U) {
                memory_name = classify_memory_node_name(name, name_length);
                if (memory_name < 0) {
                    return DTB_STATUS_INVALID;
                }
                memory_type = 0;
                memory_type_seen = 0;
                memory_reg = NULL;
                memory_reg_length = 0U;
                memory_reg_seen = 0;
            }
        } else if (token == FDT_END_NODE) {
            enum dtb_status status;

            if (depth == 0U) {
                return DTB_STATUS_INVALID;
            }

            if (depth == 2U && memory_name) {
                if (!memory_type_seen || !memory_type ||
                    !memory_reg_seen || memory_reg == NULL) {
                    return DTB_STATUS_INVALID;
                }
                status = read_memory_reg(memory_reg,
                                         memory_reg_length,
                                         address_cells,
                                         size_cells,
                                         &result,
                                         &found);
                if (status != DTB_STATUS_OK) {
                    return status;
                }
            }

            if (property_depth == depth) {
                property_depth = 0U;
            }
            depth--;
        } else if (token == FDT_PROP) {
            const unsigned char *name;
            const unsigned char *value;
            uint32_t length;
            uint32_t name_offset;
            uint32_t name_length;
            uint32_t padded_length;
            enum dtb_status status;

            if (depth == 0U || property_depth != depth ||
                structure_size - position < 8U) {
                return DTB_STATUS_INVALID;
            }

            length = read_be32(structure + position);
            name_offset = read_be32(structure + position + 4U);
            position += 8U;

            if (!align_to_u32(length, &padded_length) ||
                padded_length > structure_size - position) {
                return DTB_STATUS_INVALID;
            }

            value = structure + position;
            position += padded_length;
            status = property_name(strings,
                                   strings_size,
                                   name_offset,
                                   &name,
                                   &name_length);
            if (status != DTB_STATUS_OK) {
                return status;
            }

            if (depth == 1U &&
                bytes_equal_string(name, name_length, "#address-cells")) {
                if (address_cells_seen || length != 4U) {
                    return DTB_STATUS_INVALID;
                }
                address_cells_seen = 1;
                address_cells = read_be32(value);
                if (address_cells == 0U || address_cells > 2U) {
                    return DTB_STATUS_UNSUPPORTED;
                }
            } else if (depth == 1U &&
                       bytes_equal_string(name,
                                          name_length,
                                          "#size-cells")) {
                if (size_cells_seen || length != 4U) {
                    return DTB_STATUS_INVALID;
                }
                size_cells_seen = 1;
                size_cells = read_be32(value);
                if (size_cells == 0U || size_cells > 2U) {
                    return DTB_STATUS_UNSUPPORTED;
                }
            } else if (depth == 2U && memory_name &&
                       bytes_equal_string(name,
                                          name_length,
                                          "device_type")) {
                if (memory_type_seen) {
                    return DTB_STATUS_INVALID;
                }
                memory_type_seen = 1;
                if (!property_value_is_one_string(value, length)) {
                    return DTB_STATUS_INVALID;
                }
                memory_type = bytes_equal_string(value,
                                                 length - 1U,
                                                 "memory");
            } else if (depth == 2U && memory_name &&
                       bytes_equal_string(name, name_length, "reg")) {
                if (memory_reg_seen) {
                    return DTB_STATUS_INVALID;
                }
                memory_reg_seen = 1;
                memory_reg = value;
                memory_reg_length = length;
            }
        } else if (token == FDT_NOP) {
        } else if (token == FDT_END) {
            if (!saw_root || depth != 0U || position != structure_size) {
                return DTB_STATUS_INVALID;
            }

            if (!found) {
                return DTB_STATUS_NOT_FOUND;
            }

            *range = result;
            return DTB_STATUS_OK;
        } else {
            return DTB_STATUS_INVALID;
        }
    }

    return DTB_STATUS_INVALID;
}
