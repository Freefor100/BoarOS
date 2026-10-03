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

COST_DIAGNOSTICS ?= 0
ifneq ($(filter $(COST_DIAGNOSTICS),0 1),$(COST_DIAGNOSTICS))
$(error COST_DIAGNOSTICS must be 0 or 1)
endif
ifeq ($(COST_DIAGNOSTICS),1)
BUILD_DIR := build/cost/riscv
KERNEL_RV := build/cost/kernel-rv
else
BUILD_DIR := build/riscv
KERNEL_RV := kernel-rv
endif
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
ELF_RWX_PROGRAM_RV := $(BUILD_DIR)/tests/user/elf-rwx-rv
ROOT_ORPHAN_PROGRAM_RV := $(BUILD_DIR)/tests/user/root-orphan-rv
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
CPPFLAGS := -DBOAROS_COST_DIAGNOSTICS=$(COST_DIAGNOSTICS) -Iinclude -DBOAROS_PAGE_SHIFT=12 \
	-DBOAROS_UTS_MACHINE=\"riscv64\"
CFLAGS := $(ARCH_FLAGS) -std=gnu11 -O2 -g3 \
	-ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie \
	-ffunction-sections -fdata-sections -Wall -Wextra -Werror
CFLAGS += -fstack-usage $(CFLAGS_EXTRA)
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
	third_party/lwext4/src/ext4_journal.c \
	third_party/lwext4/src/ext4_orphan.c \
	third_party/lwext4/src/ext4_super.c \
	third_party/lwext4/src/ext4_trans.c \
	third_party/lwext4/src/ext4_truncate.c

LWIP_CPPFLAGS := -Inet/lwip_port/include -Ithird_party/lwip/src/include
LWIP_SOURCES := \
	third_party/lwip/src/core/def.c \
	third_party/lwip/src/core/inet_chksum.c \
	third_party/lwip/src/core/init.c \
	third_party/lwip/src/core/ip.c \
	third_party/lwip/src/core/mem.c \
	third_party/lwip/src/core/memp.c \
	third_party/lwip/src/core/netif.c \
	third_party/lwip/src/core/pbuf.c \
	third_party/lwip/src/core/stats.c \
	third_party/lwip/src/core/sys.c \
	third_party/lwip/src/core/tcp.c \
	third_party/lwip/src/core/tcp_in.c \
	third_party/lwip/src/core/tcp_out.c \
	third_party/lwip/src/core/timeouts.c \
	third_party/lwip/src/core/udp.c \
	third_party/lwip/src/netif/ethernet.c \
	third_party/lwip/src/core/ipv4/etharp.c \
	third_party/lwip/src/core/ipv4/icmp.c \
	third_party/lwip/src/core/ipv4/ip4.c \
	third_party/lwip/src/core/ipv4/ip4_addr.c \
	third_party/lwip/src/core/ipv4/ip4_frag.c \
	third_party/lwip/src/core/ipv6/icmp6.c \
	third_party/lwip/src/core/ipv6/ip6.c \
	third_party/lwip/src/core/ipv6/ip6_addr.c \
	third_party/lwip/src/core/ipv6/nd6.c \
	net/lwip_port/port.c

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
	arch/riscv/uart_tty.c \
	arch/riscv/virtio_mmio_block.c \
	arch/riscv/virtio_mmio_rng.c \
	arch/riscv/virtio_mmio_net.c \
	arch/riscv/plic.c \
	fs/lwext4_port.c \
	fs/files/table.c \
	fs/files/locks.c \
	fs/files/io.c \
	fs/files/path.c \
	fs/files/metadata.c \
	fs/files/console.c \
	fs/files/poll.c \
	fs/files/epoll.c \
	fs/files/socket.c \
	fs/fs_context.c \
	fs/char_device.c \
	fs/tty.c \
	fs/rtc_device.c \
	fs/open_file.c \
	fs/pipe.c \
	fs/record_lock.c \
	fs/page_cache.c \
	fs/vfs.c \
	fs/procfs.c \
	fs/tmpfs.c \
	fs/disk_mount.c \
	fs/ext4_backend.c \
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
	kernel/log.c \
	kernel/blake2s.c \
	kernel/sched/core.c \
	kernel/sched/policy.c \
	kernel/sched/runqueue.c \
	kernel/sched/scheduling.c \
	kernel/sched/exec.c \
	kernel/sched/process.c \
	kernel/sched/proc.c \
	kernel/sched/signal.c \
	kernel/sched/tty.c \
	kernel/sched/wait.c \
	kernel/sched/sync.c \
	kernel/sched/futex.c \
	kernel/syscall/dispatch.c \
	kernel/syscall/file.c \
	kernel/syscall/mount.c \
	kernel/syscall/memory.c \
	kernel/syscall/process.c \
	kernel/syscall/signal.c \
	kernel/syscall/socket.c \
	kernel/syscall/shm.c \
	kernel/syscall/time.c \
	kernel/syscall/random.c \
	kernel/syscall/log.c \
	kernel/syscall/sched.c \
	arch/riscv/signal.c \
	kernel/tick.c \
	kernel/time.c \
	kernel/cost.c \
	kernel/sched/cost.c \
	net/socket.c \
	net/ethernet.c \
	lib/qsort.c \
	lib/string.c \
	mm/vma.c \
	mm/memory_object.c \
	mm/shm.c \
	mm/heap.c \
	$(LWEXT4_SOURCES) \
	$(LWIP_SOURCES)
ASM_SOURCES := \
	arch/riscv/boot.S \
	arch/riscv/context_switch.S \
	arch/riscv/fpu.S \
	arch/riscv/trap_entry.S
