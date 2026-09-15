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
HEAP_TEST_KERNEL_RV := $(BUILD_DIR)/tests/kernel-heap-rv
BLOCK_TEST_KERNEL_RV := $(BUILD_DIR)/tests/kernel-block-rv
VFS_TEST_KERNEL_RV := $(BUILD_DIR)/tests/kernel-vfs-rv
VFS_RECOVERY_TEST_KERNEL_RV := \
	$(BUILD_DIR)/tests/kernel-vfs-recovery-rv
FILES_TEST_KERNEL_RV := $(BUILD_DIR)/tests/kernel-files-rv
FILES_PARTIAL_WRITE_TEST_KERNEL_RV := \
	$(BUILD_DIR)/tests/kernel-files-partial-write-rv
SV39_TEST_KERNEL_RV := $(BUILD_DIR)/tests/kernel-sv39-rv
SV39_FAULT_TEST_KERNEL_RV := $(BUILD_DIR)/tests/kernel-sv39-fault-rv
MM_TEST_KERNEL_RV := $(BUILD_DIR)/tests/kernel-mm-rv
MM_FATAL_TEST_KERNEL_RV := $(BUILD_DIR)/tests/kernel-mm-fatal-rv
VMA_TEST_KERNEL_RV := $(BUILD_DIR)/tests/kernel-vma-rv
UACCESS_TEST_KERNEL_RV := $(BUILD_DIR)/tests/kernel-uaccess-rv
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
ROOT_INIT_PROGRAM_RV := $(BUILD_DIR)/tests/user/root-init-rv
UACCESS_OOM_PROGRAM_RV := $(BUILD_DIR)/tests/user/uaccess-oom-rv
ROOT_EXEC_STAGE2_RV := $(BUILD_DIR)/tests/user/root-exec-stage2-rv
ROOT_EXEC_STAGE3_RV := $(BUILD_DIR)/tests/user/root-exec-stage3-rv
ROOT_EXEC_STAGE3_OOM_RV := \
	$(BUILD_DIR)/tests/user/root-exec-stage3-oom-rv
DEMAND_PAGE_OOM_TEST_KERNEL_RV := \
	$(BUILD_DIR)/tests/kernel-demand-page-oom-rv
ROOT_BOOT_CLEANUP_TEST_KERNEL_RV := \
	$(BUILD_DIR)/tests/kernel-root-boot-cleanup-rv
USER_TEST_KERNEL_RV := $(BUILD_DIR)/tests/kernel-user-rv
USER_FATAL_TEST_KERNEL_RV := $(BUILD_DIR)/tests/kernel-user-fatal-rv

ARCH_FLAGS := -march=rv64imac_zicsr_zifencei -mabi=lp64 -mcmodel=medany
CPPFLAGS := -Iinclude -DBOAROS_PAGE_SHIFT=12 \
	-DBOAROS_UTS_MACHINE=\"riscv64\"
CFLAGS := $(ARCH_FLAGS) -std=gnu11 -O2 -g3 \
	-ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie \
	-ffunction-sections -fdata-sections -Wall -Wextra -Werror
CFLAGS += $(CFLAGS_EXTRA)
ASFLAGS := $(ARCH_FLAGS) -g3
LDFLAGS := $(ARCH_FLAGS) -nostdlib -nostartfiles -static -no-pie \
	-T arch/riscv/linker.ld -Wl,--build-id=none -Wl,--gc-sections

LWEXT4_CPPFLAGS := -Ifs/lwext4_config -Ithird_party/lwext4/include \
	-DCONFIG_USE_DEFAULT_CFG=0
LWEXT4_SOURCES := \
	third_party/lwext4/src/ext4.c \
	third_party/lwext4/src/ext4_balloc.c \
	third_party/lwext4/src/ext4_bcache.c \
	third_party/lwext4/src/ext4_bitmap.c \
	third_party/lwext4/src/ext4_block_group.c \
	third_party/lwext4/src/ext4_blockdev.c \
	third_party/lwext4/src/ext4_crc32.c \
	third_party/lwext4/src/ext4_debug.c \
	third_party/lwext4/src/ext4_dir.c \
	third_party/lwext4/src/ext4_dir_idx.c \
	third_party/lwext4/src/ext4_extent.c \
	third_party/lwext4/src/ext4_fs.c \
	third_party/lwext4/src/ext4_hash.c \
	third_party/lwext4/src/ext4_ialloc.c \
	third_party/lwext4/src/ext4_inode.c \
	third_party/lwext4/src/ext4_super.c \
	third_party/lwext4/src/ext4_trans.c

C_SOURCES := \
	arch/riscv/context.c \
	arch/riscv/direct_map.c \
	arch/riscv/elf_image.c \
	arch/riscv/exec.c \
	arch/riscv/process.c \
	arch/riscv/mm.c \
	arch/riscv/root_boot.c \
	arch/riscv/sbi.c \
	arch/riscv/sv39.c \
	arch/riscv/timer.c \
	arch/riscv/trap.c \
	arch/riscv/uaccess.c \
	arch/riscv/virt_rtc.c \
	arch/riscv/virt_uart.c \
	arch/riscv/virtio_mmio_block.c \
	fs/lwext4_port.c \
	fs/files/table.c \
	fs/files/io.c \
	fs/files/path.c \
	fs/files/console.c \
	fs/files/poll.c \
	fs/files/epoll.c \
	fs/fs_context.c \
	fs/open_file.c \
	fs/pipe.c \
	fs/page_cache.c \
	fs/vfs.c \
	kernel/boot_memory.c \
	kernel/block.c \
	kernel/dtb.c \
	kernel/elf64.c \
	kernel/elf64_source.c \
	kernel/exec.c \
	kernel/main.c \
	kernel/pid.c \
	kernel/physical_page.c \
	kernel/read_source.c \
	kernel/random.c \
	kernel/sched/core.c \
	kernel/sched/exec.c \
	kernel/sched/process.c \
	kernel/sched/signal.c \
	kernel/sched/wait.c \
	kernel/sched/futex.c \
	kernel/syscall/dispatch.c \
	kernel/syscall/file.c \
	kernel/syscall/memory.c \
	kernel/syscall/process.c \
	kernel/syscall/signal.c \
	kernel/syscall/time.c \
	arch/riscv/signal.c \
	kernel/tick.c \
	kernel/time.c \
	lib/qsort.c \
	lib/string.c \
	mm/vma.c \
	mm/heap.c \
	$(LWEXT4_SOURCES)
