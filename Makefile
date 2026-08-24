CROSS_COMPILE ?= $(shell \
	if command -v riscv64-unknown-elf-gcc >/dev/null 2>&1; then \
		printf '%s' riscv64-unknown-elf-; \
	elif command -v riscv64-elf-gcc >/dev/null 2>&1; then \
		printf '%s' riscv64-elf-; \
	else \
		printf '%s' riscv64-unknown-elf-; \
	fi)

CC := $(CROSS_COMPILE)gcc
NM := $(CROSS_COMPILE)nm
OBJDUMP := $(CROSS_COMPILE)objdump
READELF := $(CROSS_COMPILE)readelf
QEMU_RISCV64 ?= qemu-system-riscv64
QEMU_MEMORY ?= 1G

BUILD_DIR := build/riscv
KERNEL_RV := kernel-rv
TRAP_TEST_KERNEL_RV := $(BUILD_DIR)/tests/kernel-trap-rv
TRAP_RETURN_TEST_KERNEL_RV := $(BUILD_DIR)/tests/kernel-trap-return-rv
TRAP_RETURN_SIE_TEST_KERNEL_RV := \
	$(BUILD_DIR)/tests/kernel-trap-return-sie-rv
HIGH_HALF_TRAP_TEST_KERNEL_RV := $(BUILD_DIR)/tests/kernel-high-half-trap-rv
NO_IDENTITY_TEST_KERNEL_RV := $(BUILD_DIR)/tests/kernel-no-identity-rv
DTB_TEST_KERNEL_RV := $(BUILD_DIR)/tests/kernel-dtb-rv
PAGE_TEST_KERNEL_RV := $(BUILD_DIR)/tests/kernel-page-rv
SV39_TEST_KERNEL_RV := $(BUILD_DIR)/tests/kernel-sv39-rv
SV39_FAULT_TEST_KERNEL_RV := $(BUILD_DIR)/tests/kernel-sv39-fault-rv
TIMER_CASES_TEST_KERNEL_RV := \
	$(BUILD_DIR)/tests/kernel-timer-cases-rv
TIMER_BOOT_TEST_KERNEL_RV := $(BUILD_DIR)/tests/kernel-timer-boot-rv
CONTEXT_TEST_KERNEL_RV := $(BUILD_DIR)/tests/kernel-context-rv
SCHEDULER_CASES_TEST_KERNEL_RV := \
	$(BUILD_DIR)/tests/kernel-scheduler-cases-rv
SCHEDULER_BOOT_TEST_KERNEL_RV := \
	$(BUILD_DIR)/tests/kernel-scheduler-boot-rv
SYSCALL_TEST_KERNEL_RV := $(BUILD_DIR)/tests/kernel-syscall-rv
ELF64_TEST_KERNEL_RV := $(BUILD_DIR)/tests/kernel-elf64-rv
USER_ELF_CASES_TEST_KERNEL_RV := \
	$(BUILD_DIR)/tests/kernel-user-elf-cases-rv
USER_ELF_TEST_KERNEL_RV := $(BUILD_DIR)/tests/kernel-user-elf-rv
USER_ELF_PROGRAM_RV := $(BUILD_DIR)/tests/user/elf-probe-rv
USER_ELF_FAULT_PROGRAM_RV := $(BUILD_DIR)/tests/user/elf-text-fault-rv
USER_ELF_GUARD_PROGRAM_RV := $(BUILD_DIR)/tests/user/elf-guard-fault-rv
USER_TEST_KERNEL_RV := $(BUILD_DIR)/tests/kernel-user-rv
USER_FATAL_TEST_KERNEL_RV := $(BUILD_DIR)/tests/kernel-user-fatal-rv

ARCH_FLAGS := -march=rv64imac_zicsr_zifencei -mabi=lp64 -mcmodel=medany
CPPFLAGS := -Iinclude -DBOAROS_PAGE_SHIFT=12
CFLAGS := $(ARCH_FLAGS) -std=gnu11 -O2 -g3 \
	-ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie \
	-ffunction-sections -fdata-sections -Wall -Wextra -Werror
CFLAGS += $(CFLAGS_EXTRA)
ASFLAGS := $(ARCH_FLAGS) -g3
LDFLAGS := $(ARCH_FLAGS) -nostdlib -nostartfiles -static -no-pie \
	-T arch/riscv/linker.ld -Wl,--build-id=none -Wl,--gc-sections

C_SOURCES := \
	arch/riscv/context.c \
	arch/riscv/direct_map.c \
	arch/riscv/sbi.c \
	arch/riscv/sv39.c \
	arch/riscv/timer.c \
	arch/riscv/trap.c \
	arch/riscv/user_elf.c \
	arch/riscv/virt_uart.c \
	kernel/boot_memory.c \
	kernel/dtb.c \
	kernel/elf64.c \
	kernel/main.c \
	kernel/physical_page.c \
	kernel/scheduler.c \
	kernel/syscall.c \
	kernel/tick.c
ASM_SOURCES := \
	arch/riscv/boot.S \
	arch/riscv/context_switch.S \
	arch/riscv/trap_entry.S