ifeq ($(ROOT_DRAIN_FIXTURE),1)
C_SOURCES += tests/riscv/root_drain_fixture.c
LDFLAGS += -Wl,--wrap=kernel_vfs_unmount
endif

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
	arch/riscv/uart_tty.c \
	arch/riscv/virtio_mmio_block.c \
	arch/riscv/virtio_mmio_rng.c \
	arch/riscv/virtio_mmio_net.c \
	arch/riscv/plic.c \
	fs/files/table.c \
	fs/files/locks.c \
	fs/files/io.c \
	fs/files/path.c \
	fs/files/metadata.c \
	fs/files/console.c \
	fs/files/poll.c \
	fs/files/epoll.c \
	fs/files/socket.c \
	fs/fs_context.c \
	fs/char_device.c \
	fs/tty.c \
	fs/rtc_device.c \
	fs/lwext4_port.c \
	fs/open_file.c \
	fs/pipe.c \
	fs/record_lock.c \
	fs/page_cache.c \
	fs/vfs.c \
	fs/procfs.c \
	fs/tmpfs.c \
	fs/disk_mount.c \
	fs/ext4_backend.c \
	kernel/block.c \
	kernel/elf64.c \
	kernel/elf64_source.c \
	kernel/exec.c \
	kernel/pid.c \
	kernel/physical_page.c \
	kernel/read_source.c \
	kernel/random.c \
	kernel/log.c \
	kernel/blake2s.c \
	kernel/sched/core.c \
	kernel/sched/policy.c \
	kernel/sched/runqueue.c \
	kernel/sched/scheduling.c \
	kernel/sched/exec.c \
	kernel/sched/process.c \
	kernel/sched/proc.c \
	kernel/sched/signal.c \
	kernel/sched/tty.c \
	kernel/sched/wait.c \
	kernel/sched/sync.c \
	kernel/sched/futex.c \
	kernel/syscall/dispatch.c \
	kernel/syscall/file.c \
	kernel/syscall/mount.c \
	kernel/syscall/memory.c \
	kernel/syscall/process.c \
	kernel/syscall/signal.c \
	kernel/syscall/socket.c \
	kernel/syscall/shm.c \
	kernel/syscall/time.c \
	kernel/syscall/random.c \
	kernel/syscall/log.c \
	kernel/syscall/sched.c \
	arch/riscv/signal.c \
	kernel/tick.c \
	kernel/time.c \
	kernel/cost.c \
	kernel/sched/cost.c \
	net/socket.c \
	lib/qsort.c \
	lib/string.c \
	mm/vma.c \
	mm/memory_object.c \
	mm/shm.c \
	mm/heap.c \
	$(LWEXT4_SOURCES) \
	$(LWIP_SOURCES)
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
ROOT_ORPHAN_PROGRAM_OBJECT_RV := \
	$(BUILD_DIR)/tests/user/root_orphan_init.o
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
	test-lwext4-host test-lwext4-recovery-host test-lwext4-rename-host \
	test-block-riscv test-heap-riscv test-page-riscv test-vfs-riscv \
	test-scheduler-cases-riscv test-scheduler-riscv \
	test-boot-riscv test-references test-riscv \
	test-sv39-fault-riscv test-sv39-riscv \
	test-syscall-riscv test-signal-riscv test-timer-riscv test-trap-riscv \
	test-trap-return-riscv test-user-fatal-riscv test-mm-riscv \
	test-uaccess-riscv test-user-riscv test-vma-riscv test-brk-riscv \
	test-mmap-riscv

all: $(KERNEL_RV)

INIT_CONFIG ?= config/init.json
.PHONY: force-init-config test-init-config-riscv
force-init-config:

$(BUILD_DIR)/generated/init-config.h: force-init-config $(INIT_CONFIG) tools/init-config.py include/kernel/exec_image.h include/kernel/fs_context.h
	python3 tools/init-config.py $(INIT_CONFIG) $@

$(BUILD_DIR)/arch/riscv/root_boot.o: $(BUILD_DIR)/generated/init-config.h
$(BUILD_DIR)/arch/riscv/root_boot.o: CPPFLAGS += -I$(BUILD_DIR)/generated

test-init-config-riscv:
	python3 tests/init-config-riscv.py

references:
	./references/fetch.sh

test-references:
	./tests/references.sh

test-lwext4-host:
	./tests/lwext4-host.sh

.PHONY: test-lwext4-instances-host
test-lwext4-instances-host:
	sh tests/lwext4-instances-host.sh

test-lwext4-rename-host:
	sh tests/lwext4-rename-host.sh

.PHONY: test-lwext4-metadata-host
test-lwext4-metadata-host:
	sh tests/lwext4-metadata-host.sh

.PHONY: test-lwext4-cost-host
test-lwext4-cost-host:
	sh tests/lwext4-cost-host.sh

.PHONY: test-lwext4-cache-host
test-lwext4-cache-host:
	sh tests/lwext4-cache-host.sh

.PHONY: test-lwext4-group-host
test-lwext4-group-host:
	sh tests/lwext4-group-host.sh

JOURNAL_GROUP_KERNEL := $(BUILD_DIR)/tests/kernel-journal-group-rv
$(BUILD_DIR)/tests/riscv/journal_group_main.o: CPPFLAGS += -Ifs/lwext4_config -Ithird_party/lwext4/include -DCONFIG_USE_DEFAULT_CFG=0
-include $(BUILD_DIR)/tests/riscv/journal_group_main.d
$(JOURNAL_GROUP_KERNEL): $(TEST_RUNTIME_OBJECTS) $(BUILD_DIR)/kernel/dtb.o $(BUILD_DIR)/tests/riscv/journal_group_main.o arch/riscv/linker.ld
	$(CC) $(LDFLAGS) -o $@ $(TEST_RUNTIME_OBJECTS) $(BUILD_DIR)/kernel/dtb.o $(BUILD_DIR)/tests/riscv/journal_group_main.o
.PHONY: test-journal-group-riscv
test-journal-group-riscv: $(JOURNAL_GROUP_KERNEL)
	python3 -B tests/journal-group-riscv.py --kernel $< --qemu $(QEMU_RISCV64)
$(BUILD_DIR)/tests/kernel-journal-idle-negative-rv: $(TEST_RUNTIME_OBJECTS) $(BUILD_DIR)/kernel/dtb.o $(BUILD_DIR)/tests/riscv/journal_group_main.o arch/riscv/linker.ld
	$(CC) $(LDFLAGS) -Wl,--wrap=kernel_scheduler_prepare_idle_return -o $@ $(TEST_RUNTIME_OBJECTS) $(BUILD_DIR)/kernel/dtb.o $(BUILD_DIR)/tests/riscv/journal_group_main.o
.PHONY: test-journal-idle-negative-riscv
test-journal-idle-negative-riscv: $(BUILD_DIR)/tests/kernel-journal-idle-negative-rv
	python3 -B tests/journal-group-riscv.py --kernel $< --qemu $(QEMU_RISCV64) --expect-idle-failure

# Real volatile-storage power cuts, separate from normal QEMU shutdown tests.
test-lwext4-recovery-host:
	sh tests/lwext4-journal-host.sh
	sh tests/lwext4-ordered-host.sh
	sh tests/lwext4-orphan-host.sh
	sh tests/lwext4-truncate-host.sh
	sh tests/lwext4-recovery-host.sh
	sh tests/lwext4-reclaim-host.sh

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
	$(CC) $(LDFLAGS) -Wl,--wrap=ext4_orphan_free -Wl,--wrap=ext4_fclose -Wl,--wrap=kernel_heap_allocate \
		-Wl,--wrap=ext4_journal_start -Wl,--wrap=ext4_user_calloc \
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
		-Wl,--wrap=kernel_copy_to_user -Wl,-Map,$(BUILD_DIR)/tests/kernel-mm-rv.map \
		-o $@ $(MM_TEST_OBJECTS)

