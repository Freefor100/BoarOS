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

#define DTB_DISCOVERY_MAX_DEPTH 16U

static uint32_t read_be32(const unsigned char *bytes)
{
    return ((uint32_t)bytes[0] << 24) |
           ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) |
           (uint32_t)bytes[3];
}

static uint64_t read_be64(const unsigned char *bytes)
{
    return ((uint64_t)read_be32(bytes) << 32) |
           read_be32(bytes + 4U);
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

static enum dtb_status append_reserved_reg(
    const unsigned char *value,
    uint32_t length,
    uint32_t address_cells,
    uint32_t size_cells,
    struct dtb_boot_info *info)
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

        if (size == 0U) {
            continue;
        }
        if (info->reserved_count == DTB_MAX_RESERVED_RANGES) {
            return DTB_STATUS_UNSUPPORTED;
        }

        info->reserved[info->reserved_count].base =
            read_cells(value + offset, address_cells);
        info->reserved[info->reserved_count].size = size;
        info->reserved_count++;
    }

    return DTB_STATUS_OK;
}

struct dtb_discovery_node {
    const unsigned char *ranges;
    const unsigned char *reg;
    uint32_t ranges_length;
    uint32_t reg_length;
    uint32_t child_address_cells;
    uint32_t child_size_cells;
    uint8_t parent_enabled;
    uint8_t enabled;
    uint8_t children_started;
    uint8_t address_cells_seen;
    uint8_t size_cells_seen;
    uint8_t ranges_seen;
    uint8_t reg_seen;
    uint8_t compatible_seen;
    uint8_t compatible_virtio_mmio;
    uint8_t status_seen;
};

static int string_list_contains(const unsigned char *value,
                                uint32_t length,
                                const char *expected,
                                int *contains)
{
    uint32_t position = 0U;
    int result = 0;

    if (length == 0U) {
        return 0;
    }
    while (position < length) {
        uint32_t string_length;

        if (!bounded_string_length(value + position,
                                   length - position,
                                   &string_length) ||
            string_length == 0U) {
            return 0;
        }
        if (bytes_equal_string(value + position,
                               string_length,
                               expected)) {
            result = 1;
        }
        position += string_length + 1U;
    }

    *contains = result;
    return 1;
}

static int status_is_available(const unsigned char *value,
                               uint32_t length,
                               int *available)
{
    uint32_t string_length;

    if (!property_value_is_one_string(value, length) || length <= 1U) {
        return 0;
    }
    string_length = length - 1U;
    *available = bytes_equal_string(value, string_length, "okay") ||
                 bytes_equal_string(value, string_length, "ok");
    return 1;
}

static enum dtb_status translate_bus_address(
    const struct dtb_discovery_node *bus,
    uint32_t parent_address_cells,
    uint64_t address,
    uint64_t size,
    uint64_t *translated)
{
    uint32_t tuple_cells;
    uint32_t tuple_size;
    uint32_t position;

    if (!bus->ranges_seen) {
        return DTB_STATUS_UNSUPPORTED;
    }
    if (bus->ranges_length == 0U) {
        *translated = address;
        return DTB_STATUS_OK;
    }
    if (bus->child_address_cells == 0U ||
        bus->child_address_cells > 2U ||
        parent_address_cells == 0U || parent_address_cells > 2U ||
        bus->child_size_cells > 2U) {
        return DTB_STATUS_UNSUPPORTED;
    }

    tuple_cells = bus->child_address_cells + parent_address_cells +
                  bus->child_size_cells;
    if (tuple_cells == 0U || tuple_cells > UINT32_MAX / 4U) {
        return DTB_STATUS_INVALID;
    }
    tuple_size = tuple_cells * 4U;
    if (bus->ranges_length % tuple_size != 0U) {
        return DTB_STATUS_INVALID;
    }

    for (position = 0U; position < bus->ranges_length;
         position += tuple_size) {
        const unsigned char *tuple = bus->ranges + position;
        uint64_t child_base = read_cells(tuple,
                                         bus->child_address_cells);
        uint64_t parent_base = read_cells(
            tuple + bus->child_address_cells * 4U,
            parent_address_cells);
        uint64_t range_size = bus->child_size_cells == 0U ?
                                  UINT64_MAX :
                                  read_cells(tuple +
                                                 (bus->child_address_cells +
                                                  parent_address_cells) * 4U,
                                             bus->child_size_cells);
        uint64_t offset;

        if (address < child_base) {
            continue;
        }
        offset = address - child_base;
        if (offset >= range_size || size > range_size - offset) {
            continue;
        }
        if (offset > UINT64_MAX - parent_base) {
            return DTB_STATUS_INVALID;
        }
        *translated = parent_base + offset;
        return DTB_STATUS_OK;
    }

    return DTB_STATUS_UNSUPPORTED;
}