OBJECTS := \
	$(patsubst %.c,$(BUILD_DIR)/%.o,$(C_SOURCES)) \
	$(patsubst %.S,$(BUILD_DIR)/%.o,$(ASM_SOURCES))
TEST_RUNTIME_C_SOURCES := \
	arch/riscv/context.c \
	arch/riscv/sbi.c \
	arch/riscv/sv39.c \
	arch/riscv/timer.c \
	arch/riscv/trap.c \
	arch/riscv/user_elf.c \
	arch/riscv/virt_uart.c \
	kernel/elf64.c \
	kernel/physical_page.c \
	kernel/scheduler.c \
	kernel/syscall.c \
	kernel/tick.c
TEST_RUNTIME_ASM_SOURCES := \
	arch/riscv/boot.S \
	arch/riscv/context_switch.S \
	arch/riscv/trap_entry.S
TEST_RUNTIME_OBJECTS := \
	$(patsubst %.c,$(BUILD_DIR)/%.o,$(TEST_RUNTIME_C_SOURCES)) \
	$(patsubst %.S,$(BUILD_DIR)/%.o,$(TEST_RUNTIME_ASM_SOURCES))
TRAP_TEST_C_SOURCES := tests/riscv/trap_main.c
TRAP_TEST_ASM_SOURCES := tests/riscv/trap_trigger.S
TRAP_TEST_OBJECTS := \
	$(TEST_RUNTIME_OBJECTS) \
	$(patsubst %.c,$(BUILD_DIR)/%.o,$(TRAP_TEST_C_SOURCES)) \
	$(patsubst %.S,$(BUILD_DIR)/%.o,$(TRAP_TEST_ASM_SOURCES))
TRAP_RETURN_TEST_C_SOURCES := tests/riscv/trap_return_main.c
TRAP_RETURN_TEST_ASM_SOURCES := tests/riscv/trap_return_trigger.S
TRAP_RETURN_TEST_OBJECTS := \
	$(TEST_RUNTIME_OBJECTS) \
	$(patsubst %.c,$(BUILD_DIR)/%.o,$(TRAP_RETURN_TEST_C_SOURCES)) \
	$(patsubst %.S,$(BUILD_DIR)/%.o,$(TRAP_RETURN_TEST_ASM_SOURCES))
TRAP_RETURN_SIE_TEST_C_SOURCES := tests/riscv/trap_return_sie_main.c
TRAP_RETURN_SIE_TEST_OBJECTS := \
	$(TEST_RUNTIME_OBJECTS) \
	$(patsubst %.c,$(BUILD_DIR)/%.o,$(TRAP_RETURN_SIE_TEST_C_SOURCES)) \
	$(patsubst %.S,$(BUILD_DIR)/%.o,$(TRAP_RETURN_TEST_ASM_SOURCES))
HIGH_HALF_TRAP_TEST_C_SOURCES := tests/riscv/high_half_trap.c
HIGH_HALF_TRAP_TEST_ASM_SOURCES := \
	tests/riscv/trap_trigger.S \
	tests/riscv/trap_return_trigger.S
HIGH_HALF_TRAP_TEST_OBJECTS := \
	$(OBJECTS) \
	$(patsubst %.c,$(BUILD_DIR)/%.o,$(HIGH_HALF_TRAP_TEST_C_SOURCES)) \
	$(patsubst %.S,$(BUILD_DIR)/%.o,$(HIGH_HALF_TRAP_TEST_ASM_SOURCES))
NO_IDENTITY_TEST_C_SOURCES := tests/riscv/no_identity.c
NO_IDENTITY_TEST_OBJECTS := \
	$(OBJECTS) \
	$(patsubst %.c,$(BUILD_DIR)/%.o,$(NO_IDENTITY_TEST_C_SOURCES))
DTB_TEST_C_SOURCES := \
	kernel/boot_memory.c \
	kernel/dtb.c \
	tests/riscv/boot_memory_cases.c \
	tests/riscv/dtb_main.c
DTB_TEST_OBJECTS := \
	$(TEST_RUNTIME_OBJECTS) \
	$(patsubst %.c,$(BUILD_DIR)/%.o,$(DTB_TEST_C_SOURCES))
PAGE_TEST_C_SOURCES := \
	tests/riscv/physical_page_cases.c \
	tests/riscv/physical_page_main.c
PAGE_TEST_OBJECTS := \
	$(TEST_RUNTIME_OBJECTS) \
	$(patsubst %.c,$(BUILD_DIR)/%.o,$(PAGE_TEST_C_SOURCES))
SV39_TEST_C_SOURCES := \
	arch/riscv/direct_map.c \
	tests/riscv/direct_map_cases.c \
	tests/riscv/sv39_cases.c \
	tests/riscv/sv39_main.c
SV39_TEST_OBJECTS := \
	$(TEST_RUNTIME_OBJECTS) \
	$(patsubst %.c,$(BUILD_DIR)/%.o,$(SV39_TEST_C_SOURCES))