$(MM_FATAL_TEST_KERNEL_RV): $(MM_FATAL_TEST_OBJECTS) \
		arch/riscv/linker.ld
	$(CC) $(LDFLAGS) \
		-Wl,--wrap=kernel_copy_to_user -Wl,-Map,$(BUILD_DIR)/tests/kernel-mm-fatal-rv.map \
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
		-Wl,--wrap=kernel_open_file_get_page \
		-Wl,--wrap=kernel_open_file_release \
		-Wl,--wrap=kernel_heap_allocate \
		-Wl,--wrap=kernel_heap_allocate_zeroed \
		-Wl,--wrap=physical_page_allocate -Wl,--wrap=physical_page_allocate_order \
		-Wl,--wrap=virt_uart_rx_ready \
		-Wl,--wrap=virt_uart_getc \
		-Wl,--wrap=riscv_sv39_current_satp \
		-Wl,-Map,$(BUILD_DIR)/tests/kernel-files-rv.map \
		-o $@ $(FILES_TEST_OBJECTS)

$(FILES_PARTIAL_WRITE_TEST_KERNEL_RV): \
		$(FILES_PARTIAL_WRITE_TEST_OBJECTS) arch/riscv/linker.ld
	$(CC) $(LDFLAGS) \
		-Wl,--wrap=kernel_open_file_get_page \
		-Wl,--wrap=kernel_open_file_release \
		-Wl,--wrap=kernel_heap_allocate_zeroed \
		-Wl,--wrap=physical_page_allocate -Wl,--wrap=physical_page_allocate_order \
		-Wl,--wrap=riscv_sv39_current_satp \
		-Wl,--wrap=ext4_fpwrite \
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
	-Wl,--wrap=kernel_files_mknodat \
	-Wl,--wrap=kernel_files_dup \
	-Wl,--wrap=kernel_files_dup3 \
	-Wl,--wrap=kernel_files_fcntl \
	-Wl,--wrap=kernel_open_file_kind \
	-Wl,--wrap=kernel_open_file_readable \
	-Wl,--wrap=kernel_open_file_writable \
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
		-Wl,--wrap=kernel_task_tgid \
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

$(ROOT_ORPHAN_PROGRAM_OBJECT_RV): tests/riscv/root_orphan_init.S
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(ASFLAGS) -MMD -MP -c $< -o $@

$(ROOT_ORPHAN_PROGRAM_RV): $(ROOT_ORPHAN_PROGRAM_OBJECT_RV) \
		tests/riscv/user_elf.ld
	$(CC) $(ARCH_FLAGS) -nostdlib -nostartfiles -static -no-pie \
		-T tests/riscv/user_elf.ld -Wl,--build-id=none \
		-Wl,--gc-sections -o $@ $(ROOT_ORPHAN_PROGRAM_OBJECT_RV)

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
		-Wl,--wrap=riscv_root_boot_cleanup \
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

$(BUILD_DIR)/%.o: %.c $(BUILD_DIR)/generated/cost-config.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/fs/lwext4_port.o $(BUILD_DIR)/fs/ext4_backend.o: \
	CPPFLAGS += $(LWEXT4_CPPFLAGS)

$(BUILD_DIR)/third_party/lwip/src/core/%.o \
$(BUILD_DIR)/net/lwip_port/%.o \
$(BUILD_DIR)/net/socket.o \
$(BUILD_DIR)/net/ethernet.o \
$(BUILD_DIR)/third_party/lwip/src/netif/ethernet.o: CPPFLAGS += $(LWIP_CPPFLAGS)

$(BUILD_DIR)/third_party/lwext4/src/%.o: \
		third_party/lwext4/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(LWEXT4_CPPFLAGS) $(CFLAGS) \
		-Wno-unused-but-set-variable -Wno-stringop-truncation \
		-MMD -MP -c $< -o $@

$(BUILD_DIR)/%.o: %.S $(BUILD_DIR)/generated/cost-config.h
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
	test-elf64-riscv test-elf-rwx-riscv \
	test-user-riscv test-user-fatal-riscv \
	test-mm-riscv test-vma-riscv test-uaccess-riscv \
	test-sv39-riscv test-sv39-fault-riscv \
	test-trap-return-riscv test-timer-riscv test-boot-riscv \
	test-high-half-trap-riscv test-no-identity-riscv \
	test-root-init-riscv test-root-orphan-riscv \
	test-uaccess-oom-riscv test-icache-riscv \
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
SQLITE_ARCHIVE := references/sqlite/sqlite-amalgamation-3530400.zip
SQLITE_SOURCE := $(BUILD_DIR)/sqlite/sqlite-amalgamation-3530400/sqlite3.c
SQLITE_ROLLBACK_RV := $(BUILD_DIR)/tests/user/sqlite-rollback-rv
SQLITE_CLI_STATIC_RV := $(BUILD_DIR)/tests/user/sqlite3-static-rv
SQLITE_CLI_DYNAMIC_RV := $(BUILD_DIR)/tests/user/sqlite3-dynamic-rv
SQLITE_CLI_INIT_RV := $(BUILD_DIR)/tests/user/sqlite-cli-init-rv
SQLITE_RECOVERY_RV := $(BUILD_DIR)/tests/user/sqlite-recovery-rv
SQLITE_WAL_RV := $(BUILD_DIR)/tests/user/sqlite-wal-rv
LOCK_LIFECYCLE_RV := $(BUILD_DIR)/tests/user/record-lock-lifecycle-rv
OFFLINE_C_RV := $(BUILD_DIR)/tests/user/offline-c-rv

$(SQLITE_SOURCE): $(SQLITE_ARCHIVE)
	@mkdir -p $(BUILD_DIR)/sqlite
	unzip -oq $< -d $(BUILD_DIR)/sqlite
	@test -f $@ && test -f $(dir $@)/shell.c
	@touch $@ $(dir $@)/shell.c

$(SQLITE_ROLLBACK_RV): tests/workloads/sqlite/rollback.c $(SQLITE_SOURCE) $(MUSL_STAMP)
	@mkdir -p $(dir $@)
	$(MUSL_ROOT)/bin/musl-gcc $(MUSL_GCC_FLAGS) -static -O2 -pthread \
		-I$(dir $(SQLITE_SOURCE)) -o $@ $< $(SQLITE_SOURCE) -ldl