ASM_SOURCES := \
	arch/riscv/boot.S \
	arch/riscv/context_switch.S \
	arch/riscv/fpu.S \
	arch/riscv/trap_entry.S
OBJECTS := \
	$(patsubst %.c,$(BUILD_DIR)/%.o,$(C_SOURCES)) \
	$(patsubst %.S,$(BUILD_DIR)/%.o,$(ASM_SOURCES))
TEST_RUNTIME_C_SOURCES := \
	arch/riscv/context.c \
	arch/riscv/direct_map.c \
	arch/riscv/elf_image.c \
	arch/riscv/exec.c \
	arch/riscv/process.c \
	arch/riscv/mm.c \
	arch/riscv/sbi.c \
	arch/riscv/sv39.c \
	arch/riscv/timer.c \
	arch/riscv/trap.c \
	arch/riscv/uaccess.c \
	arch/riscv/virt_rtc.c \
	arch/riscv/virt_uart.c \
	arch/riscv/virtio_mmio_block.c \
	fs/files/table.c \
	fs/files/io.c \
	fs/files/path.c \
	fs/files/console.c \
	fs/files/poll.c \
	fs/files/epoll.c \
	fs/fs_context.c \
	fs/lwext4_port.c \
	fs/open_file.c \
	fs/pipe.c \
	fs/page_cache.c \
	fs/vfs.c \
	kernel/block.c \
	kernel/elf64.c \
	kernel/elf64_source.c \
	kernel/exec.c \
	kernel/pid.c \
	kernel/physical_page.c \
	kernel/read_source.c \
	kernel/random.c \
	kernel/sched/core.c \
	kernel/sched/exec.c \
	kernel/sched/process.c \
	kernel/sched/signal.c \
	kernel/sched/wait.c \
	kernel/sched/futex.c \
	kernel/syscall/dispatch.c \
	kernel/syscall/file.c \
	kernel/syscall/memory.c \
	kernel/syscall/process.c \
	kernel/syscall/signal.c \
	kernel/syscall/time.c \
	arch/riscv/signal.c \
	kernel/tick.c \
	kernel/time.c \
	lib/qsort.c \
	lib/string.c \
	mm/vma.c \
	mm/heap.c \
	$(LWEXT4_SOURCES)
TEST_RUNTIME_ASM_SOURCES := \
	arch/riscv/boot.S \
	arch/riscv/context_switch.S \
	arch/riscv/fpu.S \
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
HEAP_TEST_C_SOURCES := \
	tests/riscv/heap_cases.c \
	tests/riscv/heap_main.c
HEAP_TEST_OBJECTS := \
	$(TEST_RUNTIME_OBJECTS) \
	$(patsubst %.c,$(BUILD_DIR)/%.o,$(HEAP_TEST_C_SOURCES))
BLOCK_TEST_C_SOURCES := \
	kernel/dtb.c \
	tests/riscv/block_main.c
BLOCK_TEST_OBJECTS := \
	$(TEST_RUNTIME_OBJECTS) \
	$(patsubst %.c,$(BUILD_DIR)/%.o,$(BLOCK_TEST_C_SOURCES))
VFS_TEST_SUPPORT_C_SOURCES := \
	kernel/dtb.c
VFS_TEST_C_SOURCES := \
	$(VFS_TEST_SUPPORT_C_SOURCES) \
	tests/riscv/vfs_main.c
VFS_TEST_OBJECTS := \
	$(TEST_RUNTIME_OBJECTS) \
	$(patsubst %.c,$(BUILD_DIR)/%.o,$(VFS_TEST_C_SOURCES))
VFS_RECOVERY_TEST_MAIN_OBJECT := \
	$(BUILD_DIR)/tests/riscv/vfs_recovery_main.o
VFS_RECOVERY_TEST_OBJECTS := \
	$(TEST_RUNTIME_OBJECTS) \
	$(patsubst %.c,$(BUILD_DIR)/%.o,$(VFS_TEST_SUPPORT_C_SOURCES)) \
	$(VFS_RECOVERY_TEST_MAIN_OBJECT)
FILES_TEST_C_SOURCES := \
	$(VFS_TEST_SUPPORT_C_SOURCES) \
	tests/riscv/files_main.c
FILES_TEST_OBJECTS := \
	$(TEST_RUNTIME_OBJECTS) \
	$(patsubst %.c,$(BUILD_DIR)/%.o,$(FILES_TEST_C_SOURCES))
FILES_PARTIAL_WRITE_TEST_MAIN_OBJECT := \
	$(BUILD_DIR)/tests/riscv/files_partial_write_main.o
FILES_PARTIAL_WRITE_TEST_OBJECTS := \
	$(TEST_RUNTIME_OBJECTS) \
	$(patsubst %.c,$(BUILD_DIR)/%.o,$(VFS_TEST_SUPPORT_C_SOURCES)) \
	$(FILES_PARTIAL_WRITE_TEST_MAIN_OBJECT)