SV39_FAULT_TEST_C_SOURCES := \
	kernel/boot_memory.c \
	kernel/dtb.c \
	tests/riscv/sv39_fault_main.c
SV39_FAULT_TEST_OBJECTS := \
	$(TEST_RUNTIME_OBJECTS) \
	$(patsubst %.c,$(BUILD_DIR)/%.o,$(SV39_FAULT_TEST_C_SOURCES))
TIMER_CASES_TEST_C_SOURCES := \
	tests/riscv/timer_cases.c
TIMER_CASES_TEST_OBJECTS := \
	$(TEST_RUNTIME_OBJECTS) \
	$(patsubst %.c,$(BUILD_DIR)/%.o,$(TIMER_CASES_TEST_C_SOURCES))
TIMER_BOOT_TEST_C_SOURCES := \
	tests/riscv/timer_boot.c
TIMER_BOOT_TEST_OBJECTS := \
	$(OBJECTS) \
	$(patsubst %.c,$(BUILD_DIR)/%.o,$(TIMER_BOOT_TEST_C_SOURCES))
CONTEXT_TEST_C_SOURCES := \
	tests/riscv/context_cases.c
CONTEXT_TEST_OBJECTS := \
	$(TEST_RUNTIME_OBJECTS) \
	$(patsubst %.c,$(BUILD_DIR)/%.o,$(CONTEXT_TEST_C_SOURCES))
SCHEDULER_CASES_TEST_C_SOURCES := \
	tests/riscv/scheduler_cases.c
SCHEDULER_CASES_TEST_OBJECTS := \
	$(TEST_RUNTIME_OBJECTS) \
	$(patsubst %.c,$(BUILD_DIR)/%.o,$(SCHEDULER_CASES_TEST_C_SOURCES))
SCHEDULER_BOOT_TEST_C_SOURCES := \
	tests/riscv/scheduler_boot.c
SCHEDULER_BOOT_TEST_ASM_SOURCES := \
	tests/riscv/scheduler_workers.S
SCHEDULER_BOOT_TEST_OBJECTS := \
	$(OBJECTS) \
	$(patsubst %.c,$(BUILD_DIR)/%.o,$(SCHEDULER_BOOT_TEST_C_SOURCES)) \
	$(patsubst %.S,$(BUILD_DIR)/%.o,$(SCHEDULER_BOOT_TEST_ASM_SOURCES))
SYSCALL_TEST_C_SOURCES := \
	tests/riscv/syscall_cases.c \
	tests/riscv/syscall_main.c
SYSCALL_TEST_OBJECTS := \
	$(TEST_RUNTIME_OBJECTS) \
	$(patsubst %.c,$(BUILD_DIR)/%.o,$(SYSCALL_TEST_C_SOURCES))
ELF64_TEST_C_SOURCES := \
	tests/riscv/elf64_cases.c \
	tests/riscv/elf64_main.c
ELF64_TEST_OBJECTS := \
	$(TEST_RUNTIME_OBJECTS) \
	$(patsubst %.c,$(BUILD_DIR)/%.o,$(ELF64_TEST_C_SOURCES))
USER_ELF_CASES_TEST_C_SOURCES := \
	tests/riscv/user_elf_cases.c \
	tests/riscv/user_elf_cases_main.c
USER_ELF_CASES_TEST_OBJECTS := \
	$(TEST_RUNTIME_OBJECTS) \
	$(patsubst %.c,$(BUILD_DIR)/%.o,$(USER_ELF_CASES_TEST_C_SOURCES))
USER_ELF_TEST_C_SOURCES := tests/riscv/user_elf_boot.c
USER_ELF_TEST_ASM_SOURCES := tests/riscv/user_elf_images.S
USER_ELF_TEST_OBJECTS := \
	$(OBJECTS) \
	$(patsubst %.c,$(BUILD_DIR)/%.o,$(USER_ELF_TEST_C_SOURCES)) \
	$(patsubst %.S,$(BUILD_DIR)/%.o,$(USER_ELF_TEST_ASM_SOURCES))
USER_ELF_PROGRAM_OBJECT_RV := \
	$(BUILD_DIR)/tests/user/user_elf_program.o
USER_ELF_FAULT_PROGRAM_OBJECT_RV := \
	$(BUILD_DIR)/tests/user/user_elf_fault_program.o
USER_ELF_GUARD_PROGRAM_OBJECT_RV := \
	$(BUILD_DIR)/tests/user/user_elf_guard_program.o
USER_TEST_C_SOURCES := tests/riscv/user_boot.c
USER_TEST_ASM_SOURCES := tests/riscv/user_payload.S
USER_TEST_OBJECTS := \
	$(OBJECTS) \
	$(patsubst %.c,$(BUILD_DIR)/%.o,$(USER_TEST_C_SOURCES)) \
	$(patsubst %.S,$(BUILD_DIR)/%.o,$(USER_TEST_ASM_SOURCES))