$(SQLITE_CLI_STATIC_RV): $(SQLITE_SOURCE) $(MUSL_STAMP)
	@mkdir -p $(dir $@)
	$(MUSL_ROOT)/bin/musl-gcc $(MUSL_GCC_FLAGS) -static -O2 -pthread \
		-o $@ $(dir $(SQLITE_SOURCE))/shell.c $(SQLITE_SOURCE) -ldl

$(SQLITE_CLI_DYNAMIC_RV): $(SQLITE_SOURCE) $(MUSL_STAMP)
	@mkdir -p $(dir $@)
	$(MUSL_ROOT)/bin/musl-gcc $(MUSL_GCC_FLAGS) -fPIE -pie -O2 -pthread \
		-Wl,--dynamic-linker=/lib/ld-musl-riscv64.so.1 \
		-o $@ $(dir $(SQLITE_SOURCE))/shell.c $(SQLITE_SOURCE) -ldl

$(SQLITE_CLI_INIT_RV): tests/workloads/sqlite/cli_init.c $(MUSL_STAMP)
	@mkdir -p $(dir $@)
	$(MUSL_ROOT)/bin/musl-gcc $(MUSL_GCC_FLAGS) -static -O2 -o $@ $<

$(SQLITE_RECOVERY_RV): tests/workloads/sqlite/recovery.c $(SQLITE_SOURCE) $(MUSL_STAMP)
	@mkdir -p $(dir $@)
	$(MUSL_ROOT)/bin/musl-gcc $(MUSL_GCC_FLAGS) -static -O2 -pthread \
		-I$(dir $(SQLITE_SOURCE)) -o $@ $< $(SQLITE_SOURCE) -ldl

$(SQLITE_WAL_RV): tests/workloads/sqlite/wal.c $(SQLITE_SOURCE) $(MUSL_STAMP)
	@mkdir -p $(dir $@)
	$(MUSL_ROOT)/bin/musl-gcc $(MUSL_GCC_FLAGS) -static -O2 -pthread \
		-I$(dir $(SQLITE_SOURCE)) -o $@ $< $(SQLITE_SOURCE) -ldl

.PHONY: test-sqlite-wal-riscv
test-sqlite-wal-riscv: $(SQLITE_WAL_RV) $(KERNEL_RV)
	PYTHONDONTWRITEBYTECODE=1 python3 tests/sqlite-wal-riscv.py \
		--kernel $(KERNEL_RV) --program $(SQLITE_WAL_RV) \
		--qemu $(QEMU_RISCV64)

$(LOCK_LIFECYCLE_RV): tests/workloads/locks/lifecycle.c $(MUSL_STAMP)
	@mkdir -p $(dir $@)
	$(MUSL_ROOT)/bin/musl-gcc $(MUSL_GCC_FLAGS) -static -O2 -pthread -o $@ $<

$(OFFLINE_C_RV): tests/workloads/toolchain/offline_c.c $(MUSL_STAMP)
	@mkdir -p $(dir $@)
	$(MUSL_ROOT)/bin/musl-gcc $(MUSL_GCC_FLAGS) -static -O2 -o $@ $<

.PHONY: prepare-offline-c-toolchain test-offline-c-baseline-riscv test-offline-c-riscv
prepare-offline-c-toolchain:
	python3 tests/workloads/toolchain/prepare_alpine.py

test-offline-c-baseline-riscv: $(OFFLINE_C_RV) $(KERNEL_RV)
	PYTHONDONTWRITEBYTECODE=1 python3 tests/offline-c-riscv.py \
		--kernel $(KERNEL_RV) --program $(OFFLINE_C_RV) \
		$(if $(OFFLINE_C_LINUX_KERNEL),--linux-kernel $(OFFLINE_C_LINUX_KERNEL)) \
		--qemu $(QEMU_RISCV64) --expect-first-failure preprocess:exec:2

test-offline-c-riscv: $(OFFLINE_C_RV) $(KERNEL_RV) prepare-offline-c-toolchain
	PYTHONDONTWRITEBYTECODE=1 python3 tests/offline-c-riscv.py \
		--kernel $(KERNEL_RV) --program $(OFFLINE_C_RV) \
		$(if $(OFFLINE_C_LINUX_KERNEL),--linux-kernel $(OFFLINE_C_LINUX_KERNEL)) \
		--qemu $(QEMU_RISCV64) \
		--toolchain-tree build/offline-c/alpine-tree

.PHONY: test-offline-c-tmpfs-riscv
test-offline-c-tmpfs-riscv: $(OFFLINE_C_RV) $(KERNEL_RV) prepare-offline-c-toolchain
	PYTHONDONTWRITEBYTECODE=1 python3 tests/offline-c-riscv.py \
		--kernel $(KERNEL_RV) --program $(OFFLINE_C_RV) --tmpfs \
		$(if $(OFFLINE_C_LINUX_KERNEL),--linux-kernel $(OFFLINE_C_LINUX_KERNEL)) \
		--qemu $(QEMU_RISCV64) \
		--toolchain-tree build/offline-c/alpine-tree

.PHONY: test-sqlite-rollback-riscv
test-sqlite-rollback-riscv: $(SQLITE_ROLLBACK_RV) $(SQLITE_CLI_STATIC_RV) $(SQLITE_CLI_DYNAMIC_RV) $(SQLITE_CLI_INIT_RV) $(MUSL_LDSO) $(KERNEL_RV)
	SQLITE_ROLLBACK_RV=$(SQLITE_ROLLBACK_RV) \
	SQLITE_CLI_STATIC_RV=$(SQLITE_CLI_STATIC_RV) \
	SQLITE_CLI_DYNAMIC_RV=$(SQLITE_CLI_DYNAMIC_RV) \
	SQLITE_CLI_INIT_RV=$(SQLITE_CLI_INIT_RV) MUSL_LDSO=$(MUSL_LDSO) \
	QEMU_RISCV64=$(QEMU_RISCV64) \
		./tests/sqlite-rollback-riscv.sh
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

$(REAL_USERLAND_RV): tests/userland/real.c tests/userland/truncate.h tests/userland/timestamps.h tests/userland/sync.h tests/userland/namespace.h tests/userland/metadata.h tests/userland/shared_mapping.h tests/userland/shared_futex.h tests/userland/tmpfs.h tests/userland/sysv_shm.h tests/userland/fault_signals.h tests/userland/write_operations.h $(MUSL_STAMP)
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