SV39_TEST_C_SOURCES := \
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
MM_TEST_C_SOURCES := \
	tests/riscv/mm_cases.c \
	tests/riscv/mm_cases_main.c
MM_TEST_OBJECTS := \
	$(TEST_RUNTIME_OBJECTS) \
	$(patsubst %.c,$(BUILD_DIR)/%.o,$(MM_TEST_C_SOURCES))
MM_FATAL_TEST_OBJECTS := \
	$(filter-out $(BUILD_DIR)/tests/riscv/mm_cases_main.o,$(MM_TEST_OBJECTS)) \
	$(BUILD_DIR)/tests/riscv/mm_fatal_main.o
VMA_TEST_C_SOURCES := \
	tests/riscv/vma_cases.c \
	tests/riscv/vma_cases_main.c
VMA_TEST_OBJECTS := \
	$(TEST_RUNTIME_OBJECTS) \
	$(patsubst %.c,$(BUILD_DIR)/%.o,$(VMA_TEST_C_SOURCES))
UACCESS_TEST_C_SOURCES := \
	tests/riscv/uaccess_cases.c \
	tests/riscv/uaccess_main.c
UACCESS_TEST_OBJECTS := \
	$(TEST_RUNTIME_OBJECTS) \
	$(patsubst %.c,$(BUILD_DIR)/%.o,$(UACCESS_TEST_C_SOURCES))
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
SIGNAL_TEST_KERNEL_RV := $(BUILD_DIR)/tests/kernel-signal-rv
SIGNAL_TEST_C_SOURCES := \
	tests/riscv/signal_cases.c \
	tests/riscv/signal_main.c
SIGNAL_TEST_OBJECTS := \
	$(TEST_RUNTIME_OBJECTS) \
	$(patsubst %.c,$(BUILD_DIR)/%.o,$(SIGNAL_TEST_C_SOURCES))
ELF64_TEST_C_SOURCES := \
	tests/riscv/elf64_cases.c \
	tests/riscv/elf64_main.c
ELF64_TEST_OBJECTS := \
	$(TEST_RUNTIME_OBJECTS) \
	$(patsubst %.c,$(BUILD_DIR)/%.o,$(ELF64_TEST_C_SOURCES))
ROOT_INIT_PROGRAM_OBJECT_RV := \
	$(BUILD_DIR)/tests/user/root_init.o
UACCESS_OOM_PROGRAM_OBJECT_RV := \
	$(BUILD_DIR)/tests/user/uaccess_oom.o
ROOT_EXEC_STAGE2_OBJECT_RV := \
	$(BUILD_DIR)/tests/user/root_exec_stage2.o
ROOT_EXEC_STAGE3_OBJECT_RV := \
	$(BUILD_DIR)/tests/user/root_exec_stage3.o
ROOT_EXEC_STAGE3_OOM_OBJECT_RV := \
	$(BUILD_DIR)/tests/user/root_exec_stage3_oom.o
DEMAND_PAGE_OOM_TEST_OBJECT_RV := \
	$(BUILD_DIR)/tests/riscv/demand_page_oom.o
ROOT_BOOT_CLEANUP_TEST_OBJECT_RV := \
	$(BUILD_DIR)/tests/riscv/root_boot_cleanup_boot.o
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
	$(HEAP_TEST_OBJECTS:.o=.d) \
	$(BLOCK_TEST_OBJECTS:.o=.d) \
	$(VFS_TEST_OBJECTS:.o=.d) \
	$(VFS_RECOVERY_TEST_MAIN_OBJECT:.o=.d) \
	$(FILES_TEST_OBJECTS:.o=.d) \
	$(FILES_PARTIAL_WRITE_TEST_MAIN_OBJECT:.o=.d) \
	$(SV39_TEST_OBJECTS:.o=.d) \
	$(SV39_FAULT_TEST_OBJECTS:.o=.d) \
	$(MM_TEST_OBJECTS:.o=.d) \
	$(MM_FATAL_TEST_OBJECTS:.o=.d) \
	$(VMA_TEST_OBJECTS:.o=.d) \
	$(UACCESS_TEST_OBJECTS:.o=.d) \
	$(TIMER_CASES_TEST_OBJECTS:.o=.d) \
	$(TIMER_BOOT_TEST_OBJECTS:.o=.d) \
	$(CONTEXT_TEST_OBJECTS:.o=.d) \
	$(SCHEDULER_CASES_TEST_OBJECTS:.o=.d) \
	$(SCHEDULER_BOOT_TEST_OBJECTS:.o=.d) \
	$(SYSCALL_TEST_OBJECTS:.o=.d) \
	$(SIGNAL_TEST_OBJECTS:.o=.d) \
	$(ELF64_TEST_OBJECTS:.o=.d) \
	$(ROOT_INIT_PROGRAM_OBJECT_RV:.o=.d) \
	$(UACCESS_OOM_PROGRAM_OBJECT_RV:.o=.d) \
	$(ROOT_EXEC_STAGE2_OBJECT_RV:.o=.d) \
	$(ROOT_EXEC_STAGE3_OBJECT_RV:.o=.d) \
	$(ROOT_EXEC_STAGE3_OOM_OBJECT_RV:.o=.d) \
	$(DEMAND_PAGE_OOM_TEST_OBJECT_RV:.o=.d) \
	$(ROOT_BOOT_CLEANUP_TEST_OBJECT_RV:.o=.d) \
	$(USER_TEST_OBJECTS:.o=.d) \
	$(USER_FATAL_TEST_OBJECTS:.o=.d)