USER_FATAL_TEST_C_SOURCES := tests/riscv/user_bad_return.c
USER_FATAL_TEST_OBJECTS := \
	$(USER_TEST_OBJECTS) \
	$(patsubst %.c,$(BUILD_DIR)/%.o,$(USER_FATAL_TEST_C_SOURCES))
DEPS := \
	$(OBJECTS:.o=.d) \
	$(TRAP_TEST_OBJECTS:.o=.d) \
	$(TRAP_RETURN_TEST_OBJECTS:.o=.d) \
	$(TRAP_RETURN_SIE_TEST_OBJECTS:.o=.d) \
	$(HIGH_HALF_TRAP_TEST_OBJECTS:.o=.d) \
	$(NO_IDENTITY_TEST_OBJECTS:.o=.d) \
	$(DTB_TEST_OBJECTS:.o=.d) \
	$(PAGE_TEST_OBJECTS:.o=.d) \
	$(SV39_TEST_OBJECTS:.o=.d) \
	$(SV39_FAULT_TEST_OBJECTS:.o=.d) \
	$(TIMER_CASES_TEST_OBJECTS:.o=.d) \
	$(TIMER_BOOT_TEST_OBJECTS:.o=.d) \
	$(CONTEXT_TEST_OBJECTS:.o=.d) \
	$(SCHEDULER_CASES_TEST_OBJECTS:.o=.d) \
	$(SCHEDULER_BOOT_TEST_OBJECTS:.o=.d) \
	$(SYSCALL_TEST_OBJECTS:.o=.d) \
	$(ELF64_TEST_OBJECTS:.o=.d) \
	$(USER_ELF_CASES_TEST_OBJECTS:.o=.d) \
	$(USER_ELF_TEST_OBJECTS:.o=.d) \
	$(USER_ELF_PROGRAM_OBJECT_RV:.o=.d) \
	$(USER_ELF_FAULT_PROGRAM_OBJECT_RV:.o=.d) \
	$(USER_ELF_GUARD_PROGRAM_OBJECT_RV:.o=.d) \
	$(USER_TEST_OBJECTS:.o=.d) \
	$(USER_FATAL_TEST_OBJECTS:.o=.d)

.PHONY: all clean debug-riscv references run-riscv test-dtb-riscv \
	test-context-riscv \
	test-elf64-riscv test-user-elf-cases-riscv test-user-elf-riscv \
	test-high-half-trap-riscv test-idle-riscv test-no-identity-riscv \
	test-page-riscv test-scheduler-cases-riscv test-scheduler-riscv \
	test-references test-riscv test-sv39-fault-riscv test-sv39-riscv \
	test-syscall-riscv test-timer-riscv test-trap-riscv \
	test-trap-return-riscv test-user-fatal-riscv test-user-riscv

all: $(KERNEL_RV)

references:
	./references/fetch.sh

test-references:
	./tests/references.sh

$(KERNEL_RV): $(OBJECTS) arch/riscv/linker.ld
	$(CC) $(LDFLAGS) -Wl,-Map,$(BUILD_DIR)/kernel-rv.map \
		-o $@ $(OBJECTS)

$(TRAP_TEST_KERNEL_RV): $(TRAP_TEST_OBJECTS) arch/riscv/linker.ld
	$(CC) $(LDFLAGS) -Wl,-Map,$(BUILD_DIR)/tests/kernel-trap-rv.map \
		-o $@ $(TRAP_TEST_OBJECTS)

$(TRAP_RETURN_TEST_KERNEL_RV): $(TRAP_RETURN_TEST_OBJECTS) \
		arch/riscv/linker.ld
	$(CC) $(LDFLAGS) -Wl,--wrap=riscv_trap_dispatch \
		-Wl,-Map,$(BUILD_DIR)/tests/kernel-trap-return-rv.map \
		-o $@ $(TRAP_RETURN_TEST_OBJECTS)

$(TRAP_RETURN_SIE_TEST_KERNEL_RV): $(TRAP_RETURN_SIE_TEST_OBJECTS) \
		arch/riscv/linker.ld
	$(CC) $(LDFLAGS) -Wl,--wrap=riscv_trap_dispatch \
		-Wl,-Map,$(BUILD_DIR)/tests/kernel-trap-return-sie-rv.map \
		-o $@ $(TRAP_RETURN_SIE_TEST_OBJECTS)

$(HIGH_HALF_TRAP_TEST_KERNEL_RV): $(HIGH_HALF_TRAP_TEST_OBJECTS) \
		arch/riscv/linker.ld
	$(CC) $(LDFLAGS) -Wl,--wrap=riscv_timer_start \
		-Wl,--wrap=riscv_trap_dispatch \
		-Wl,-Map,$(BUILD_DIR)/tests/kernel-high-half-trap-rv.map \
		-o $@ $(HIGH_HALF_TRAP_TEST_OBJECTS)

$(NO_IDENTITY_TEST_KERNEL_RV): $(NO_IDENTITY_TEST_OBJECTS) \
		arch/riscv/linker.ld
	$(CC) $(LDFLAGS) -Wl,--wrap=riscv_timer_start \
		-Wl,-Map,$(BUILD_DIR)/tests/kernel-no-identity-rv.map \
		-o $@ $(NO_IDENTITY_TEST_OBJECTS)