.PHONY: test-glibc-riscv
test-glibc-riscv: $(KERNEL_RV)
	PYTHONDONTWRITEBYTECODE=1 python3 tests/userland/glibc/run.py \
		--kernel $(KERNEL_RV) --qemu $(QEMU_RISCV64)

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

MULTI_MOUNT_RV := $(BUILD_DIR)/tests/user/multi-mount-rv
MULTI_MOUNT_READONLY_RV := $(BUILD_DIR)/tests/user/multi-mount-readonly-rv

$(MULTI_MOUNT_RV): tests/userland/multi_mount.c $(MUSL_STAMP)
	@mkdir -p $(dir $@)
	$(MUSL_ROOT)/bin/musl-gcc $(MUSL_GCC_FLAGS) -static -O2 -Wall -Wextra -Werror -o $@ $<

$(MULTI_MOUNT_READONLY_RV): tests/userland/multi_mount.c $(MUSL_STAMP)
	@mkdir -p $(dir $@)
	$(MUSL_ROOT)/bin/musl-gcc $(MUSL_GCC_FLAGS) -DREAD_ONLY_BOOT=1 -static -O2 -Wall -Wextra -Werror -o $@ $<

.PHONY: test-root-multi-block-riscv
test-root-multi-block-riscv: $(KERNEL_RV) $(MULTI_MOUNT_RV) $(MULTI_MOUNT_READONLY_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) QEMU_MEMORY=$(QEMU_MEMORY) \
		KERNEL_RV=$(KERNEL_RV) MULTI_MOUNT_RV=$(MULTI_MOUNT_RV) \
		MULTI_MOUNT_READONLY_RV=$(MULTI_MOUNT_READONLY_RV) \
		./tests/root-multi-block-riscv.sh

.PHONY: test-root-orphan-riscv
test-root-orphan-riscv: $(KERNEL_RV) $(ROOT_ORPHAN_PROGRAM_RV) \
		$(ROOT_EXEC_STAGE2_RV) $(ROOT_EXEC_STAGE3_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) QEMU_MEMORY=$(QEMU_MEMORY) \
		KERNEL_RV=$(KERNEL_RV) \
		ROOT_INIT_PROGRAM_RV=$(ROOT_ORPHAN_PROGRAM_RV) \
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
		ROOT_BOOT_ERROR_STATUS=0xb \
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

.PHONY: prune-build
prune-build:
	python3 tests/prune-build.py --apply

-include $(DEPS)

.PHONY: test-allocator-release-host
build/host/lwip-port: tests/host/lwip_port_test.c \
		net/lwip_port/port.c $(filter third_party/lwip/%,$(LWIP_SOURCES))
	@mkdir -p $(dir $@)
	cc -std=c11 -Wall -Wextra -Werror -Inet/lwip_port/include \
		-Ithird_party/lwip/src/include -idirafter include \
		tests/host/lwip_port_test.c net/lwip_port/port.c \
		$(filter third_party/lwip/%,$(LWIP_SOURCES)) -o $@

.PHONY: test-lwip-host
test-lwip-host: build/host/lwip-port
	$<

test-allocator-release-host:
	mkdir -p build/host
	cc -std=c11 -Wall -Wextra -Werror -DBOAROS_PAGE_SHIFT=12 -Itests/host/random -Iinclude tests/host/allocator_release.c kernel/physical_page.c mm/heap.c -o build/host/allocator-release
	build/host/allocator-release

include tests/diff-abi/Makefile.inc

.PHONY: test-block-host
test-block-host:
	mkdir -p build/host
	cc -std=c11 -Wall -Wextra -Werror -idirafter include tests/host/block_flush.c kernel/block.c -o build/host/block-flush
	build/host/block-flush
	cc -std=c11 -Wall -Wextra -Werror -idirafter include tests/host/block_registry.c kernel/block.c -o build/host/block-registry
	build/host/block-registry
	cc -std=c11 -Wall -Wextra -Werror -idirafter include tests/host/block_fault_test.c tests/host/block_fault.c kernel/block.c -o build/host/block-fault
	build/host/block-fault

.PHONY: test-record-lock-host
test-record-lock-host:
	mkdir -p build/host
	cc -std=c11 -Wall -Wextra -Werror -idirafter include tests/host/record_lock_test.c -o build/host/record-lock
	build/host/record-lock

build/host/nbd-fault: tests/host/nbd_fault.c tests/host/block_fault.c tests/host/block_fault.h kernel/block.c
	@mkdir -p $(dir $@)
	cc -std=c11 -Wall -Wextra -Werror -idirafter include tests/host/nbd_fault.c tests/host/block_fault.c kernel/block.c -o $@

.PHONY: test-nbd-host
test-nbd-host: build/host/nbd-fault
	PYTHONDONTWRITEBYTECODE=1 python3 tests/host/nbd_fault_test.py $<

.PHONY: test-sqlite-nbd-riscv
test-sqlite-nbd-riscv: $(SQLITE_ROLLBACK_RV) $(KERNEL_RV) build/host/nbd-fault
	SQLITE_ROLLBACK_RV=$(SQLITE_ROLLBACK_RV) KERNEL_RV=$(KERNEL_RV) \
	NBD_FAULT_SERVER=build/host/nbd-fault QEMU_RISCV64=$(QEMU_RISCV64) \
		./tests/sqlite-nbd-riscv.sh

.PHONY: test-sqlite-recovery-riscv
test-sqlite-recovery-riscv: $(SQLITE_RECOVERY_RV) $(KERNEL_RV) build/host/nbd-fault
	PYTHONDONTWRITEBYTECODE=1 python3 tests/sqlite-recovery-riscv.py \
		--kernel $(KERNEL_RV) --program $(SQLITE_RECOVERY_RV) \
		--server build/host/nbd-fault --qemu $(QEMU_RISCV64) --linux

.PHONY: test-record-lock-riscv
test-record-lock-riscv: $(LOCK_LIFECYCLE_RV) $(KERNEL_RV)
	PYTHONDONTWRITEBYTECODE=1 python3 tests/record-lock-riscv.py \
		--kernel $(KERNEL_RV) --program $(LOCK_LIFECYCLE_RV) \
		--qemu $(QEMU_RISCV64)

.PHONY: test-sqlite-recovery-matrix-riscv
test-sqlite-recovery-matrix-riscv: $(SQLITE_RECOVERY_RV) $(KERNEL_RV) build/host/nbd-fault
	PYTHONDONTWRITEBYTECODE=1 python3 tests/sqlite-recovery-riscv.py \
		--kernel $(KERNEL_RV) --program $(SQLITE_RECOVERY_RV) \
		--server build/host/nbd-fault --qemu $(QEMU_RISCV64) --matrix full