static enum dtb_status insert_virtio_mmio(
    struct dtb_boot_info *info,
    uint64_t base,
    uint64_t size)
{
    uint32_t position;

    if (size == 0U || size > UINT64_MAX - base) {
        return DTB_STATUS_INVALID;
    }
    if (info->virtio_mmio_count == DTB_MAX_VIRTIO_MMIO_RANGES) {
        return DTB_STATUS_UNSUPPORTED;
    }

    position = info->virtio_mmio_count;
    while (position > 0U && info->virtio_mmio[position - 1U].base > base) {
        info->virtio_mmio[position] = info->virtio_mmio[position - 1U];
        position--;
    }
    info->virtio_mmio[position].base = base;
    info->virtio_mmio[position].size = size;
    info->virtio_mmio_count++;

    if ((position > 0U &&
         info->virtio_mmio[position - 1U].size >
             base - info->virtio_mmio[position - 1U].base) ||
        (position + 1U < info->virtio_mmio_count &&
         size > info->virtio_mmio[position + 1U].base - base)) {
        return DTB_STATUS_INVALID;
    }

    return DTB_STATUS_OK;
}

static enum dtb_status finish_discovery_node(
    struct dtb_discovery_node *nodes,
    uint32_t depth,
    struct dtb_boot_info *info)
{
    struct dtb_discovery_node *node = &nodes[depth - 1U];
    const struct dtb_discovery_node *parent;
    uint32_t address_cells;
    uint32_t size_cells;
    uint32_t tuple_cells;
    uint32_t tuple_size;
    uint64_t address;
    uint64_t size;
    uint32_t bus_index;

    if (!node->compatible_virtio_mmio || !node->enabled) {
        return DTB_STATUS_OK;
    }
    if (depth < 2U || !node->reg_seen || node->reg == 0) {
        return DTB_STATUS_INVALID;
    }

    parent = &nodes[depth - 2U];
    address_cells = parent->child_address_cells;
    size_cells = parent->child_size_cells;
    if (address_cells == 0U || address_cells > 2U ||
        size_cells == 0U || size_cells > 2U) {
        return DTB_STATUS_UNSUPPORTED;
    }
    tuple_cells = address_cells + size_cells;
    tuple_size = tuple_cells * 4U;
    if (node->reg_length != tuple_size) {
        return DTB_STATUS_INVALID;
    }

    address = read_cells(node->reg, address_cells);
    size = read_cells(node->reg + address_cells * 4U, size_cells);
    if (size == 0U || size > UINT64_MAX - address) {
        return DTB_STATUS_INVALID;
    }

    bus_index = depth - 2U;
    while (bus_index > 0U) {
        enum dtb_status status = translate_bus_address(
            &nodes[bus_index],
            nodes[bus_index - 1U].child_address_cells,
            address,
            size,
            &address);

        if (status != DTB_STATUS_OK) {
            return status;
        }
        bus_index--;
    }

    return insert_virtio_mmio(info, address, size);
}

static enum dtb_status discover_virtio_mmio(
    const unsigned char *structure,
    uint32_t structure_size,
    const unsigned char *strings,
    uint32_t strings_size,
    struct dtb_boot_info *info)
{
    struct dtb_discovery_node nodes[DTB_DISCOVERY_MAX_DEPTH];
    uint32_t position = 0U;
    uint32_t depth = 0U;
    int saw_root = 0;