.PHONY: all clean debug-riscv references run-riscv test-dtb-riscv \
	test-context-riscv \
	test-elf64-riscv \
	test-root-init-riscv test-demand-page-riscv test-exec-riscv \
	test-root-boot-cleanup-riscv \
	test-uaccess-oom-riscv test-icache-riscv \
	test-files-riscv \
	test-files-partial-write-riscv \
	test-high-half-trap-riscv test-idle-riscv test-no-identity-riscv \
	test-lwext4-host \
	test-block-riscv test-heap-riscv test-page-riscv test-vfs-riscv \
	test-scheduler-cases-riscv test-scheduler-riscv \
	test-boot-riscv test-references test-riscv \
	test-sv39-fault-riscv test-sv39-riscv \
	test-syscall-riscv test-signal-riscv test-timer-riscv test-trap-riscv \
	test-trap-return-riscv test-user-fatal-riscv test-mm-riscv \
	test-uaccess-riscv test-user-riscv test-vma-riscv test-brk-riscv \
	test-mmap-riscv

all: $(KERNEL_RV)

references:
	./references/fetch.sh

test-references:
	./tests/references.sh

test-lwext4-host:
	./tests/lwext4-host.sh

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

$(HEAP_TEST_KERNEL_RV): $(HEAP_TEST_OBJECTS) arch/riscv/linker.ld
	$(CC) $(LDFLAGS) -Wl,-Map,$(BUILD_DIR)/tests/kernel-heap-rv.map \
		-o $@ $(HEAP_TEST_OBJECTS)

$(BLOCK_TEST_KERNEL_RV): $(BLOCK_TEST_OBJECTS) arch/riscv/linker.ld
	$(CC) $(LDFLAGS) -Wl,-Map,$(BUILD_DIR)/tests/kernel-block-rv.map \
		-o $@ $(BLOCK_TEST_OBJECTS)

$(VFS_TEST_KERNEL_RV): $(VFS_TEST_OBJECTS) arch/riscv/linker.ld
	$(CC) $(LDFLAGS) -Wl,--wrap=ext4_orphan_free \
		-Wl,-Map,$(BUILD_DIR)/tests/kernel-vfs-rv.map \
		-o $@ $(VFS_TEST_OBJECTS)

$(BUILD_DIR)/tests/riscv/vfs_main.o: CPPFLAGS += $(LWEXT4_CPPFLAGS)

$(VFS_RECOVERY_TEST_KERNEL_RV): $(VFS_RECOVERY_TEST_OBJECTS) \
		arch/riscv/linker.ld
	$(CC) $(LDFLAGS) \
		-Wl,-Map,$(BUILD_DIR)/tests/kernel-vfs-recovery-rv.map \
		-o $@ $(VFS_RECOVERY_TEST_OBJECTS)

$(VFS_RECOVERY_TEST_MAIN_OBJECT): tests/riscv/vfs_main.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DVFS_EXPECT_RECOVERY \
		-MMD -MP -c $< -o $@

$(SV39_TEST_KERNEL_RV): $(SV39_TEST_OBJECTS) arch/riscv/linker.ld
	$(CC) $(LDFLAGS) -Wl,-Map,$(BUILD_DIR)/tests/kernel-sv39-rv.map \
		-o $@ $(SV39_TEST_OBJECTS)

$(SV39_FAULT_TEST_KERNEL_RV): $(SV39_FAULT_TEST_OBJECTS) \
		arch/riscv/linker.ld
	$(CC) $(LDFLAGS) \
		-Wl,-Map,$(BUILD_DIR)/tests/kernel-sv39-fault-rv.map \
		-o $@ $(SV39_FAULT_TEST_OBJECTS)

$(MM_TEST_KERNEL_RV): $(MM_TEST_OBJECTS) \
		arch/riscv/linker.ld
	$(CC) $(LDFLAGS) \
		-Wl,-Map,$(BUILD_DIR)/tests/kernel-mm-rv.map \
		-o $@ $(MM_TEST_OBJECTS)

$(MM_FATAL_TEST_KERNEL_RV): $(MM_FATAL_TEST_OBJECTS) \
		arch/riscv/linker.ld
	$(CC) $(LDFLAGS) \
		-Wl,-Map,$(BUILD_DIR)/tests/kernel-mm-fatal-rv.map \
		-o $@ $(MM_FATAL_TEST_OBJECTS)

$(VMA_TEST_KERNEL_RV): $(VMA_TEST_OBJECTS) \
		arch/riscv/linker.ld
	$(CC) $(LDFLAGS) \
		-Wl,--wrap=kernel_heap_resize \
		-Wl,--wrap=physical_page_allocate \
		-Wl,--wrap=riscv_sv39_current_satp \
		-Wl,-Map,$(BUILD_DIR)/tests/kernel-vma-rv.map \
		-o $@ $(VMA_TEST_OBJECTS)

$(UACCESS_TEST_KERNEL_RV): $(UACCESS_TEST_OBJECTS) \
		arch/riscv/linker.ld
	$(CC) $(LDFLAGS) \
		-Wl,-Map,$(BUILD_DIR)/tests/kernel-uaccess-rv.map \
		-o $@ $(UACCESS_TEST_OBJECTS)

$(FILES_TEST_KERNEL_RV): $(FILES_TEST_OBJECTS) arch/riscv/linker.ld
	$(CC) $(LDFLAGS) \
		-Wl,--wrap=kernel_open_file_release \
		-Wl,--wrap=riscv_sv39_current_satp \
		-Wl,-Map,$(BUILD_DIR)/tests/kernel-files-rv.map \
		-o $@ $(FILES_TEST_OBJECTS)

$(FILES_PARTIAL_WRITE_TEST_KERNEL_RV): \
		$(FILES_PARTIAL_WRITE_TEST_OBJECTS) arch/riscv/linker.ld
	$(CC) $(LDFLAGS) \
		-Wl,--wrap=kernel_open_file_release \
		-Wl,--wrap=riscv_sv39_current_satp \
		-Wl,--wrap=ext4_fwrite \
		-Wl,--wrap=ext4_ftruncate \
		-Wl,-Map,$(BUILD_DIR)/tests/kernel-files-partial-write-rv.map \
		-o $@ $(FILES_PARTIAL_WRITE_TEST_OBJECTS)