.PHONY: test-sqlite-wal-recovery-riscv test-sqlite-wal-recovery-matrix-riscv
test-sqlite-wal-recovery-riscv: $(SQLITE_RECOVERY_RV) $(KERNEL_RV) build/host/nbd-fault
	PYTHONDONTWRITEBYTECODE=1 python3 tests/sqlite-recovery-riscv.py \
		--kernel $(KERNEL_RV) --program $(SQLITE_RECOVERY_RV) \
		--server build/host/nbd-fault --qemu $(QEMU_RISCV64) --linux --journal wal

test-sqlite-wal-recovery-matrix-riscv: $(SQLITE_RECOVERY_RV) $(KERNEL_RV) build/host/nbd-fault
	PYTHONDONTWRITEBYTECODE=1 python3 tests/sqlite-recovery-riscv.py \
		--kernel $(KERNEL_RV) --program $(SQLITE_RECOVERY_RV) \
		--server build/host/nbd-fault --qemu $(QEMU_RISCV64) --journal wal --matrix full

# Rebuild only production code in isolation: stale .su files and boot-only
# test fixture frames cannot silently satisfy or distort this gate.
.PHONY: test-stack-usage
test-stack-usage:
	mkdir -p build/stack-usage
	$(MAKE) -B BUILD_DIR=build/stack-usage KERNEL_RV=build/stack-usage/kernel-rv all >build/stack-usage/build.log 2>&1
	cc $(CPPFLAGS) -std=c11 -Wall -Wextra -Werror tests/stack-budget.c -o build/stack-usage/stack-budget
	python3 tests/test-stack-usage.py
	python3 tests/stack-usage.py build/stack-usage $$(build/stack-usage/stack-budget) >build/stack-usage/report.log
	cat build/stack-usage/report.log

.PHONY: inventory-userland-riscv test-program-inventory-host
test-program-inventory-host:
	PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests/program-inventory -p 'test_*.py'

# Program incompatibilities remain inventory records unless --require-pass
# is requested; build and runner errors always fail.
inventory-userland-riscv: $(KERNEL_RV) $(MUSL_STAMP) test-program-inventory-host
	python3 tests/program-inventory/run.py

include tests/program-inventory/Makefile.inc

.PHONY: test-elf-tail-riscv
test-elf-tail-riscv: $(KERNEL_RV) $(MUSL_STAMP)
	python3 tests/elf-tail-riscv.py

$(ELF_RWX_PROGRAM_RV): tests/riscv/elf_rwx_main.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -nostdlib -static -no-pie -Wl,--build-id=none \
		-Wl,-e,_start -Wl,-N -Wl,-Ttext=0x10000 -o $@ $<

.PHONY: test-elf-rwx-riscv
test-elf-rwx-riscv: $(KERNEL_RV) $(ELF_RWX_PROGRAM_RV)
	KERNEL_RV=$(KERNEL_RV) ELF_RWX_PROGRAM_RV=$(ELF_RWX_PROGRAM_RV) \
		QEMU_RISCV64=$(QEMU_RISCV64) ./tests/elf-rwx-riscv.sh

SCALE_OBJECTS := $(TEST_RUNTIME_OBJECTS) \
    $(patsubst %.c,$(BUILD_DIR)/%.o,$(VFS_TEST_SUPPORT_C_SOURCES)) \
    $(BUILD_DIR)/tests/riscv/scale_main.o
-include $(BUILD_DIR)/tests/riscv/scale_main.d
$(BUILD_DIR)/tests/kernel-scale-rv: $(SCALE_OBJECTS) arch/riscv/linker.ld
	$(CC) $(LDFLAGS) -Wl,--wrap=riscv_sv39_current_satp \
		-Wl,--wrap=physical_page_allocate -Wl,--wrap=kernel_heap_allocate_zeroed \
		-Wl,--wrap=kernel_heap_resize -Wl,--wrap=kernel_heap_allocate \
		-Wl,--wrap=kernel_copy_from_user \
		-Wl,--wrap=kernel_wait_queue_wake_all \
		-o $@ $(SCALE_OBJECTS)
.PHONY: test-scale-riscv
test-scale-riscv: $(BUILD_DIR)/tests/kernel-scale-rv
	python3 tests/scale-riscv.py --kernel $< --qemu $(QEMU_RISCV64)

IO_SLEEP_OBJECTS := $(TEST_RUNTIME_OBJECTS) $(BUILD_DIR)/kernel/dtb.o $(BUILD_DIR)/tests/riscv/io_sleep_main.o
-include $(BUILD_DIR)/tests/riscv/io_sleep_main.d
$(BUILD_DIR)/tests/kernel-io-sleep-rv: $(IO_SLEEP_OBJECTS) arch/riscv/linker.ld
	$(CC) $(LDFLAGS) -Wl,--wrap=kernel_vfs_node_pread -Wl,--wrap=kernel_vfs_node_writeback -Wl,--wrap=kernel_open_file_sync_range -Wl,--wrap=kernel_open_file_release -Wl,--wrap=riscv_sv39_current_satp -Wl,--wrap=kernel_copy_from_user -Wl,--wrap=kernel_copy_to_user -Wl,--wrap=kernel_task_current -Wl,--wrap=kernel_vfs_node_lock -Wl,--wrap=kernel_open_file_get_page -o $@ $(IO_SLEEP_OBJECTS)
.PHONY: test-io-sleep-riscv
test-io-sleep-riscv: $(BUILD_DIR)/tests/kernel-io-sleep-rv build/host/nbd-fault
	python3 tests/io-sleep-riscv.py --kernel $< --qemu $(QEMU_RISCV64)

SQLITE_SECOND_DISK_RV := $(BUILD_DIR)/tests/user/sqlite-second-disk-rv
$(SQLITE_SECOND_DISK_RV): tests/workloads/sqlite/wal.c $(SQLITE_SOURCE) $(MUSL_STAMP)
	@mkdir -p $(dir $@)
	$(MUSL_ROOT)/bin/musl-gcc $(MUSL_GCC_FLAGS) -static -O2 -pthread \
		-DSQLITE_SECOND_DISK -I$(dir $(SQLITE_SOURCE)) -o $@ $< $(SQLITE_SOURCE) -ldl
.PHONY: test-sqlite-second-disk-riscv
test-sqlite-second-disk-riscv: $(SQLITE_SECOND_DISK_RV) $(KERNEL_RV)
	PYTHONDONTWRITEBYTECODE=1 python3 tests/sqlite-wal-riscv.py \
		--kernel $(KERNEL_RV) --program $(SQLITE_SECOND_DISK_RV) \
		--qemu $(QEMU_RISCV64) --second-disk