    info->virtio_mmio_count = 0U;

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
            struct dtb_discovery_node node;

            if (!bounded_string_length(name,
                                       structure_size - position,
                                       &name_length) ||
                !align_to_u32(position + name_length + 1U,
                              &next_position) ||
                next_position > structure_size) {
                return DTB_STATUS_INVALID;
            }
            if (depth == DTB_DISCOVERY_MAX_DEPTH) {
                return DTB_STATUS_UNSUPPORTED;
            }
            if (depth == 0U) {
                if (saw_root || name_length != 0U) {
                    return DTB_STATUS_INVALID;
                }
                saw_root = 1;
            } else {
                nodes[depth - 1U].children_started = 1U;
            }

            node.ranges = 0;
            node.reg = 0;
            node.ranges_length = 0U;
            node.reg_length = 0U;
            node.child_address_cells = 2U;
            node.child_size_cells = 1U;
            node.parent_enabled = depth == 0U ?
                                      1U : nodes[depth - 1U].enabled;
            node.enabled = node.parent_enabled;
            node.children_started = 0U;
            node.address_cells_seen = 0U;
            node.size_cells_seen = 0U;
            node.ranges_seen = 0U;
            node.reg_seen = 0U;
            node.compatible_seen = 0U;
            node.compatible_virtio_mmio = 0U;
            node.status_seen = 0U;
            nodes[depth] = node;
            depth++;
            position = next_position;
        } else if (token == FDT_END_NODE) {
            enum dtb_status status;

            if (depth == 0U) {
                return DTB_STATUS_INVALID;
            }
            status = finish_discovery_node(nodes, depth, info);
            if (status != DTB_STATUS_OK) {
                return status;
            }
            depth--;
        } else if (token == FDT_PROP) {
            const unsigned char *name;
            const unsigned char *value;
            struct dtb_discovery_node *node;
            uint32_t length;
            uint32_t name_offset;
            uint32_t name_length;
            uint32_t padded_length;
            enum dtb_status status;

            if (depth == 0U || structure_size - position < 8U) {
                return DTB_STATUS_INVALID;
            }
            node = &nodes[depth - 1U];
            if (node->children_started) {
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

            if (bytes_equal_string(name, name_length, "#address-cells")) {
                if (node->address_cells_seen || length != 4U) {
                    return DTB_STATUS_INVALID;
                }
                node->address_cells_seen = 1U;
                node->child_address_cells = read_be32(value);
            } else if (bytes_equal_string(name,
                                          name_length,
                                          "#size-cells")) {
                if (node->size_cells_seen || length != 4U) {
                    return DTB_STATUS_INVALID;
                }
                node->size_cells_seen = 1U;
                node->child_size_cells = read_be32(value);
            } else if (bytes_equal_string(name, name_length, "ranges")) {
                if (node->ranges_seen) {
                    return DTB_STATUS_INVALID;
                }
                node->ranges_seen = 1U;
                node->ranges = value;
                node->ranges_length = length;
            } else if (bytes_equal_string(name, name_length, "reg")) {
                if (node->reg_seen) {
                    return DTB_STATUS_INVALID;
                }
                node->reg_seen = 1U;
                node->reg = value;
                node->reg_length = length;
            } else if (bytes_equal_string(name,
                                          name_length,
                                          "compatible")) {
                int compatible;

                if (node->compatible_seen ||
                    !string_list_contains(value,
                                          length,
                                          "virtio,mmio",
                                          &compatible)) {
                    return DTB_STATUS_INVALID;
                }
                node->compatible_seen = 1U;
                node->compatible_virtio_mmio = (uint8_t)compatible;
            } else if (bytes_equal_string(name, name_length, "status")) {
                int available;

                if (node->status_seen ||
                    !status_is_available(value, length, &available)) {
                    return DTB_STATUS_INVALID;
                }
                node->status_seen = 1U;
                node->enabled = (uint8_t)(node->parent_enabled && available);
            }
        } else if (token == FDT_NOP) {
        } else if (token == FDT_END) {
            if (!saw_root || depth != 0U || position != structure_size) {
                return DTB_STATUS_INVALID;
            }
            return DTB_STATUS_OK;
        } else {
            return DTB_STATUS_INVALID;
        }
    }

    return DTB_STATUS_INVALID;
}