$(FILES_PARTIAL_WRITE_TEST_MAIN_OBJECT): tests/riscv/files_main.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(LWEXT4_CPPFLAGS) $(CFLAGS) \
		-DFILES_PARTIAL_WRITE_TEST -MMD -MP -c $< -o $@

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
	$(CC) $(LDFLAGS) -Wl,--wrap=kernel_task_mm_borrow_mutable \
	-Wl,--wrap=kernel_mm_brk \
	-Wl,--wrap=kernel_mm_mmap_anonymous \
	-Wl,--wrap=kernel_mm_validate_file_private_mapping \
	-Wl,--wrap=kernel_mm_mmap_file_private \
		-Wl,--wrap=kernel_mm_munmap \
		-Wl,--wrap=kernel_mm_mprotect \
	-Wl,--wrap=kernel_task_files_borrow \
	-Wl,--wrap=kernel_task_fs_context_borrow \
	-Wl,--wrap=kernel_task_set_tid_address \
	-Wl,--wrap=kernel_task_cpu_ticks \
	-Wl,--wrap=kernel_files_pin \
	-Wl,--wrap=kernel_files_write \
	-Wl,--wrap=kernel_files_writev \
	-Wl,--wrap=kernel_files_lseek \
	-Wl,--wrap=kernel_files_fstat \
	-Wl,--wrap=kernel_files_fstatat \
	-Wl,--wrap=kernel_files_getdents \
	-Wl,--wrap=kernel_files_dup \
	-Wl,--wrap=kernel_files_dup3 \
	-Wl,--wrap=kernel_files_fcntl \
	-Wl,--wrap=kernel_open_file_kind \
	-Wl,--wrap=kernel_open_file_readable \
	-Wl,--wrap=kernel_open_file_release \
	-Wl,-Map,$(BUILD_DIR)/tests/kernel-syscall-rv.map \
		-o $@ $(SYSCALL_TEST_OBJECTS)

$(SIGNAL_TEST_KERNEL_RV): $(SIGNAL_TEST_OBJECTS) arch/riscv/linker.ld
	$(CC) $(LDFLAGS) \
		-Wl,--wrap=kernel_copy_from_user \
		-Wl,--wrap=kernel_copy_to_user \
		-Wl,--wrap=kernel_signal_get_action \
		-Wl,--wrap=kernel_signal_set_action \
		-Wl,--wrap=kernel_signal_get_blocked \
		-Wl,--wrap=kernel_signal_update_blocked \
		-Wl,--wrap=kernel_signal_get_pending \
		-Wl,--wrap=kernel_signal_send_targets \
		-Wl,--wrap=kernel_signal_send_thread \
		-Wl,--wrap=kernel_task_mm_borrow_mutable \
		-Wl,--wrap=kernel_task_tid \
		-Wl,-Map,$(BUILD_DIR)/tests/kernel-signal-rv.map \
		-o $@ $(SIGNAL_TEST_OBJECTS)

$(ELF64_TEST_KERNEL_RV): $(ELF64_TEST_OBJECTS) arch/riscv/linker.ld
	$(CC) $(LDFLAGS) \
		-Wl,-Map,$(BUILD_DIR)/tests/kernel-elf64-rv.map \
		-o $@ $(ELF64_TEST_OBJECTS)

$(ROOT_INIT_PROGRAM_OBJECT_RV): tests/riscv/root_init.S
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(ASFLAGS) -MMD -MP -c $< -o $@

$(ROOT_INIT_PROGRAM_RV): $(ROOT_INIT_PROGRAM_OBJECT_RV) \
		tests/riscv/user_elf.ld
	$(CC) $(ARCH_FLAGS) -nostdlib -nostartfiles -static -no-pie \
		-T tests/riscv/user_elf.ld -Wl,--build-id=none \
		-Wl,--gc-sections -o $@ $(ROOT_INIT_PROGRAM_OBJECT_RV)

$(UACCESS_OOM_PROGRAM_OBJECT_RV): tests/riscv/uaccess_oom.S
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(ASFLAGS) -MMD -MP -c $< -o $@

$(UACCESS_OOM_PROGRAM_RV): $(UACCESS_OOM_PROGRAM_OBJECT_RV) \
		tests/riscv/user_elf.ld
	$(CC) $(ARCH_FLAGS) -nostdlib -nostartfiles -static -no-pie \
		-T tests/riscv/user_elf.ld -Wl,--build-id=none \
		-Wl,--gc-sections -o $@ $(UACCESS_OOM_PROGRAM_OBJECT_RV)

$(ROOT_EXEC_STAGE2_OBJECT_RV): tests/riscv/root_exec_stage.S
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(ASFLAGS) -DROOT_EXEC_STAGE=2 \
		-MMD -MP -c $< -o $@

$(ROOT_EXEC_STAGE3_OBJECT_RV): tests/riscv/root_exec_stage.S
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(ASFLAGS) -DROOT_EXEC_STAGE=3 \
		-MMD -MP -c $< -o $@

$(ROOT_EXEC_STAGE3_OOM_OBJECT_RV): tests/riscv/root_exec_stage.S
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(ASFLAGS) -DROOT_EXEC_STAGE=3 \
		-DROOT_FAULT_WAIT_STATUS=9 -DROOT_FAULT_INJECT_OOM=1 \
		-MMD -MP -c $< -o $@