$(DTB_TEST_KERNEL_RV): $(DTB_TEST_OBJECTS) arch/riscv/linker.ld
	$(CC) $(LDFLAGS) -Wl,-Map,$(BUILD_DIR)/tests/kernel-dtb-rv.map \
		-o $@ $(DTB_TEST_OBJECTS)

$(PAGE_TEST_KERNEL_RV): $(PAGE_TEST_OBJECTS) arch/riscv/linker.ld
	$(CC) $(LDFLAGS) -Wl,-Map,$(BUILD_DIR)/tests/kernel-page-rv.map \
		-o $@ $(PAGE_TEST_OBJECTS)

$(SV39_TEST_KERNEL_RV): $(SV39_TEST_OBJECTS) arch/riscv/linker.ld
	$(CC) $(LDFLAGS) -Wl,-Map,$(BUILD_DIR)/tests/kernel-sv39-rv.map \
		-o $@ $(SV39_TEST_OBJECTS)

$(SV39_FAULT_TEST_KERNEL_RV): $(SV39_FAULT_TEST_OBJECTS) \
		arch/riscv/linker.ld
	$(CC) $(LDFLAGS) \
		-Wl,-Map,$(BUILD_DIR)/tests/kernel-sv39-fault-rv.map \
		-o $@ $(SV39_FAULT_TEST_OBJECTS)

$(TIMER_CASES_TEST_KERNEL_RV): $(TIMER_CASES_TEST_OBJECTS) \
		arch/riscv/linker.ld
	$(CC) $(LDFLAGS) -Wl,--wrap=sbi_probe_extension \
		-Wl,--wrap=sbi_set_timer \
		-Wl,-Map,$(BUILD_DIR)/tests/kernel-timer-cases-rv.map \
		-o $@ $(TIMER_CASES_TEST_OBJECTS)

$(TIMER_BOOT_TEST_KERNEL_RV): $(TIMER_BOOT_TEST_OBJECTS) \
		arch/riscv/linker.ld
	$(CC) $(LDFLAGS) -Wl,--wrap=kernel_tick_advance \
		-Wl,-Map,$(BUILD_DIR)/tests/kernel-timer-boot-rv.map \
		-o $@ $(TIMER_BOOT_TEST_OBJECTS)

$(CONTEXT_TEST_KERNEL_RV): $(CONTEXT_TEST_OBJECTS) arch/riscv/linker.ld
	$(CC) $(LDFLAGS) \
		-Wl,-Map,$(BUILD_DIR)/tests/kernel-context-rv.map \
		-o $@ $(CONTEXT_TEST_OBJECTS)

$(SCHEDULER_CASES_TEST_KERNEL_RV): $(SCHEDULER_CASES_TEST_OBJECTS) \
		arch/riscv/linker.ld
	$(CC) $(LDFLAGS) \
		-Wl,-Map,$(BUILD_DIR)/tests/kernel-scheduler-cases-rv.map \
		-o $@ $(SCHEDULER_CASES_TEST_OBJECTS)

$(SCHEDULER_BOOT_TEST_KERNEL_RV): $(SCHEDULER_BOOT_TEST_OBJECTS) \
		arch/riscv/linker.ld
	$(CC) $(LDFLAGS) -Wl,--wrap=kernel_scheduler_init \
		-Wl,--wrap=kernel_tick_advance \
		-Wl,-Map,$(BUILD_DIR)/tests/kernel-scheduler-boot-rv.map \
		-o $@ $(SCHEDULER_BOOT_TEST_OBJECTS)

$(SYSCALL_TEST_KERNEL_RV): $(SYSCALL_TEST_OBJECTS) arch/riscv/linker.ld
	$(CC) $(LDFLAGS) \
		-Wl,-Map,$(BUILD_DIR)/tests/kernel-syscall-rv.map \
		-o $@ $(SYSCALL_TEST_OBJECTS)

$(ELF64_TEST_KERNEL_RV): $(ELF64_TEST_OBJECTS) arch/riscv/linker.ld
	$(CC) $(LDFLAGS) \
		-Wl,-Map,$(BUILD_DIR)/tests/kernel-elf64-rv.map \
		-o $@ $(ELF64_TEST_OBJECTS)

$(USER_ELF_CASES_TEST_KERNEL_RV): $(USER_ELF_CASES_TEST_OBJECTS) \
		arch/riscv/linker.ld
	$(CC) $(LDFLAGS) -Wl,--wrap=riscv_sv39_user_space_destroy \
		-Wl,-Map,$(BUILD_DIR)/tests/kernel-user-elf-cases-rv.map \
		-o $@ $(USER_ELF_CASES_TEST_OBJECTS)

$(USER_ELF_PROGRAM_OBJECT_RV): tests/riscv/user_elf_program.S
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(ASFLAGS) -MMD -MP -c $< -o $@

$(USER_ELF_FAULT_PROGRAM_OBJECT_RV): tests/riscv/user_elf_program.S
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(ASFLAGS) -DUSER_ELF_TEXT_FAULT \
		-MMD -MP -c $< -o $@

