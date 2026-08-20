CROSS_COMPILE ?= $(shell \
	if command -v riscv64-unknown-elf-gcc >/dev/null 2>&1; then \
		printf '%s' riscv64-unknown-elf-; \
	elif command -v riscv64-elf-gcc >/dev/null 2>&1; then \
		printf '%s' riscv64-elf-; \
	else \
		printf '%s' riscv64-unknown-elf-; \
	fi)

CC := $(CROSS_COMPILE)gcc
QEMU_RISCV64 ?= qemu-system-riscv64
QEMU_MEMORY ?= 1G

BUILD_DIR := build/riscv
KERNEL_RV := kernel-rv
TRAP_TEST_KERNEL_RV := $(BUILD_DIR)/tests/kernel-trap-rv
DTB_TEST_KERNEL_RV := $(BUILD_DIR)/tests/kernel-dtb-rv

ARCH_FLAGS := -march=rv64imac_zicsr_zifencei -mabi=lp64 -mcmodel=medany
CPPFLAGS := -Iinclude
CFLAGS := $(ARCH_FLAGS) -std=gnu11 -O2 -g3 \
	-ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie \
	-ffunction-sections -fdata-sections -Wall -Wextra -Werror
ASFLAGS := $(ARCH_FLAGS) -g3
LDFLAGS := $(ARCH_FLAGS) -nostdlib -nostartfiles -static -no-pie \
	-T arch/riscv/linker.ld -Wl,--build-id=none -Wl,--gc-sections

C_SOURCES := \
	arch/riscv/sbi.c \
	arch/riscv/trap.c \
	arch/riscv/virt_uart.c \
	kernel/boot_memory.c \
	kernel/dtb.c \
	kernel/main.c
ASM_SOURCES := \
	arch/riscv/boot.S \
	arch/riscv/trap_entry.S
OBJECTS := \
	$(patsubst %.c,$(BUILD_DIR)/%.o,$(C_SOURCES)) \
	$(patsubst %.S,$(BUILD_DIR)/%.o,$(ASM_SOURCES))
TRAP_TEST_C_SOURCES := tests/riscv/trap_main.c
TRAP_TEST_ASM_SOURCES := tests/riscv/trap_trigger.S
TRAP_TEST_OBJECTS := \
	$(filter-out $(BUILD_DIR)/kernel/main.o,$(OBJECTS)) \
	$(patsubst %.c,$(BUILD_DIR)/%.o,$(TRAP_TEST_C_SOURCES)) \
	$(patsubst %.S,$(BUILD_DIR)/%.o,$(TRAP_TEST_ASM_SOURCES))
DTB_TEST_C_SOURCES := \
	tests/riscv/boot_memory_cases.c \
	tests/riscv/dtb_main.c
DTB_TEST_OBJECTS := \
	$(filter-out $(BUILD_DIR)/kernel/main.o,$(OBJECTS)) \
	$(patsubst %.c,$(BUILD_DIR)/%.o,$(DTB_TEST_C_SOURCES))
DEPS := \
	$(OBJECTS:.o=.d) \
	$(TRAP_TEST_OBJECTS:.o=.d) \
	$(DTB_TEST_OBJECTS:.o=.d)

.PHONY: all clean debug-riscv run-riscv test-dtb-riscv test-riscv \
	test-trap-riscv

all: $(KERNEL_RV)

$(KERNEL_RV): $(OBJECTS) arch/riscv/linker.ld
	$(CC) $(LDFLAGS) -Wl,-Map,$(BUILD_DIR)/kernel-rv.map \
		-o $@ $(OBJECTS)

$(TRAP_TEST_KERNEL_RV): $(TRAP_TEST_OBJECTS) arch/riscv/linker.ld
	$(CC) $(LDFLAGS) -Wl,-Map,$(BUILD_DIR)/tests/kernel-trap-rv.map \
		-o $@ $(TRAP_TEST_OBJECTS)

$(DTB_TEST_KERNEL_RV): $(DTB_TEST_OBJECTS) arch/riscv/linker.ld
	$(CC) $(LDFLAGS) -Wl,-Map,$(BUILD_DIR)/tests/kernel-dtb-rv.map \
		-o $@ $(DTB_TEST_OBJECTS)

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(ASFLAGS) -MMD -MP -c $< -o $@

run-riscv: $(KERNEL_RV)
	$(QEMU_RISCV64) -machine virt -bios default -kernel $< \
		-m $(QEMU_MEMORY) -smp 1 -nographic -no-reboot

debug-riscv: $(KERNEL_RV)
	$(QEMU_RISCV64) -machine virt -bios default -kernel $< \
		-m $(QEMU_MEMORY) -smp 1 -nographic -no-reboot -S -s

test-riscv: test-dtb-riscv $(KERNEL_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) ./tests/boot-riscv.sh

test-dtb-riscv: $(DTB_TEST_KERNEL_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) DTB_TEST_KERNEL_RV=$< \
		./tests/dtb-riscv.sh

test-trap-riscv: $(TRAP_TEST_KERNEL_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) KERNEL_RV=$< ./tests/trap-riscv.sh

clean:
	$(RM) -r -- $(BUILD_DIR) $(KERNEL_RV)

-include $(DEPS)