MULTI_DISK_IO_RV := $(BUILD_DIR)/tests/user/multi-disk-io-rv
$(MULTI_DISK_IO_RV): tests/userland/multi_disk_io.c $(MUSL_STAMP)
	@mkdir -p $(dir $@)
	$(MUSL_ROOT)/bin/musl-gcc $(MUSL_GCC_FLAGS) -static -O2 -o $@ $<
.PHONY: test-multi-disk-io-riscv
test-multi-disk-io-riscv: $(KERNEL_RV) $(MULTI_DISK_IO_RV) build/host/nbd-fault
	PYTHONDONTWRITEBYTECODE=1 python3 tests/multi-disk-io-riscv.py \
		--kernel $(KERNEL_RV) --program $(MULTI_DISK_IO_RV) --qemu $(QEMU_RISCV64)

.PHONY: test-pid-object-host test-random-host
test-pid-object-host:
	@mkdir -p build/host
	cc -std=c11 -Wall -Wextra -Werror -Iinclude tests/pid-object-host.c kernel/pid.c -o build/host/pid-object-test
	build/host/pid-object-test

test-random-host:
	PYTHONDONTWRITEBYTECODE=1 python3 tests/host/random_vectors.py

RNG_USER_RV := $(BUILD_DIR)/tests/user/rng-rv
$(RNG_USER_RV): tests/userland/rng.c $(MUSL_STAMP)
	@mkdir -p $(@D)
	$(MUSL_ROOT)/bin/musl-gcc $(MUSL_GCC_FLAGS) -static -O2 -Wall -Wextra -Werror -o $@ $<

.PHONY: test-rng-riscv test-virtio-rng-host
test-rng-riscv: $(KERNEL_RV) $(RNG_USER_RV)
	python3 -B tests/rng-riscv.py --kernel $(KERNEL_RV) --program $(RNG_USER_RV) --qemu $(QEMU_RISCV64)

test-virtio-rng-host:
	@mkdir -p build/host
	cc -std=c11 -Wall -Wextra -Werror -Itests/host/random -Iinclude \
		tests/host/virtio_rng_test.c arch/riscv/virtio_mmio_rng.c -o build/host/virtio-rng
	build/host/virtio-rng

.PHONY: test-sched-policy-host
test-sched-policy-host:
	@mkdir -p build/host
	cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -Iinclude \
		tests/host/sched_policy_test.c kernel/sched/policy.c -o build/host/sched-policy
	build/host/sched-policy
	cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -Iinclude \
		tests/host/sched_runqueue_test.c kernel/sched/runqueue.c -o build/host/sched-runqueue
	build/host/sched-runqueue
	cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -Itests/host/random -Iinclude \
		tests/host/sched_syscall_test.c kernel/syscall/sched.c kernel/sched/policy.c -o build/host/sched-syscall
	build/host/sched-syscall

.PHONY: test-multi-disk-rt-riscv
test-multi-disk-rt-riscv: $(KERNEL_RV) $(MULTI_DISK_IO_RV) build/host/nbd-fault
	PYTHONDONTWRITEBYTECODE=1 python3 tests/multi-disk-io-riscv.py \
		--kernel $(KERNEL_RV) --program $(MULTI_DISK_IO_RV) --qemu $(QEMU_RISCV64) --rt-load

.PHONY: test-sched-bandwidth-riscv
test-sched-bandwidth-riscv: $(KERNEL_RV) $(MUSL_STAMP)
	KERNEL_RV=$(KERNEL_RV) QEMU_RISCV64=$(QEMU_RISCV64) sh tests/sched-bandwidth-riscv.sh

# Cost variants never reuse objects compiled with a different observation flag.
.PHONY: force-cost-config test-cost-riscv test-cost-host
force-cost-config:
$(BUILD_DIR)/generated/cost-config.h: force-cost-config
	@mkdir -p $(dir $@)
	@printf '#define BOAROS_COST_DIAGNOSTICS %s\n' '$(COST_DIAGNOSTICS)' > $@.tmp
	@cmp -s $@ $@.tmp && rm $@.tmp || mv $@.tmp $@
COST_CASE ?= all
test-cost-host:
	@mkdir -p build/cost/host
	cc -std=c11 -Wall -Wextra -Werror -idirafter include -DBOAROS_COST_DIAGNOSTICS=1 tests/cost/core_test.c kernel/cost.c -o build/cost/host/core-test
	build/cost/host/core-test
	cc -std=c11 -Wall -Wextra -Werror -idirafter include -DBOAROS_COST_DIAGNOSTICS=1 tests/cost/account_test.c kernel/cost.c -o build/cost/host/account-test
	build/cost/host/account-test
	cc -std=c11 -Wall -Wextra -Werror -idirafter include -DBOAROS_COST_DIAGNOSTICS=1 tests/cost/irq_test.c kernel/cost.c -o build/cost/host/irq-test
	build/cost/host/irq-test
	cc -std=c11 -Wall -Wextra -Werror -Itests/host/random -idirafter include -DBOAROS_PAGE_SHIFT=12 -DBOAROS_COST_DIAGNOSTICS=1 tests/cost/page_test.c kernel/cost.c kernel/physical_page.c -o build/cost/host/page-test
	build/cost/host/page-test
	cc -std=c11 -Wall -Wextra -Werror -Itests/host/random -idirafter include -DBOAROS_PAGE_SHIFT=12 -DBOAROS_COST_DIAGNOSTICS=1 tests/memory/cost_test.c kernel/cost.c kernel/physical_page.c mm/heap.c arch/riscv/uaccess.c -o build/cost/host/memory-test
	build/cost/host/memory-test
	python3 -B tests/test-cost-report.py
test-cost-riscv: test-cost-host
	$(MAKE) COST_DIAGNOSTICS=1 all
	python3 -B tests/cost-riscv.py --kernel build/cost/kernel-rv --qemu $(QEMU_RISCV64) --case $(COST_CASE)

.PHONY: test-log-host
test-log-host:
	@mkdir -p build/host
	cc -D_GNU_SOURCE -std=c11 -Wall -Wextra -Werror -Itests/host/random -idirafter include tests/host/kernel_log.c kernel/log.c -o build/host/log-test
	build/host/log-test

.PHONY: test-environment-riscv
test-environment-riscv: $(KERNEL_RV)
	KERNEL_RV=$(KERNEL_RV) QEMU_RISCV64=$(QEMU_RISCV64) sh tests/environment-riscv.sh