$(USER_ELF_GUARD_PROGRAM_OBJECT_RV): tests/riscv/user_elf_program.S
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(ASFLAGS) -DUSER_ELF_GUARD_FAULT \
		-MMD -MP -c $< -o $@

$(USER_ELF_PROGRAM_RV): $(USER_ELF_PROGRAM_OBJECT_RV) \
		tests/riscv/user_elf.ld
	$(CC) $(ARCH_FLAGS) -nostdlib -nostartfiles -static -no-pie \
		-T tests/riscv/user_elf.ld -Wl,--build-id=none \
		-Wl,--gc-sections -o $@ $(USER_ELF_PROGRAM_OBJECT_RV)

$(USER_ELF_FAULT_PROGRAM_RV): $(USER_ELF_FAULT_PROGRAM_OBJECT_RV) \
		tests/riscv/user_elf.ld
	$(CC) $(ARCH_FLAGS) -nostdlib -nostartfiles -static -no-pie \
		-T tests/riscv/user_elf.ld -Wl,--build-id=none \
		-Wl,--gc-sections -o $@ $(USER_ELF_FAULT_PROGRAM_OBJECT_RV)

$(USER_ELF_GUARD_PROGRAM_RV): $(USER_ELF_GUARD_PROGRAM_OBJECT_RV) \
		tests/riscv/user_elf.ld
	$(CC) $(ARCH_FLAGS) -nostdlib -nostartfiles -static -no-pie \
		-T tests/riscv/user_elf.ld -Wl,--build-id=none \
		-Wl,--gc-sections -o $@ $(USER_ELF_GUARD_PROGRAM_OBJECT_RV)

$(BUILD_DIR)/tests/riscv/user_elf_images.o: \
		tests/riscv/user_elf_images.S \
		$(USER_ELF_PROGRAM_RV) $(USER_ELF_FAULT_PROGRAM_RV) \
		$(USER_ELF_GUARD_PROGRAM_RV)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(ASFLAGS) -MMD -MP -c $< -o $@

$(USER_ELF_TEST_KERNEL_RV): $(USER_ELF_TEST_OBJECTS) arch/riscv/linker.ld
	$(CC) $(LDFLAGS) -Wl,--wrap=riscv_sv39_activate \
		-Wl,--wrap=kernel_scheduler_init \
		-Wl,--wrap=kernel_scheduler_reap_one \
		-Wl,--wrap=kernel_tick_advance \
		-Wl,-Map,$(BUILD_DIR)/tests/kernel-user-elf-rv.map \
		-o $@ $(USER_ELF_TEST_OBJECTS)

$(USER_TEST_KERNEL_RV): $(USER_TEST_OBJECTS) arch/riscv/linker.ld
	$(CC) $(LDFLAGS) -Wl,--wrap=riscv_sv39_activate \
		-Wl,--wrap=kernel_scheduler_init \
		-Wl,--wrap=kernel_scheduler_reap_one \
		-Wl,--wrap=kernel_tick_advance \
		-Wl,-Map,$(BUILD_DIR)/tests/kernel-user-rv.map \
		-o $@ $(USER_TEST_OBJECTS)

$(USER_FATAL_TEST_KERNEL_RV): $(USER_FATAL_TEST_OBJECTS) \
		arch/riscv/linker.ld
	$(CC) $(LDFLAGS) -Wl,--wrap=riscv_sv39_activate \
		-Wl,--wrap=kernel_scheduler_init \
		-Wl,--wrap=kernel_scheduler_reap_one \
		-Wl,--wrap=kernel_tick_advance \
		-Wl,--wrap=riscv_trap_dispatch \
		-Wl,-Map,$(BUILD_DIR)/tests/kernel-user-fatal-rv.map \
		-o $@ $(USER_FATAL_TEST_OBJECTS)

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