enum dtb_status dtb_read_boot_info(const void *dtb,
                                   struct dtb_boot_info *info)
{
    const unsigned char *blob = dtb;
    const unsigned char *structure;
    const unsigned char *strings;
    const unsigned char *memory_reg = NULL;
    const unsigned char *reserved_reg = NULL;
    uint32_t memory_reg_length = 0U;
    uint32_t reserved_reg_length = 0U;
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
    uint32_t reserved_address_cells = 0U;
    uint32_t reserved_size_cells = 0U;
    struct dtb_boot_info result = {0};
    int saw_root = 0;
    int address_cells_seen = 0;
    int size_cells_seen = 0;
    int memory_name = 0;
    int memory_type = 0;
    int memory_type_seen = 0;
    int found = 0;
    int memory_reg_seen = 0;
    int reserved_node = 0;
    int reserved_node_seen = 0;
    int reserved_address_cells_seen = 0;
    int reserved_size_cells_seen = 0;
    int reserved_ranges_seen = 0;
    int reserved_child = 0;
    int reserved_reg_seen = 0;
    int reserved_size_seen = 0;
    int cpus_node = 0;
    int chosen_node = 0;
    int rng_seed_seen = 0;
    int timebase_frequency_seen = 0;

    if (blob == NULL || info == NULL) {
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

    result.dtb_size = total_size;
    result.rng_seed_size = 0U;
    reserve_position = reserve_offset;
    for (;;) {
        if (reserve_position > structure_offset ||
            structure_offset - reserve_position < 16U) {
            return DTB_STATUS_INVALID;
        }

        if (reserve_entry_is_terminator(blob + reserve_position)) {
            break;
        }

        if (read_be64(blob + reserve_position + 8U) != 0U) {
            if (result.reserved_count == DTB_MAX_RESERVED_RANGES) {
                return DTB_STATUS_UNSUPPORTED;
            }
            result.reserved[result.reserved_count].base =
                read_be64(blob + reserve_position);
            result.reserved[result.reserved_count].size =
                read_be64(blob + reserve_position + 8U);
            result.reserved_count++;
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
                cpus_node = bytes_equal_string(name, name_length, "cpus");
                chosen_node = bytes_equal_string(name, name_length, "chosen");
                memory_name = classify_memory_node_name(name, name_length);
                if (memory_name < 0) {
                    return DTB_STATUS_INVALID;
                }
                memory_type = 0;
                memory_type_seen = 0;
                memory_reg = NULL;
                memory_reg_length = 0U;
                memory_reg_seen = 0;

                reserved_node = bytes_equal_string(name,
                                                   name_length,
                                                   "reserved-memory");
                if (reserved_node) {
                    if (reserved_node_seen) {
                        return DTB_STATUS_INVALID;
                    }
                    reserved_node_seen = 1;
                    reserved_address_cells_seen = 0;
                    reserved_size_cells_seen = 0;
                    reserved_ranges_seen = 0;
                }
            } else if (depth == 3U && reserved_node) {
                if (!reserved_address_cells_seen ||
                    !reserved_size_cells_seen || !reserved_ranges_seen) {
                    return DTB_STATUS_INVALID;
                }
                reserved_child = 1;
                reserved_reg = NULL;
                reserved_reg_length = 0U;
                reserved_reg_seen = 0;
                reserved_size_seen = 0;
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
                                         &result.memory,
                                         &found);
                if (status != DTB_STATUS_OK) {
                    return status;
                }
            }

            if (depth == 3U && reserved_child) {
                if (reserved_reg_seen && reserved_size_seen) {
                    return DTB_STATUS_INVALID;
                }
                if (!reserved_reg_seen) {
                    return reserved_size_seen ? DTB_STATUS_UNSUPPORTED
                                              : DTB_STATUS_INVALID;
                }
                status = append_reserved_reg(reserved_reg,
                                             reserved_reg_length,
                                             reserved_address_cells,
                                             reserved_size_cells,
                                             &result);
                if (status != DTB_STATUS_OK) {
                    return status;
                }
                reserved_child = 0;
            } else if (depth == 2U && reserved_node) {
                if (!reserved_address_cells_seen ||
                    !reserved_size_cells_seen || !reserved_ranges_seen ||
                    reserved_address_cells != address_cells ||
                    reserved_size_cells != size_cells) {
                    return DTB_STATUS_INVALID;
                }
                reserved_node = 0;
            }

            if (depth == 2U) {
                cpus_node = 0;
                chosen_node = 0;
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

            if (((depth == 2U && (memory_name || reserved_node)) ||
                 (depth == 3U && reserved_child)) &&
                bytes_equal_string(name, name_length, "status")) {
                return DTB_STATUS_UNSUPPORTED;
            } else if (depth == 2U && cpus_node &&
                       bytes_equal_string(name,
                                          name_length,
                                          "timebase-frequency")) {
                if (timebase_frequency_seen || length != 4U) {
                    return DTB_STATUS_INVALID;
                }
                result.timebase_frequency = read_be32(value);
                if (result.timebase_frequency == 0U) {
                    return DTB_STATUS_INVALID;
                }
                timebase_frequency_seen = 1;
            } else if (depth == 2U && chosen_node &&
                       bytes_equal_string(name, name_length, "rng-seed")) {
                uint32_t copy_size;

                if (rng_seed_seen || length < DTB_RNG_SEED_SIZE) {
                    return length < DTB_RNG_SEED_SIZE
                               ? DTB_STATUS_UNSUPPORTED
                               : DTB_STATUS_INVALID;
                }
                copy_size = DTB_RNG_SEED_SIZE;
                for (uint32_t seed_index = 0U;
                     seed_index < copy_size;
                     seed_index++) {
                    result.rng_seed[seed_index] = value[seed_index];
                }
                result.rng_seed_size = copy_size;
                rng_seed_seen = 1;
            } else if (depth == 1U &&
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
            } else if (depth == 2U && reserved_node &&
                       bytes_equal_string(name,
                                          name_length,
                                          "#address-cells")) {
                if (reserved_address_cells_seen || length != 4U) {
                    return DTB_STATUS_INVALID;
                }
                reserved_address_cells_seen = 1;
                reserved_address_cells = read_be32(value);
                if (reserved_address_cells == 0U ||
                    reserved_address_cells > 2U) {
                    return DTB_STATUS_UNSUPPORTED;
                }
            } else if (depth == 2U && reserved_node &&
                       bytes_equal_string(name,
                                          name_length,
                                          "#size-cells")) {
                if (reserved_size_cells_seen || length != 4U) {
                    return DTB_STATUS_INVALID;
                }
                reserved_size_cells_seen = 1;
                reserved_size_cells = read_be32(value);
                if (reserved_size_cells == 0U ||
                    reserved_size_cells > 2U) {
                    return DTB_STATUS_UNSUPPORTED;
                }
            } else if (depth == 2U && reserved_node &&
                       bytes_equal_string(name, name_length, "ranges")) {
                if (reserved_ranges_seen || length != 0U) {
                    return DTB_STATUS_INVALID;
                }
                reserved_ranges_seen = 1;
            } else if (depth == 3U && reserved_child &&
                       bytes_equal_string(name, name_length, "reg")) {
                if (reserved_reg_seen) {
                    return DTB_STATUS_INVALID;
                }
                reserved_reg_seen = 1;
                reserved_reg = value;
                reserved_reg_length = length;
            } else if (depth == 3U && reserved_child &&
                       bytes_equal_string(name, name_length, "size")) {
                if (reserved_size_seen) {
                    return DTB_STATUS_INVALID;
                }
                reserved_size_seen = 1;
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

            {
                enum dtb_status status = discover_virtio_mmio(
                    structure,
                    structure_size,
                    strings,
                    strings_size,
                    &result);

                if (status != DTB_STATUS_OK) {
                    return status;
                }
            }

            *info = result;
            return DTB_STATUS_OK;
        } else {
            return DTB_STATUS_INVALID;
        }
    }

    return DTB_STATUS_INVALID;
}