$(ROOT_EXEC_STAGE2_RV): $(ROOT_EXEC_STAGE2_OBJECT_RV) \
		tests/riscv/user_elf.ld
	$(CC) $(ARCH_FLAGS) -nostdlib -nostartfiles -static -no-pie \
		-T tests/riscv/user_elf.ld -Wl,--build-id=none \
		-Wl,--gc-sections -o $@ $(ROOT_EXEC_STAGE2_OBJECT_RV)

$(ROOT_EXEC_STAGE3_RV): $(ROOT_EXEC_STAGE3_OBJECT_RV) \
		tests/riscv/user_elf.ld
	$(CC) $(ARCH_FLAGS) -nostdlib -nostartfiles -static -no-pie \
		-T tests/riscv/user_elf.ld -Wl,--build-id=none \
		-Wl,--gc-sections -o $@ $(ROOT_EXEC_STAGE3_OBJECT_RV)

$(ROOT_EXEC_STAGE3_OOM_RV): $(ROOT_EXEC_STAGE3_OOM_OBJECT_RV) \
		tests/riscv/user_elf.ld
	$(CC) $(ARCH_FLAGS) -nostdlib -nostartfiles -static -no-pie \
		-T tests/riscv/user_elf.ld -Wl,--build-id=none \
		-Wl,--gc-sections -o $@ $(ROOT_EXEC_STAGE3_OOM_OBJECT_RV)

$(DEMAND_PAGE_OOM_TEST_KERNEL_RV): $(OBJECTS) \
		$(DEMAND_PAGE_OOM_TEST_OBJECT_RV) arch/riscv/linker.ld
	$(CC) $(LDFLAGS) \
		-Wl,--wrap=kernel_scheduler_resolve_current_user_fault \
		-Wl,-Map,$(BUILD_DIR)/tests/kernel-demand-page-oom-rv.map \
		-o $@ $(OBJECTS) $(DEMAND_PAGE_OOM_TEST_OBJECT_RV)

$(ROOT_BOOT_CLEANUP_TEST_KERNEL_RV): $(OBJECTS) \
		$(ROOT_BOOT_CLEANUP_TEST_OBJECT_RV) arch/riscv/linker.ld
	$(CC) $(LDFLAGS) -Wl,--wrap=kernel_fs_context_create \
		-Wl,--wrap=kernel_block_write_at \
		-Wl,-Map,$(BUILD_DIR)/tests/kernel-root-boot-cleanup-rv.map \
		-o $@ $(OBJECTS) $(ROOT_BOOT_CLEANUP_TEST_OBJECT_RV)

$(USER_TEST_KERNEL_RV): $(USER_TEST_OBJECTS) arch/riscv/linker.ld
	$(CC) $(LDFLAGS) -Wl,--wrap=riscv_sv39_activate \
		-Wl,--wrap=kernel_scheduler_init \
		-Wl,--wrap=kernel_scheduler_reap_one \
		-Wl,--wrap=physical_page_release \
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

$(BUILD_DIR)/fs/lwext4_port.o $(BUILD_DIR)/fs/vfs.o: \
	CPPFLAGS += $(LWEXT4_CPPFLAGS)

$(BUILD_DIR)/third_party/lwext4/src/%.o: \
		third_party/lwext4/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(LWEXT4_CPPFLAGS) $(CFLAGS) \
		-Wno-unused-but-set-variable -Wno-stringop-truncation \
		-MMD -MP -c $< -o $@