.PHONY: test-rtc-host
test-rtc-host:
	@mkdir -p build/host
	cc -std=c11 -Wall -Wextra -Werror -Itests/host/random -idirafter include tests/host/rtc_device.c fs/rtc_device.c -o build/host/rtc-test
	build/host/rtc-test

.PHONY: test-network-riscv
test-network-riscv: kernel-rv
	python3 -B tests/network-riscv.py

.PHONY: test-virtio-net-host test-lwip-reassembly-host test-ethernet-worker-host test-network-external-riscv force-net-config
NET_IPV4 ?= 0x0a4d0002
NET_NETMASK ?= 0xffffff00
force-net-config:
$(BUILD_DIR)/generated/net-config.h: force-net-config
	@mkdir -p $(dir $@)
	@printf '#define BOAROS_NET_IPV4 %sU\n#define BOAROS_NET_NETMASK %sU\n' '$(NET_IPV4)' '$(NET_NETMASK)' > $@.tmp
	@cmp -s $@ $@.tmp && rm $@.tmp || mv $@.tmp $@
$(BUILD_DIR)/net/ethernet.o: $(BUILD_DIR)/generated/net-config.h
$(BUILD_DIR)/net/ethernet.o: CPPFLAGS += -I$(BUILD_DIR)/generated
test-virtio-net-host:
	python3 -B tests/host/virtio_net.py
test-lwip-reassembly-host:
	sh tests/host/lwip_reassembly.sh

test-ethernet-worker-host:
	python3 -B tests/host/ethernet_worker.py
test-network-external-riscv: $(KERNEL_RV)
	python3 -B tests/network-external.py --kernel $(KERNEL_RV) --transport both

.PHONY: test-allocator-preemption-host
test-allocator-preemption-host:
	mkdir -p build/host/allocator
	cc -std=c11 -O1 -fno-inline -finstrument-functions -Wall -Wextra -Werror -DBOAROS_PAGE_SHIFT=12 -Itests/host/allocator -Iinclude -c kernel/physical_page.c -o build/host/allocator/page.o
	cc -std=c11 -O1 -fno-inline -finstrument-functions -Wall -Wextra -Werror -DBOAROS_PAGE_SHIFT=12 -Itests/host/allocator -Iinclude -c mm/heap.c -o build/host/allocator/heap.o
	cc -std=c11 -Wall -Wextra -Werror -DBOAROS_PAGE_SHIFT=12 -Iinclude tests/host/allocator_preemption.c build/host/allocator/page.o build/host/allocator/heap.o -o build/host/allocator/preemption
	build/host/allocator/preemption

.PHONY: test-fifo-riscv
test-fifo-riscv: $(KERNEL_RV)
	python3 -B tests/fifo-riscv.py --kernel $(KERNEL_RV)

OFFLINE_PROJECT_RV := $(BUILD_DIR)/tests/user/offline-project-rv
$(OFFLINE_PROJECT_RV): tests/workloads/toolchain/project.c $(MUSL_STAMP)
	@mkdir -p $(dir $@)
	$(MUSL_ROOT)/bin/musl-gcc $(MUSL_GCC_FLAGS) -static -O2 -Wall -Wextra -Werror $< -o $@

.PHONY: test-offline-project-riscv test-offline-project-tmpfs-riscv
test-offline-project-riscv: $(OFFLINE_PROJECT_RV) $(KERNEL_RV) prepare-offline-c-toolchain
	python3 -B tests/offline-c-riscv.py --project lua --kernel $(KERNEL_RV) --program $(OFFLINE_PROJECT_RV) --toolchain-tree build/offline-c/alpine-tree --timeout 900

test-offline-project-tmpfs-riscv: $(OFFLINE_PROJECT_RV) $(KERNEL_RV) prepare-offline-c-toolchain
	python3 -B tests/offline-c-riscv.py --project lua --kernel $(KERNEL_RV) --program $(OFFLINE_PROJECT_RV) --toolchain-tree build/offline-c/alpine-tree --timeout 900 --tmpfs --performance

.PHONY: test-uart-host
test-uart-host:
	python3 -B tests/host/dtb_uart_test.py
	./tests/uart-host.sh

.PHONY: test-tty-host
test-tty-host:
	@mkdir -p build/host/tty
	cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -DBOAROS_PAGE_SHIFT=12 -Itests/host/random -idirafter include -fsanitize=address,undefined tests/tty/core_host.c fs/tty.c -o build/host/tty/core
	build/host/tty/core
	cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -DBOAROS_PAGE_SHIFT=12 -Itests/host/random -idirafter include -fsanitize=address,undefined tests/tty/flags_host.c fs/tty.c -o build/host/tty/flags
	build/host/tty/flags
	cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -DBOAROS_PAGE_SHIFT=12 -idirafter include -ffunction-sections -fdata-sections -Wl,--gc-sections -fsanitize=address,undefined tests/tty/group_host.c kernel/sched/tty.c kernel/pid.c -o build/host/tty/group
	build/host/tty/group

TTY_PROBE_RV := $(BUILD_DIR)/tests/user/tty-probe-rv
TTY_JOBCTRL_RV := $(BUILD_DIR)/tests/user/tty-jobctrl-rv
$(TTY_PROBE_RV): tests/tty/probe.c $(MUSL_STAMP)
	@mkdir -p $(dir $@)
	$(MUSL_ROOT)/bin/musl-gcc $(MUSL_GCC_FLAGS) -static -pthread -O2 -Wall -Wextra -Werror $< -o $@
$(TTY_JOBCTRL_RV): tests/tty/jobctrl_probe.c $(MUSL_STAMP)
	@mkdir -p $(dir $@)
	$(MUSL_ROOT)/bin/musl-gcc $(MUSL_GCC_FLAGS) -static -O2 -Wall -Wextra -Werror $< -o $@

.PHONY: test-tty-riscv test-tty-diff-riscv
test-tty-riscv: $(KERNEL_RV) $(MUSL_STAMP)
	python3 -B tests/tty/riscv.py --kernel $(KERNEL_RV) --qemu $(QEMU_RISCV64)
test-tty-diff-riscv: $(KERNEL_RV) $(TTY_PROBE_RV) $(TTY_JOBCTRL_RV)
	python3 -B tests/tty/riscv.py --kernel $(KERNEL_RV) --qemu $(QEMU_RISCV64) --probe $(TTY_PROBE_RV)
	python3 -B tests/tty/riscv.py --kernel $(KERNEL_RV) --qemu $(QEMU_RISCV64) --probe $(TTY_JOBCTRL_RV) --no-ctty