test-riscv: $(DTB_TEST_KERNEL_RV) $(PAGE_TEST_KERNEL_RV) \
	$(CONTEXT_TEST_KERNEL_RV) $(SCHEDULER_CASES_TEST_KERNEL_RV) \
	$(SCHEDULER_BOOT_TEST_KERNEL_RV) \
	$(SYSCALL_TEST_KERNEL_RV) $(ELF64_TEST_KERNEL_RV) \
	$(USER_ELF_CASES_TEST_KERNEL_RV) $(USER_ELF_TEST_KERNEL_RV) \
	$(USER_TEST_KERNEL_RV) $(USER_FATAL_TEST_KERNEL_RV) \
	$(SV39_TEST_KERNEL_RV) $(SV39_FAULT_TEST_KERNEL_RV) \
	$(TRAP_RETURN_TEST_KERNEL_RV) $(TRAP_RETURN_SIE_TEST_KERNEL_RV) \
	$(HIGH_HALF_TRAP_TEST_KERNEL_RV) $(NO_IDENTITY_TEST_KERNEL_RV) \
	$(TIMER_CASES_TEST_KERNEL_RV) $(TIMER_BOOT_TEST_KERNEL_RV) \
	$(KERNEL_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) \
		DTB_TEST_KERNEL_RV=$(DTB_TEST_KERNEL_RV) ./tests/dtb-riscv.sh
	QEMU_RISCV64=$(QEMU_RISCV64) \
		PAGE_TEST_KERNEL_RV=$(PAGE_TEST_KERNEL_RV) ./tests/page-riscv.sh
	QEMU_RISCV64=$(QEMU_RISCV64) OBJDUMP_RV=$(OBJDUMP) \
		CONTEXT_TEST_KERNEL_RV=$(CONTEXT_TEST_KERNEL_RV) \
		CONTEXT_OBJECT_RV=$(BUILD_DIR)/arch/riscv/context_switch.o \
		./tests/context-riscv.sh
	QEMU_RISCV64=$(QEMU_RISCV64) \
		SCHEDULER_CASES_TEST_KERNEL_RV=$(SCHEDULER_CASES_TEST_KERNEL_RV) \
		./tests/scheduler-cases-riscv.sh
	QEMU_RISCV64=$(QEMU_RISCV64) \
		SCHEDULER_BOOT_TEST_KERNEL_RV=$(SCHEDULER_BOOT_TEST_KERNEL_RV) \
		./tests/scheduler-riscv.sh
	QEMU_RISCV64=$(QEMU_RISCV64) \
		SYSCALL_TEST_KERNEL_RV=$(SYSCALL_TEST_KERNEL_RV) \
		./tests/syscall-riscv.sh
	QEMU_RISCV64=$(QEMU_RISCV64) \
		ELF64_TEST_KERNEL_RV=$(ELF64_TEST_KERNEL_RV) \
		./tests/elf64-riscv.sh
	QEMU_RISCV64=$(QEMU_RISCV64) \
		USER_ELF_CASES_TEST_KERNEL_RV=$(USER_ELF_CASES_TEST_KERNEL_RV) \
		./tests/user-elf-cases-riscv.sh
	QEMU_RISCV64=$(QEMU_RISCV64) READELF_RV=$(READELF) \
		USER_ELF_TEST_KERNEL_RV=$(USER_ELF_TEST_KERNEL_RV) \
		USER_ELF_PROGRAM_RV=$(USER_ELF_PROGRAM_RV) \
		USER_ELF_FAULT_PROGRAM_RV=$(USER_ELF_FAULT_PROGRAM_RV) \
		USER_ELF_GUARD_PROGRAM_RV=$(USER_ELF_GUARD_PROGRAM_RV) \
		./tests/user-elf-riscv.sh
	QEMU_RISCV64=$(QEMU_RISCV64) USER_TEST_KERNEL_RV=$(USER_TEST_KERNEL_RV) \
		./tests/user-riscv.sh
	QEMU_RISCV64=$(QEMU_RISCV64) \
		USER_FATAL_TEST_KERNEL_RV=$(USER_FATAL_TEST_KERNEL_RV) \
		./tests/user-fatal-riscv.sh
	QEMU_RISCV64=$(QEMU_RISCV64) \
		SV39_TEST_KERNEL_RV=$(SV39_TEST_KERNEL_RV) ./tests/sv39-riscv.sh
	QEMU_RISCV64=$(QEMU_RISCV64) \
		SV39_FAULT_TEST_KERNEL_RV=$(SV39_FAULT_TEST_KERNEL_RV) \
		./tests/sv39-fault-riscv.sh
	QEMU_RISCV64=$(QEMU_RISCV64) \
		TRAP_RETURN_TEST_KERNEL_RV=$(TRAP_RETURN_TEST_KERNEL_RV) \
		TRAP_RETURN_SIE_TEST_KERNEL_RV=$(TRAP_RETURN_SIE_TEST_KERNEL_RV) \
		OBJDUMP_RV=$(OBJDUMP) \
		./tests/trap-return-riscv.sh
	QEMU_RISCV64=$(QEMU_RISCV64) \
		TIMER_CASES_TEST_KERNEL_RV=$(TIMER_CASES_TEST_KERNEL_RV) \
		TIMER_BOOT_TEST_KERNEL_RV=$(TIMER_BOOT_TEST_KERNEL_RV) \
		./tests/timer-riscv.sh
	QEMU_RISCV64=$(QEMU_RISCV64) NM_RV=$(NM) READELF_RV=$(READELF) \
		BOOT_TEST_KERNEL_RV=$(TIMER_BOOT_TEST_KERNEL_RV) \
		./tests/boot-riscv.sh
	QEMU_RISCV64=$(QEMU_RISCV64) NM_RV=$(NM) \
		HIGH_HALF_TRAP_TEST_KERNEL_RV=$(HIGH_HALF_TRAP_TEST_KERNEL_RV) \
		./tests/high-half-trap-riscv.sh
	QEMU_RISCV64=$(QEMU_RISCV64) \
		NO_IDENTITY_TEST_KERNEL_RV=$(NO_IDENTITY_TEST_KERNEL_RV) \
		./tests/no-identity-riscv.sh
	QEMU_RISCV64=$(QEMU_RISCV64) KERNEL_RV=$(KERNEL_RV) \
		./tests/idle-riscv.sh