$(BUILD_DIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(ASFLAGS) -MMD -MP -c $< -o $@

run-riscv: $(KERNEL_RV)
	$(QEMU_RISCV64) -machine virt -bios default -kernel $< \
		-m $(QEMU_MEMORY) -smp 1 -nographic -no-reboot

debug-riscv: $(KERNEL_RV)
	$(QEMU_RISCV64) -machine virt -bios default -kernel $< \
		-m $(QEMU_MEMORY) -smp 1 -nographic -no-reboot -S -s

.NOTPARALLEL: test-riscv
test-riscv: test-dtb-riscv test-page-riscv test-heap-riscv \
	test-block-riscv test-vfs-riscv test-files-riscv \
	test-files-partial-write-riscv \
	test-context-riscv test-scheduler-cases-riscv \
	test-scheduler-riscv test-syscall-riscv test-signal-riscv \
	test-elf64-riscv test-user-riscv test-user-fatal-riscv \
	test-mm-riscv test-vma-riscv test-uaccess-riscv \
	test-sv39-riscv test-sv39-fault-riscv \
	test-trap-return-riscv test-timer-riscv test-boot-riscv \
	test-high-half-trap-riscv test-no-identity-riscv \
	test-root-init-riscv test-uaccess-oom-riscv test-icache-riscv \
	test-demand-page-riscv test-root-boot-cleanup-riscv \
	test-exec-riscv test-idle-riscv

test-dtb-riscv: $(DTB_TEST_KERNEL_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) DTB_TEST_KERNEL_RV=$< \
		./tests/dtb-riscv.sh

test-page-riscv: $(PAGE_TEST_KERNEL_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) PAGE_TEST_KERNEL_RV=$< \
		./tests/page-riscv.sh

test-heap-riscv: $(HEAP_TEST_KERNEL_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) HEAP_TEST_KERNEL_RV=$< \
		./tests/heap-riscv.sh

test-block-riscv: $(BLOCK_TEST_KERNEL_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) BLOCK_TEST_KERNEL_RV=$< \
		./tests/block-riscv.sh

test-vfs-riscv: $(VFS_TEST_KERNEL_RV) $(VFS_RECOVERY_TEST_KERNEL_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) \
		VFS_TEST_KERNEL_RV=$(VFS_TEST_KERNEL_RV) \
		VFS_RECOVERY_TEST_KERNEL_RV=$(VFS_RECOVERY_TEST_KERNEL_RV) \
		./tests/vfs-riscv.sh

test-files-riscv: $(FILES_TEST_KERNEL_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) FILES_TEST_KERNEL_RV=$< \
		./tests/files-riscv.sh

test-files-partial-write-riscv: $(FILES_PARTIAL_WRITE_TEST_KERNEL_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) \
		FILES_TEST_KERNEL_RV=$< FILES_TEST_READONLY=off \
		FILES_TEST_EXPECT_CONSOLE_WRITES=0 \
		FILES_TEST_SUCCESS_MARKER='BoarOS: process files partial write tests passed' \
		./tests/files-riscv.sh

test-context-riscv: $(CONTEXT_TEST_KERNEL_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) OBJDUMP_RV=$(OBJDUMP) \
		CONTEXT_TEST_KERNEL_RV=$< \
		CONTEXT_OBJECT_RV=$(BUILD_DIR)/arch/riscv/context_switch.o \
		./tests/context-riscv.sh

test-scheduler-cases-riscv: $(SCHEDULER_CASES_TEST_KERNEL_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) SCHEDULER_CASES_TEST_KERNEL_RV=$< \
		./tests/scheduler-cases-riscv.sh

test-scheduler-riscv: $(SCHEDULER_BOOT_TEST_KERNEL_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) OBJDUMP_RV=$(OBJDUMP) \
		SCHEDULER_BOOT_TEST_KERNEL_RV=$< \
		SCHEDULER_OBJECT_RV=$(BUILD_DIR)/kernel/sched/core.o \
		./tests/scheduler-riscv.sh

test-syscall-riscv: $(SYSCALL_TEST_KERNEL_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) SYSCALL_TEST_KERNEL_RV=$< \
		./tests/syscall-riscv.sh

test-signal-riscv: $(SIGNAL_TEST_KERNEL_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) SIGNAL_TEST_KERNEL_RV=$< \
		./tests/signal-riscv.sh

MUSL_TARBALL := references/musl/musl-1.2.5.tar.gz
MUSL_ROOT := $(BUILD_DIR)/musl-root
MUSL_STAMP := $(MUSL_ROOT)/.shared-built
MUSL_LDSO := $(MUSL_ROOT)/lib/ld-musl-riscv64.so.1
REAL_USERLAND_RV := $(BUILD_DIR)/tests/user/real-userland-rv
PTHREAD_USERLAND_RV := $(BUILD_DIR)/tests/user/pthread-userland-rv
PTHREAD_TLS_DSO_RV := $(BUILD_DIR)/tests/user/libboaros-tls.so

$(MUSL_STAMP): $(MUSL_TARBALL)
	@mkdir -p $(BUILD_DIR)/musl-src
	rm -rf $(BUILD_DIR)/musl-src/musl-1.2.5 $(MUSL_ROOT)
	tar -C $(BUILD_DIR)/musl-src -xzf $<
	cd $(BUILD_DIR)/musl-src/musl-1.2.5 && \
		CC=riscv64-linux-gnu-gcc AR=riscv64-linux-gnu-ar \
		RANLIB=riscv64-linux-gnu-ranlib ./configure \
			--target=riscv64-linux-musl \
			--prefix=$(abspath $(MUSL_ROOT)) \
			--syslibdir=$(abspath $(MUSL_ROOT))/lib && \
		make && make install
	touch $@

$(MUSL_LDSO): $(MUSL_STAMP)
	@test -f $@

MUSL_GCC_FLAGS ?= $(shell $(MUSL_ROOT)/bin/musl-gcc -fno-link-libatomic -E -x c /dev/null >/dev/null 2>&1 && echo -fno-link-libatomic)

$(REAL_USERLAND_RV): tests/userland/real.c $(MUSL_STAMP)
	@mkdir -p $(dir $@)
	$(MUSL_ROOT)/bin/musl-gcc $(MUSL_GCC_FLAGS) -static -O2 \
		-o $@ $<

$(PTHREAD_USERLAND_RV): tests/userland/pthread.c $(MUSL_STAMP)
	@mkdir -p $(dir $@)
	$(MUSL_ROOT)/bin/musl-gcc $(MUSL_GCC_FLAGS) -fPIE -pie -O2 -pthread \
		-Wl,--dynamic-linker=/lib/ld-musl-riscv64.so.1 \
		-o $@ $< -ldl -lc

$(PTHREAD_TLS_DSO_RV): tests/userland/tls_dso.c $(MUSL_STAMP)
	@mkdir -p $(dir $@)
	$(MUSL_ROOT)/bin/musl-gcc $(MUSL_GCC_FLAGS) -fPIC -shared -O2 \
		-Wl,-soname,libboaros-tls.so \
		-o $@ $< -lc

.PHONY: test-userland-riscv
test-userland-riscv: $(REAL_USERLAND_RV) $(PTHREAD_USERLAND_RV) \
		$(PTHREAD_TLS_DSO_RV) $(MUSL_LDSO) kernel-rv
	QEMU_RISCV64=$(QEMU_RISCV64) REAL_USERLAND_RV=$(REAL_USERLAND_RV) \
		PTHREAD_USERLAND_RV=$(PTHREAD_USERLAND_RV) \
		PTHREAD_TLS_DSO_RV=$(PTHREAD_TLS_DSO_RV) MUSL_LDSO=$(MUSL_LDSO) \
		./tests/userland-riscv.sh

test-brk-riscv: test-sv39-riscv test-vma-riscv \
		test-elf64-riscv test-syscall-riscv \
		test-root-init-riscv

test-mmap-riscv: test-sv39-riscv test-vma-riscv \
		test-syscall-riscv test-files-riscv test-root-init-riscv

test-elf64-riscv: $(ELF64_TEST_KERNEL_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) ELF64_TEST_KERNEL_RV=$< \
		./tests/elf64-riscv.sh

test-root-init-riscv: $(KERNEL_RV) $(ROOT_INIT_PROGRAM_RV) \
		$(ROOT_EXEC_STAGE2_RV) $(ROOT_EXEC_STAGE3_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) QEMU_MEMORY=$(QEMU_MEMORY) \
		KERNEL_RV=$(KERNEL_RV) \
		ROOT_INIT_PROGRAM_RV=$(ROOT_INIT_PROGRAM_RV) \
		ROOT_EXEC_STAGE2_RV=$(ROOT_EXEC_STAGE2_RV) \
		ROOT_EXEC_STAGE3_RV=$(ROOT_EXEC_STAGE3_RV) \
		./tests/root-init-riscv.sh
	QEMU_RISCV64=$(QEMU_RISCV64) QEMU_MEMORY=$(QEMU_MEMORY) \
		VIRTIO_MMIO_FORCE_LEGACY=false \
		KERNEL_RV=$(KERNEL_RV) \
		ROOT_INIT_PROGRAM_RV=$(ROOT_INIT_PROGRAM_RV) \
		ROOT_EXEC_STAGE2_RV=$(ROOT_EXEC_STAGE2_RV) \
		ROOT_EXEC_STAGE3_RV=$(ROOT_EXEC_STAGE3_RV) \
		./tests/root-init-riscv.sh

test-uaccess-oom-riscv: $(KERNEL_RV) $(UACCESS_OOM_PROGRAM_RV) \
		$(ROOT_EXEC_STAGE2_RV) $(ROOT_EXEC_STAGE3_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) QEMU_MEMORY=64M \
		VIRTIO_MMIO_FORCE_LEGACY=false \
		KERNEL_RV=$(KERNEL_RV) \
		ROOT_INIT_PROGRAM_RV=$(UACCESS_OOM_PROGRAM_RV) \
		ROOT_EXEC_STAGE2_RV=$(ROOT_EXEC_STAGE2_RV) \
		ROOT_EXEC_STAGE3_RV=$(ROOT_EXEC_STAGE3_RV) \
		./tests/uaccess-oom-riscv.sh

test-icache-riscv: $(KERNEL_RV)
	OBJDUMP_RV=$(OBJDUMP) ./tests/icache-riscv.sh

test-demand-page-riscv: test-root-init-riscv \
		$(DEMAND_PAGE_OOM_TEST_KERNEL_RV) $(ROOT_INIT_PROGRAM_RV) \
		$(ROOT_EXEC_STAGE2_RV) $(ROOT_EXEC_STAGE3_RV) \
		$(ROOT_EXEC_STAGE3_OOM_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) QEMU_MEMORY=$(QEMU_MEMORY) \
		KERNEL_RV=$(DEMAND_PAGE_OOM_TEST_KERNEL_RV) \
		ROOT_INIT_PROGRAM_RV=$(ROOT_INIT_PROGRAM_RV) \
		ROOT_EXEC_STAGE2_RV=$(ROOT_EXEC_STAGE2_RV) \
		ROOT_EXEC_STAGE3_RV=$(ROOT_EXEC_STAGE3_OOM_RV) \
		./tests/root-init-riscv.sh

test-exec-riscv: test-root-init-riscv

test-root-boot-cleanup-riscv: $(ROOT_BOOT_CLEANUP_TEST_KERNEL_RV) \
		$(ROOT_INIT_PROGRAM_RV) $(ROOT_EXEC_STAGE2_RV) \
		$(ROOT_EXEC_STAGE3_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) QEMU_MEMORY=$(QEMU_MEMORY) \
		KERNEL_RV=$(ROOT_BOOT_CLEANUP_TEST_KERNEL_RV) \
		ROOT_INIT_PROGRAM_RV=$(ROOT_INIT_PROGRAM_RV) \
		ROOT_EXEC_STAGE2_RV=$(ROOT_EXEC_STAGE2_RV) \
		ROOT_EXEC_STAGE3_RV=$(ROOT_EXEC_STAGE3_RV) \
		ROOT_BOOT_ERROR_STATUS=0x9 \
		./tests/root-init-riscv.sh

test-user-riscv: $(USER_TEST_KERNEL_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) USER_TEST_KERNEL_RV=$< \
		./tests/user-riscv.sh

test-user-fatal-riscv: $(USER_FATAL_TEST_KERNEL_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) USER_FATAL_TEST_KERNEL_RV=$< \
		./tests/user-fatal-riscv.sh

test-mm-riscv: $(MM_TEST_KERNEL_RV) $(MM_FATAL_TEST_KERNEL_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) MM_TEST_KERNEL_RV=$< \
		./tests/mm-riscv.sh
	QEMU_RISCV64=$(QEMU_RISCV64) \
		MM_FATAL_TEST_KERNEL_RV=$(MM_FATAL_TEST_KERNEL_RV) \
		./tests/mm-fatal-riscv.sh

test-vma-riscv: $(VMA_TEST_KERNEL_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) VMA_TEST_KERNEL_RV=$< \
		./tests/vma-riscv.sh

test-uaccess-riscv: $(UACCESS_TEST_KERNEL_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) UACCESS_TEST_KERNEL_RV=$< \
		./tests/uaccess-riscv.sh

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

test-boot-riscv: $(TIMER_BOOT_TEST_KERNEL_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) NM_RV=$(NM) READELF_RV=$(READELF) \
		BOOT_TEST_KERNEL_RV=$< ./tests/boot-riscv.sh

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

.PHONY: test-allocator-release-host
test-allocator-release-host:
	mkdir -p build/host
	cc -std=c11 -Wall -Wextra -Werror -DBOAROS_PAGE_SHIFT=12 -Iinclude tests/host/allocator_release.c kernel/physical_page.c mm/heap.c -o build/host/allocator-release
	build/host/allocator-release