test-dtb-riscv: $(DTB_TEST_KERNEL_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) DTB_TEST_KERNEL_RV=$< \
		./tests/dtb-riscv.sh

test-page-riscv: $(PAGE_TEST_KERNEL_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) PAGE_TEST_KERNEL_RV=$< \
		./tests/page-riscv.sh

test-context-riscv: $(CONTEXT_TEST_KERNEL_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) OBJDUMP_RV=$(OBJDUMP) \
		CONTEXT_TEST_KERNEL_RV=$< \
		CONTEXT_OBJECT_RV=$(BUILD_DIR)/arch/riscv/context_switch.o \
		./tests/context-riscv.sh

test-scheduler-cases-riscv: $(SCHEDULER_CASES_TEST_KERNEL_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) SCHEDULER_CASES_TEST_KERNEL_RV=$< \
		./tests/scheduler-cases-riscv.sh

test-scheduler-riscv: $(SCHEDULER_BOOT_TEST_KERNEL_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) SCHEDULER_BOOT_TEST_KERNEL_RV=$< \
		./tests/scheduler-riscv.sh

test-syscall-riscv: $(SYSCALL_TEST_KERNEL_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) SYSCALL_TEST_KERNEL_RV=$< \
		./tests/syscall-riscv.sh

test-elf64-riscv: $(ELF64_TEST_KERNEL_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) ELF64_TEST_KERNEL_RV=$< \
		./tests/elf64-riscv.sh

test-user-elf-cases-riscv: $(USER_ELF_CASES_TEST_KERNEL_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) USER_ELF_CASES_TEST_KERNEL_RV=$< \
		./tests/user-elf-cases-riscv.sh

test-user-elf-riscv: $(USER_ELF_TEST_KERNEL_RV) \
		$(USER_ELF_PROGRAM_RV) $(USER_ELF_FAULT_PROGRAM_RV) \
		$(USER_ELF_GUARD_PROGRAM_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) READELF_RV=$(READELF) \
		USER_ELF_TEST_KERNEL_RV=$(USER_ELF_TEST_KERNEL_RV) \
		USER_ELF_PROGRAM_RV=$(USER_ELF_PROGRAM_RV) \
		USER_ELF_FAULT_PROGRAM_RV=$(USER_ELF_FAULT_PROGRAM_RV) \
		USER_ELF_GUARD_PROGRAM_RV=$(USER_ELF_GUARD_PROGRAM_RV) \
		./tests/user-elf-riscv.sh

test-user-riscv: $(USER_TEST_KERNEL_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) USER_TEST_KERNEL_RV=$< \
		./tests/user-riscv.sh

test-user-fatal-riscv: $(USER_FATAL_TEST_KERNEL_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) USER_FATAL_TEST_KERNEL_RV=$< \
		./tests/user-fatal-riscv.sh

test-sv39-riscv: $(SV39_TEST_KERNEL_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) SV39_TEST_KERNEL_RV=$< \
		./tests/sv39-riscv.sh

test-sv39-fault-riscv: $(SV39_FAULT_TEST_KERNEL_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) SV39_FAULT_TEST_KERNEL_RV=$< \
		./tests/sv39-fault-riscv.sh

test-timer-riscv: $(TIMER_CASES_TEST_KERNEL_RV) \
		$(TIMER_BOOT_TEST_KERNEL_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) TIMER_CASES_TEST_KERNEL_RV=$< \
		TIMER_BOOT_TEST_KERNEL_RV=$(TIMER_BOOT_TEST_KERNEL_RV) \
		./tests/timer-riscv.sh

test-idle-riscv: $(KERNEL_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) KERNEL_RV=$< ./tests/idle-riscv.sh

test-trap-riscv: $(TRAP_TEST_KERNEL_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) KERNEL_RV=$< ./tests/trap-riscv.sh

test-trap-return-riscv: $(TRAP_RETURN_TEST_KERNEL_RV) \
		$(TRAP_RETURN_SIE_TEST_KERNEL_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) TRAP_RETURN_TEST_KERNEL_RV=$< \
		TRAP_RETURN_SIE_TEST_KERNEL_RV=$(TRAP_RETURN_SIE_TEST_KERNEL_RV) \
		OBJDUMP_RV=$(OBJDUMP) \
		./tests/trap-return-riscv.sh

test-high-half-trap-riscv: $(HIGH_HALF_TRAP_TEST_KERNEL_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) NM_RV=$(NM) \
		HIGH_HALF_TRAP_TEST_KERNEL_RV=$< ./tests/high-half-trap-riscv.sh

test-no-identity-riscv: $(NO_IDENTITY_TEST_KERNEL_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) NO_IDENTITY_TEST_KERNEL_RV=$< \
		./tests/no-identity-riscv.sh

clean:
	$(RM) -r -- $(BUILD_DIR) $(KERNEL_RV)

-include $(DEPS)
