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
# CI显式选择共同RAM矩阵；空值保留各聚焦入口原来的默认配置。
TEST_MEMORIES ?=
ifneq ($(filter-out 512M 1G,$(TEST_MEMORIES)),)
$(error TEST_MEMORIES must contain only 512M or 1G)
endif
TEST_MEMORY_ARGS := $(foreach memory,$(TEST_MEMORIES),--memory $(memory))

COST_DIAGNOSTICS ?= 0
LOCK_DEBUG ?= 0
SMP ?= 0
SMP_CPUS ?= 2
ifneq ($(filter $(SMP_CPUS),2 4),$(SMP_CPUS))
$(error SMP_CPUS must be 2 or 4)
endif
SMP_BUILD_SUFFIX := $(if $(filter 4,$(SMP_CPUS)),4,)
USER_SMP ?= 0
USER_SMP_BUILD ?= build/riscv-user-smp$(SMP_BUILD_SUFFIX)
USER_SMP_COMPOSITION ?= 0
ifneq ($(filter $(USER_SMP_COMPOSITION),0 1),$(USER_SMP_COMPOSITION))
$(error USER_SMP_COMPOSITION must be 0 or 1)
endif
ifeq ($(USER_SMP_COMPOSITION),1)
ifneq ($(SMP)$(USER_SMP),11)
$(error USER_SMP_COMPOSITION requires SMP=1 USER_SMP=1)
endif
endif
ifneq ($(filter $(USER_SMP),0 1),$(USER_SMP))
$(error USER_SMP must be 0 or 1)
endif
ifeq ($(USER_SMP),1)
ifneq ($(SMP),1)
$(error USER_SMP requires SMP=1)
endif
endif
SMP_TEST ?= 0
MM_SMP_TEST ?= 0
MM_SHARED_SMP_TEST ?= 0
MM_FILE_SMP_TEST ?= 0
MM_DISK_SMP_TEST ?= 0
FILES_SMP_TEST ?= 0
FILES_SMP_BUILD ?= build/riscv-files-smp-test$(SMP_BUILD_SUFFIX)
FILE_IO_SMP_TEST ?= 0
FILE_IO_SMP_BUILD ?= build/riscv-file-io-smp-test$(SMP_BUILD_SUFFIX)
SMP_TEST_PROFILE ?= normal
ifneq ($(filter $(SMP_TEST_PROFILE),normal missing-rfence denied-start handshake-timeout guard denied-stop rfence-error mm-fence-i),$(SMP_TEST_PROFILE))
$(error invalid SMP_TEST_PROFILE)
endif
SMP_TEST_SUFFIX := $(if $(filter normal,$(SMP_TEST_PROFILE)),,$(SMP_TEST_PROFILE)/)
SMP_NEGATIVE := $(if $(filter handshake-timeout,$(SMP_TEST_PROFILE)),7,$(if $(filter missing-rfence,$(SMP_TEST_PROFILE)),1,$(if $(filter denied-start,$(SMP_TEST_PROFILE)),2,$(if $(filter guard,$(SMP_TEST_PROFILE)),3,$(if $(filter denied-stop,$(SMP_TEST_PROFILE)),4,$(if $(filter rfence-error,$(SMP_TEST_PROFILE)),5,$(if $(filter mm-fence-i,$(SMP_TEST_PROFILE)),6,0)))))))
ifneq ($(filter $(SMP_TEST),0 1),$(SMP_TEST))
$(error SMP_TEST must be 0 or 1)
endif
ifeq ($(SMP_TEST_PROFILE),mm-fence-i)
ifneq ($(MM_SMP_TEST),1)
$(error mm-fence-i requires MM_SMP_TEST=1)
endif
endif
ifneq ($(filter $(SMP),0 1),$(SMP))
$(error SMP must be 0 or 1)
endif
ifeq ($(SMP_TEST),1)
ifeq ($(SMP),0)
$(error SMP_TEST requires SMP=1)
endif
endif
ifneq ($(filter $(MM_SMP_TEST),0 1),$(MM_SMP_TEST))
$(error MM_SMP_TEST must be 0 or 1)
endif
ifeq ($(MM_SMP_TEST),1)
ifneq ($(SMP)$(SMP_TEST),11)
$(error MM_SMP_TEST requires SMP=1 SMP_TEST=1)
endif
ifneq ($(filter $(SMP_TEST_PROFILE),normal rfence-error mm-fence-i),$(SMP_TEST_PROFILE))
$(error MM_SMP_TEST profile must be normal, rfence-error or mm-fence-i)
endif
endif
ifneq ($(filter $(MM_SHARED_SMP_TEST),0 1),$(MM_SHARED_SMP_TEST))
$(error MM_SHARED_SMP_TEST must be 0 or 1)
endif
ifeq ($(MM_SHARED_SMP_TEST),1)
ifneq ($(MM_SMP_TEST),1)
$(error MM_SHARED_SMP_TEST requires MM_SMP_TEST=1)
endif
ifneq ($(filter $(SMP_TEST_PROFILE),normal rfence-error),$(SMP_TEST_PROFILE))
$(error MM_SHARED_SMP_TEST profile must be normal or rfence-error)
endif
endif
ifneq ($(filter $(MM_FILE_SMP_TEST),0 1),$(MM_FILE_SMP_TEST))
$(error MM_FILE_SMP_TEST must be 0 or 1)
endif
ifneq ($(filter $(MM_DISK_SMP_TEST),0 1),$(MM_DISK_SMP_TEST))
$(error MM_DISK_SMP_TEST must be 0 or 1)
endif
ifeq ($(MM_DISK_SMP_TEST),1)
ifneq ($(MM_FILE_SMP_TEST),1)
$(error MM_DISK_SMP_TEST requires MM_FILE_SMP_TEST=1)
endif
endif
ifeq ($(MM_FILE_SMP_TEST),1)
ifneq ($(MM_SHARED_SMP_TEST),1)
$(error MM_FILE_SMP_TEST requires MM_SHARED_SMP_TEST=1)
endif
endif
ifneq ($(filter $(FILES_SMP_TEST),0 1),$(FILES_SMP_TEST))
$(error FILES_SMP_TEST must be 0 or 1)
endif
ifeq ($(FILES_SMP_TEST),1)
ifneq ($(MM_DISK_SMP_TEST),1)
$(error FILES_SMP_TEST requires the controlled disk MM fixture)
endif
endif
ifneq ($(filter $(FILE_IO_SMP_TEST),0 1),$(FILE_IO_SMP_TEST))
$(error FILE_IO_SMP_TEST must be 0 or 1)
endif
ifeq ($(FILE_IO_SMP_TEST),1)
ifneq ($(FILES_SMP_TEST),1)
$(error FILE_IO_SMP_TEST requires the controlled FD fixture)
endif
endif
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
ifeq ($(SMP),1)
BUILD_DIR := build/riscv-smp$(SMP_BUILD_SUFFIX)/riscv
KERNEL_RV := build/riscv-smp$(SMP_BUILD_SUFFIX)/kernel-rv
CPU_CAPACITY := $(SMP_CPUS)
else
CPU_CAPACITY := 1
endif
ifeq ($(SMP_TEST),1)
BUILD_DIR := build/riscv-smp$(SMP_BUILD_SUFFIX)-test/riscv
KERNEL_RV := build/riscv-smp$(SMP_BUILD_SUFFIX)-test/kernel-rv
BUILD_DIR := build/riscv-smp$(SMP_BUILD_SUFFIX)-test/$(SMP_TEST_SUFFIX)riscv
KERNEL_RV := build/riscv-smp$(SMP_BUILD_SUFFIX)-test/$(SMP_TEST_SUFFIX)kernel-rv
endif
ifeq ($(MM_SMP_TEST),1)
BUILD_DIR := build/riscv-mm-smp-test$(SMP_BUILD_SUFFIX)/$(SMP_TEST_SUFFIX)riscv
KERNEL_RV := build/riscv-mm-smp-test$(SMP_BUILD_SUFFIX)/$(SMP_TEST_SUFFIX)kernel-rv
endif
ifeq ($(MM_SHARED_SMP_TEST),1)
BUILD_DIR := build/riscv-mm-shared-smp-test$(SMP_BUILD_SUFFIX)/$(SMP_TEST_SUFFIX)riscv
KERNEL_RV := build/riscv-mm-shared-smp-test$(SMP_BUILD_SUFFIX)/$(SMP_TEST_SUFFIX)kernel-rv
endif
ifeq ($(MM_FILE_SMP_TEST),1)
BUILD_DIR := build/riscv-mm-file-smp-test$(SMP_BUILD_SUFFIX)/$(SMP_TEST_SUFFIX)riscv
KERNEL_RV := build/riscv-mm-file-smp-test$(SMP_BUILD_SUFFIX)/$(SMP_TEST_SUFFIX)kernel-rv
endif
ifeq ($(MM_DISK_SMP_TEST),1)
BUILD_DIR := build/riscv-mm-disk-smp-test$(SMP_BUILD_SUFFIX)/$(SMP_TEST_SUFFIX)riscv
KERNEL_RV := build/riscv-mm-disk-smp-test$(SMP_BUILD_SUFFIX)/$(SMP_TEST_SUFFIX)kernel-rv
endif
ifeq ($(FILES_SMP_TEST),1)
BUILD_DIR := $(FILES_SMP_BUILD)/$(SMP_TEST_SUFFIX)riscv
KERNEL_RV := $(FILES_SMP_BUILD)/$(SMP_TEST_SUFFIX)kernel-rv
endif
ifeq ($(FILE_IO_SMP_TEST),1)
BUILD_DIR := $(FILE_IO_SMP_BUILD)/$(SMP_TEST_SUFFIX)riscv
KERNEL_RV := $(FILE_IO_SMP_BUILD)/$(SMP_TEST_SUFFIX)kernel-rv
endif
ifeq ($(USER_SMP),1)
BUILD_DIR := $(USER_SMP_BUILD)/riscv
KERNEL_RV := $(USER_SMP_BUILD)/kernel-rv
endif
ifeq ($(SMP)$(COST_DIAGNOSTICS),11)
BUILD_DIR := build/cost-smp$(SMP_BUILD_SUFFIX)/riscv
KERNEL_RV := build/cost-smp$(SMP_BUILD_SUFFIX)/kernel-rv
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
STACK_GUARD_TEST_KERNEL_RV := $(BUILD_DIR)/tests/kernel-stack-guard-rv
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
CPPFLAGS := -DBOAROS_LOCK_DEBUG=$(LOCK_DEBUG) -DBOAROS_COST_DIAGNOSTICS=$(COST_DIAGNOSTICS) -Iinclude -DBOAROS_PAGE_SHIFT=12 \
	-DBOAROS_UTS_MACHINE=\"riscv64\"
CPPFLAGS += -DBOAROS_MAX_CPUS=$(CPU_CAPACITY)
CPPFLAGS += -DBOAROS_USER_SMP=$(USER_SMP)
CPPFLAGS += -DBOAROS_USER_SMP_COMPOSITION=$(USER_SMP_COMPOSITION)
CPPFLAGS += -DBOAROS_SMP_TEST=$(SMP_TEST) -DBOAROS_MM_DISK_TEST=$(MM_DISK_SMP_TEST)
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
	third_party/lwext4/src/ext4_xattr.c \
	third_party/lwext4/src/ext4_ialloc.c \
	third_party/lwext4/src/ext4_inode.c \
	third_party/lwext4/src/ext4_journal.c \
	third_party/lwext4/src/ext4_journal_memory.c \
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

CPU_C_SOURCES := kernel/cpu.c kernel/raw_lock.c
COMMON_C_SOURCES := \
	$(CPU_C_SOURCES) \
	kernel/elf_image.c \
	kernel/exec_image.c \
	mm/mm.c \
	mm/uaccess.c \
	drivers/serial/ns16550.c \
	drivers/virtio/block.c \
	drivers/virtio/transport.c \
	drivers/virtio/split_queue.c \
	drivers/virtio/mmio.c \
	drivers/virtio/rng.c \
	drivers/virtio/net.c \
	fs/lwext4_port.c \
	fs/files/table.c \
	fs/files/locks.c \
	fs/files/io.c \
	fs/files/range.c \
	fs/files/path.c \
	fs/files/metadata.c \
	fs/files/console.c \
	fs/files/poll.c \
	fs/files/epoll.c \
	fs/event_fd.c \
	fs/signal_fd.c \
	fs/files/socket.c \
	fs/fs_context.c \
	fs/char_device.c \
	fs/tty.c \
	fs/devpts.c \
	fs/pty.c \
	fs/rtc_device.c \
	fs/open_file.c \
	fs/block_file.c \
	fs/loop.c \
	fs/pipe.c \
	fs/record_lock.c \
	fs/page_cache.c \
	fs/vfs.c \
	fs/file_mapping.c \
	fs/procfs.c \
	fs/tmpfs.c \
	fs/disk_mount.c \
	fs/fat32.c \
	fs/ext4_backend.c \
	kernel/boot_memory.c \
	kernel/block.c \
	kernel/dtb.c \
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
	kernel/sched/scheduling.c kernel/sched/accounting.c kernel/sched/cpu_clock.c \
	kernel/sched/lifetime.c \
	kernel/sched/identity.c \
	kernel/sched/exec.c \
	kernel/sched/process.c \
	kernel/sched/proc.c \
	kernel/sched/signal.c \
	kernel/signal_stack.c \
	kernel/signal_pending.c \
	kernel/sched/tty.c \
	kernel/sched/wait.c \
	kernel/sched/park.c \
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
	kernel/syscall/event.c \
	kernel/syscall/capability.c \
	kernel/syscall/random.c \
	kernel/syscall/log.c \
	kernel/syscall/sched.c \
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

RV_C_SOURCES := \
	arch/riscv/smp.c \
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
	arch/riscv/virt_rtc.c \
	arch/riscv/virt_uart.c \
	arch/riscv/uart_tty.c \
	arch/riscv/virtio_mmio_block.c \
	arch/riscv/virtio_mmio_rng.c \
	arch/riscv/virtio_mmio_net.c \
	arch/riscv/plic.c \
	kernel/main.c \
	arch/riscv/signal.c

# CPU 与架构/MM 后端相邻；共用源只维护一次，不把它们移到整个通用内核之后。
C_SOURCES := $(CPU_C_SOURCES) $(RV_C_SOURCES) $(filter-out $(CPU_C_SOURCES),$(COMMON_C_SOURCES))
ASM_SOURCES := \
	arch/riscv/boot.S \
	arch/riscv/secondary.S \
	arch/riscv/context_switch.S \
	arch/riscv/fpu.S \
	arch/riscv/trap_entry.S \
	arch/riscv/signal_trampoline.S
# 无根模块测试复用生产运行时，由各目标追加测试入口。
TEST_RUNTIME_C_SOURCES := $(filter-out arch/riscv/smp.c arch/riscv/root_boot.c \
    kernel/main.c kernel/boot_memory.c kernel/dtb.c net/ethernet.c,\
    $(C_SOURCES))
TEST_RUNTIME_ASM_SOURCES := $(filter-out arch/riscv/secondary.S,$(ASM_SOURCES))
ifeq ($(ROOT_DRAIN_FIXTURE),1)
C_SOURCES += tests/riscv/root_drain_fixture.c
LDFLAGS += -Wl,--wrap=kernel_vfs_unmount
endif

OBJECTS := \
	$(patsubst %.c,$(BUILD_DIR)/%.o,$(C_SOURCES)) \
	$(patsubst %.S,$(BUILD_DIR)/%.o,$(ASM_SOURCES))
ifeq ($(USER_SMP_COMPOSITION),1)
OBJECTS += $(BUILD_DIR)/tests/riscv/user_smp_composition.o
LDFLAGS += -Wl,--wrap=kernel_pipe_readv,--wrap=kernel_pipe_writev,--wrap=kernel_open_file_poll,--wrap=kernel_event_fd_io,--wrap=syscall_handle_timerfd,--wrap=kernel_timer_fd_expire
LDFLAGS += -Wl,--wrap=kernel_signal_wait_take,--wrap=kernel_signal_fd_take,--wrap=kernel_signal_fd_poll,--wrap=signal_pending_prepare,--wrap=kernel_signal_release_table
LDFLAGS += -Wl,--wrap=kernel_signal_stack_prepare,--wrap=riscv_signal_restore_current
LDFLAGS += -Wl,--wrap=kernel_socket_reserve_read,--wrap=kernel_socket_stream_reserve,--wrap=kernel_socket_write_datagram,--wrap=kernel_socket_write_unix,--wrap=kernel_socket_wait_ready,--wrap=kernel_socket_service_owned,--wrap=kernel_copy_from_user,--wrap=kernel_wait_cursor_wake,--wrap=kernel_wait_backend_quiesce
LDFLAGS += -Wl,--wrap=ns16550_stop_report,--wrap=scheduler_group_usage_snapshot,--wrap=virtio_split_enqueue
LDFLAGS += -Wl,--wrap=physical_page_allocate,--wrap=physical_page_allocate_order,--wrap=kernel_mm_munmap,--wrap=kernel_open_file_prepare_mapping_page,--wrap=kernel_vfs_node_pread_batch,--wrap=riscv_smp_sync_user,--wrap=kernel_wait_park,--wrap=kernel_copy_to_user,--wrap=kernel_mm_resolve_user_fault,--wrap=virt_uart_puts,--wrap=kernel_tty_service_output,--wrap=kernel_mm_pin_user_page,--wrap=kernel_uaccess_write_cursor_copy,--wrap=kernel_mm_fork
endif
ifeq ($(SMP_TEST),1)
ifeq ($(MM_SMP_TEST),1)
OBJECTS += $(BUILD_DIR)/tests/riscv/mm_smp.o $(BUILD_DIR)/tests/riscv/mm_smp_probe.o
LDFLAGS += -Wl,--wrap=riscv_trap_dispatch,--wrap=sbi_remote_sfence,--wrap=sbi_remote_fence_i,--wrap=physical_page_allocate,--wrap=physical_page_allocate_order,--wrap=kernel_memory_object_prepare_page,--wrap=kernel_open_file_prepare_mapping_page
else
OBJECTS += $(BUILD_DIR)/tests/riscv/smp_cases.o
OBJECTS += $(BUILD_DIR)/tests/riscv/smp_registers.o
LDFLAGS += -Wl,--wrap=riscv_context_switch,--wrap=sbi_remote_sfence
LDFLAGS += -Wl,--wrap=sbi_probe_extension,--wrap=sbi_hart_start,--wrap=sbi_hart_status,--wrap=sbi_set_timer
LDFLAGS += -Wl,--wrap=physical_page_allocate
$(BUILD_DIR)/tests/riscv/smp_cases.o: CPPFLAGS += -DBOAROS_SMP_NEGATIVE=$(SMP_NEGATIVE)
endif
ifeq ($(FILES_SMP_TEST),1)
OBJECTS += $(BUILD_DIR)/tests/riscv/files_smp.o
LDFLAGS += -Wl,--wrap=kernel_wait_park
endif
ifeq ($(FILE_IO_SMP_TEST),1)
OBJECTS += $(BUILD_DIR)/tests/riscv/file_io_smp.o
$(BUILD_DIR)/tests/riscv/file_io_smp.o: CPPFLAGS += $(LWEXT4_CPPFLAGS)
LDFLAGS += -Wl,--wrap=kernel_page_cache_run_cpu0,--wrap=kernel_copy_from_user,--wrap=riscv_plic_in_interrupt,--wrap=kernel_uaccess_write_cursor_copy,--wrap=kernel_vfs_node_pread_batch
LDFLAGS += -Wl,--wrap=ext4_file_prepare_write,--wrap=ext4_ftruncate,--wrap=ext4_fallocate,--wrap=ext4_file_sync_metadata_prepare
LDFLAGS += -Wl,--wrap=virtio_split_enqueue
LDFLAGS += -Wl,--wrap=ext4_lookup_child,--wrap=ext4_create_child,--wrap=ext4_unlink_child,--wrap=ext4_rename_child,--wrap=ext4_dir_entry_next_status
endif
$(BUILD_DIR)/tests/riscv/mm_smp.o: CPPFLAGS += -DBOAROS_FILE_IO_SMP_TEST=$(FILE_IO_SMP_TEST)
$(BUILD_DIR)/tests/riscv/mm_smp.o: CPPFLAGS += -DBOAROS_FILES_SMP_TEST=$(FILES_SMP_TEST) -DBOAROS_SMP_NEGATIVE=$(SMP_NEGATIVE) -DBOAROS_MM_SHARED_TEST=$(MM_SHARED_SMP_TEST) -DBOAROS_MM_FILE_TEST=$(MM_FILE_SMP_TEST)
endif
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
STACK_GUARD_TEST_C_SOURCES := \
	kernel/boot_memory.c \
	kernel/dtb.c \
	tests/riscv/stack_guard_main.c
STACK_GUARD_TEST_OBJECTS := \
	$(TEST_RUNTIME_OBJECTS) \
	$(patsubst %.c,$(BUILD_DIR)/%.o,$(STACK_GUARD_TEST_C_SOURCES))
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
	tests/riscv/pin_observation.c \
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
	$(STACK_GUARD_TEST_OBJECTS:.o=.d) \
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
	test-sv39-fault-riscv test-sv39-riscv test-stack-guard-riscv \
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

.PHONY: test-lwext4-extent-host
test-lwext4-extent-host:
	sh tests/lwext4-metadata-host.sh extent

.PHONY: test-lwext4-cost-host
test-lwext4-cost-host:
	sh tests/lwext4-cost-host.sh

.PHONY: test-lwext4-checksum-host
test-lwext4-checksum-host:
	@mkdir -p build/host
	cc -std=gnu11 -O2 -Wall -Wextra -Werror -DCONFIG_USE_DEFAULT_CFG=1 -Ithird_party/lwext4/include tests/host/lwext4_checksum.c third_party/lwext4/src/ext4_crc32.c -o build/host/lwext4-checksum
	build/host/lwext4-checksum
	python3 -B tests/lwext4-checksum-read-host.py

.PHONY: test-lwext4-crc-layout-riscv
test-lwext4-crc-layout-riscv: $(KERNEL_RV)
	python3 -B tests/lwext4-crc-layout.py --nm $(NM) $(KERNEL_RV)

.PHONY: test-string-host
test-string-host:
	sh tests/string-host.sh

.PHONY: test-lwext4-cache-host
.PHONY: test-lwext4-deep-truncate-host
test-lwext4-deep-truncate-host:
	sh tests/lwext4-deep-truncate-host.sh

.PHONY: test-lwext4-batch-read-host
test-lwext4-batch-read-host:
	sh tests/lwext4-batch-read-host.sh

.PHONY: test-lwext4-exclusion-host
test-lwext4-exclusion-host:
	sh tests/lwext4-exclusion-host.sh

.PHONY: test-lwext4-payload-host test-lwext4-memory-owner-host
test-lwext4-payload-host:
	sh tests/lwext4-payload-host.sh

test-lwext4-memory-owner-host:
	sh tests/lwext4-memory-owner-host.sh

test-lwext4-cache-host: test-lwext4-exclusion-host test-lwext4-payload-host test-lwext4-memory-owner-host
	sh tests/lwext4-cache-host.sh

.PHONY: test-lwext4-group-host
test-lwext4-group-host:
	sh tests/lwext4-group-host.sh

.PHONY: test-lwext4-operation-host test-lwext4-port-host test-lwext4-parallel-host test-ext4-backend-parallel-host
test-lwext4-operation-host:
	sh tests/lwext4-operation-host.sh

test-lwext4-port-host:
	sh tests/lwext4-port-host.sh

test-lwext4-parallel-host: test-lwext4-operation-host test-lwext4-port-host
	sh tests/lwext4-parallel-host.sh

test-ext4-backend-parallel-host:
	sh tests/ext4-backend-parallel-host.sh
	sh tests/namespace-parallel-host.sh
	sh tests/ext4-namespace-parallel-host.sh

JOURNAL_GROUP_KERNEL := $(BUILD_DIR)/tests/kernel-journal-group-rv
JOURNAL_GROUP_RUNTIME_OBJECTS := $(filter-out $(BUILD_DIR)/fs/ext4_backend.o,$(TEST_RUNTIME_OBJECTS)) $(BUILD_DIR)/tests/riscv/journal_group_backend.o
$(BUILD_DIR)/tests/riscv/journal_group_main.o: CPPFLAGS += -Ifs/lwext4_config -Ithird_party/lwext4/include -DCONFIG_USE_DEFAULT_CFG=0
$(BUILD_DIR)/tests/riscv/journal_group_backend.o: CPPFLAGS += -Ifs/lwext4_config -Ithird_party/lwext4/include -DCONFIG_USE_DEFAULT_CFG=0 -Dkernel_time_monotonic_ns=journal_group_time_monotonic_ns
$(BUILD_DIR)/tests/riscv/journal_group_backend.o: fs/ext4_backend.c $(BUILD_DIR)/generated/cost-config.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@
-include $(BUILD_DIR)/tests/riscv/journal_group_main.d
-include $(BUILD_DIR)/tests/riscv/journal_group_backend.d
$(JOURNAL_GROUP_KERNEL): $(JOURNAL_GROUP_RUNTIME_OBJECTS) $(BUILD_DIR)/kernel/dtb.o $(BUILD_DIR)/tests/riscv/journal_group_main.o arch/riscv/linker.ld
	$(CC) $(LDFLAGS) -Wl,--wrap=kernel_wait_prepare -o $@ $(JOURNAL_GROUP_RUNTIME_OBJECTS) $(BUILD_DIR)/kernel/dtb.o $(BUILD_DIR)/tests/riscv/journal_group_main.o
.PHONY: test-journal-group-riscv
test-journal-group-riscv: $(JOURNAL_GROUP_KERNEL)
	python3 -B tests/journal-group-riscv.py --kernel $< --qemu $(QEMU_RISCV64)
	python3 -B tests/journal-group-riscv.py --kernel $< --qemu $(QEMU_RISCV64) --nojournal

# Real volatile-storage power cuts, separate from normal QEMU shutdown tests.
test-lwext4-recovery-host:
	sh tests/lwext4-journal-host.sh
	sh tests/lwext4-ordered-host.sh
	sh tests/lwext4-orphan-host.sh
	sh tests/lwext4-truncate-host.sh
	sh tests/lwext4-recovery-host.sh
	sh tests/lwext4-reclaim-host.sh

$(KERNEL_RV): $(OBJECTS) arch/riscv/linker.ld $(BUILD_DIR)/generated/link-config
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
		-Wl,--wrap=ext4_journal_start -Wl,--wrap=ext4_user_calloc -Wl,--wrap=ext4_mode_set -Wl,--wrap=kernel_rwlock_write \
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

$(STACK_GUARD_TEST_KERNEL_RV): $(STACK_GUARD_TEST_OBJECTS) \
		arch/riscv/linker.ld
	$(CC) $(LDFLAGS) \
		-Wl,-Map,$(BUILD_DIR)/tests/kernel-stack-guard-rv.map \
		-o $@ $(STACK_GUARD_TEST_OBJECTS)

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
		-Wl,--wrap=kernel_mm_pin_user_page -Wl,-Map,$(BUILD_DIR)/tests/kernel-uaccess-rv.map \
		-o $@ $(UACCESS_TEST_OBJECTS)

$(FILES_TEST_KERNEL_RV): $(FILES_TEST_OBJECTS) arch/riscv/linker.ld
	$(CC) $(LDFLAGS) \
		-Wl,--wrap=kernel_open_file_prepare_mapping_page \
		-Wl,--wrap=kernel_open_file_release \
		-Wl,--wrap=kernel_heap_allocate \
		-Wl,--wrap=kernel_heap_allocate_zeroed \
		-Wl,--wrap=physical_page_allocate \
		-Wl,--wrap=virt_uart_rx_ready \
		-Wl,--wrap=virt_uart_getc \
		-Wl,--wrap=riscv_sv39_current_satp \
		-Wl,-Map,$(BUILD_DIR)/tests/kernel-files-rv.map \
		-o $@ $(FILES_TEST_OBJECTS)

$(FILES_PARTIAL_WRITE_TEST_KERNEL_RV): \
		$(FILES_PARTIAL_WRITE_TEST_OBJECTS) arch/riscv/linker.ld
	$(CC) $(LDFLAGS) \
		-Wl,--wrap=kernel_open_file_prepare_mapping_page \
		-Wl,--wrap=kernel_open_file_release \
		-Wl,--wrap=kernel_heap_allocate_zeroed \
		-Wl,--wrap=physical_page_allocate \
		-Wl,--wrap=riscv_sv39_current_satp \
		-Wl,--wrap=ext4_fpwrite_batch \
		-Wl,--wrap=ext4_file_sync_metadata_prepare \
		-Wl,--wrap=kernel_lock_release \
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
	-Wl,--wrap=kernel_files_pin_data \
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

$(BUILD_DIR)/fs/lwext4_port.o $(BUILD_DIR)/fs/ext4_backend.o $(BUILD_DIR)/fs/page_cache.o: \
	CPPFLAGS += $(LWEXT4_CPPFLAGS)

$(BUILD_DIR)/third_party/lwip/src/core/%.o \
$(BUILD_DIR)/net/lwip_port/%.o \
$(BUILD_DIR)/net/socket.o \
$(BUILD_DIR)/net/ethernet.o \
$(BUILD_DIR)/third_party/lwip/src/netif/ethernet.o: CPPFLAGS += $(LWIP_CPPFLAGS)

$(BUILD_DIR)/third_party/lwext4/src/%.o: \
		third_party/lwext4/src/%.c $(BUILD_DIR)/generated/cost-config.h
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
	test-sv39-riscv test-sv39-fault-riscv test-stack-guard-riscv \
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
		--qemu $(QEMU_RISCV64) $(TEST_MEMORY_ARGS)

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
	SQLITE_SUPERVISOR_CC=$(CC) QEMU_RISCV64=$(QEMU_RISCV64) \
		./tests/sqlite-rollback-riscv.sh $(TEST_MEMORY_ARGS)
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
	OBJDUMP_RV=$(OBJDUMP) MM_OBJECT_RV=$(abspath $(BUILD_DIR)/mm/mm.o) \
		ELF_IMAGE_OBJECT_RV=$(abspath $(BUILD_DIR)/kernel/elf_image.o) \
		SV39_OBJECT_RV=$(abspath $(BUILD_DIR)/arch/riscv/sv39.o) ./tests/icache-riscv.sh

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

test-stack-guard-riscv: $(STACK_GUARD_TEST_KERNEL_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) STACK_GUARD_TEST_KERNEL_RV=$< \
		./tests/stack-guard-riscv.sh

.PHONY: test-riscv-timer-host
test-riscv-timer-host:
	python3 -B tests/host/riscv_timer_events.py

test-timer-riscv: $(TIMER_CASES_TEST_KERNEL_RV) \
		$(TIMER_BOOT_TEST_KERNEL_RV) test-riscv-timer-host
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
	python3 tests/prune-build.py --apply $(foreach path,$(PRUNE_BUILD_KEEP),--keep $(path))

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
	python3 -B tests/host/network_owner.py --case all

HOST_CPU_SOURCES := kernel/cpu.c kernel/raw_lock.c tests/host/cpu_context.c

test-allocator-release-host:
	mkdir -p build/host
	cc -std=c11 -Wall -Wextra -Werror -DBOAROS_PAGE_SHIFT=12 -Itests/host/random -Iinclude tests/host/allocator_release.c kernel/physical_page.c $(HOST_CPU_SOURCES) mm/heap.c -o build/host/allocator-release
	build/host/allocator-release

.PHONY: test-allocator-cost-host
test-allocator-cost-host:
	@mkdir -p build/cost/host
	@for shift in 12 14; do \
		cc -std=c11 -O2 -Wall -Wextra -Werror -Itests/host/random -idirafter include \
			-DBOAROS_PAGE_SHIFT=$$shift -DBOAROS_COST_DIAGNOSTICS=1 \
			tests/cost/allocator_work.c kernel/cost.c kernel/physical_page.c $(HOST_CPU_SOURCES) \
			-o build/cost/host/allocator-work-$$shift && \
		build/cost/host/allocator-work-$$shift || exit; \
	done

include tests/diff-abi/Makefile.inc

.PHONY: test-fat32-host
test-fat32-host:
	sh tests/fat32-host.sh

FAT32_USER_RV := $(BUILD_DIR)/tests/user/fat32-rv
$(FAT32_USER_RV): tests/userland/fat32.c $(MUSL_STAMP)
	@mkdir -p $(dir $@)
	$(MUSL_ROOT)/bin/musl-gcc $(MUSL_GCC_FLAGS) -static -O2 -Wall -Wextra -Werror -o $@ $<
.PHONY: test-fat32-user-riscv
test-fat32-user-riscv: $(KERNEL_RV) $(FAT32_USER_RV)
	python3 -B tests/fat32-user.py --arch riscv --kernel $(KERNEL_RV) \
		--program $(FAT32_USER_RV) --qemu $(QEMU_RISCV64)

.PHONY: test-block-host
test-block-host:
	mkdir -p build/host
	cc -std=c11 -Wall -Wextra -Werror -idirafter include tests/host/block_flush.c kernel/block.c tests/host/block_runtime.c -pthread -Itests/host/block_model -o build/host/block-flush
	build/host/block-flush
	cc -std=c11 -Wall -Wextra -Werror -idirafter include tests/host/block_registry.c kernel/block.c tests/host/block_runtime.c -pthread -Itests/host/block_model -o build/host/block-registry
	build/host/block-registry
	cc -std=c11 -Wall -Wextra -Werror -idirafter include tests/host/block_partitions.c kernel/block.c tests/host/block_runtime.c -pthread -Itests/host/block_model -o build/host/block-partitions
	build/host/block-partitions
	cc -std=c11 -Wall -Wextra -Werror -DBOAROS_MAX_CPUS=2 -idirafter include tests/host/block_partitions.c kernel/block.c tests/host/block_runtime.c tests/host/sync/notifications.c -pthread -Itests/host/block_model -o build/host/block-partitions-smp
	build/host/block-partitions-smp
	cc -std=c11 -Wall -Wextra -Werror -idirafter include tests/host/block_fault_test.c tests/host/block_fault.c kernel/block.c tests/host/block_runtime.c -pthread -Itests/host/block_model -o build/host/block-fault
	build/host/block-fault

.PHONY: test-record-lock-host
test-record-lock-host:
	mkdir -p build/host
	cc -std=c11 -Wall -Wextra -Werror -Itests/host/sync -idirafter include tests/host/record_lock_test.c -o build/host/record-lock
	build/host/record-lock

build/host/nbd-fault: tests/host/nbd_fault.c tests/host/block_fault.c tests/host/block_fault.h kernel/block.c tests/host/block_runtime.c tests/host/block_model/arch/context.h
	@mkdir -p $(dir $@)
	cc -std=c11 -Wall -Wextra -Werror -idirafter include tests/host/nbd_fault.c tests/host/block_fault.c kernel/block.c tests/host/block_runtime.c -pthread -Itests/host/block_model -o $@

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
		-Wl,--wrap=kernel_vfs_node_pread,--wrap=kernel_vfs_node_pread_batch -Wl,--wrap=kernel_vfs_node_writeback \
		-Wl,--wrap=kernel_wait_queue_wake_all \
		-Wl,--wrap=netif_poll_all -Wl,--wrap=sys_check_timeouts -Wl,--wrap=sys_now \
		-Wl,--wrap=netif_poll_budget -Wl,--wrap=sys_check_timeouts_budget \
		-o $@ $(SCALE_OBJECTS)
.PHONY: test-scale-riscv
test-scale-riscv: $(BUILD_DIR)/tests/kernel-scale-rv
	python3 tests/scale-riscv.py --kernel $< --qemu $(QEMU_RISCV64)

IO_SLEEP_OBJECTS := $(TEST_RUNTIME_OBJECTS) $(BUILD_DIR)/kernel/dtb.o $(BUILD_DIR)/tests/riscv/io_sleep_main.o
-include $(BUILD_DIR)/tests/riscv/io_sleep_main.d
$(BUILD_DIR)/tests/riscv/io_sleep_main.o: CPPFLAGS += $(LWEXT4_CPPFLAGS)
$(BUILD_DIR)/tests/kernel-io-sleep-rv: $(IO_SLEEP_OBJECTS) arch/riscv/linker.ld
	$(CC) $(LDFLAGS) -Wl,--wrap=kernel_vfs_node_pread_batch -Wl,--wrap=kernel_vfs_node_writeback,--wrap=ext4_fpwrite_batch -Wl,--wrap=kernel_open_file_sync_range -Wl,--wrap=kernel_open_file_release -Wl,--wrap=kernel_copy_from_user -Wl,--wrap=kernel_uaccess_write_cursor_copy -Wl,--wrap=kernel_uaccess_pin_user_page -o $@ $(IO_SLEEP_OBJECTS)
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
	@set -e; for shift in 12 14; do \
	cc -std=c11 -O2 -Wall -Wextra -Werror -DBOAROS_PAGE_SHIFT=$$shift \
		-DBOAROS_COST_DIAGNOSTICS=0 -Itests/host/random -idirafter include \
		tests/host/pid_allocation.c kernel/pid.c kernel/physical_page.c $(HOST_CPU_SOURCES) mm/heap.c \
		-o build/host/pid-allocation-$$shift; \
		build/host/pid-allocation-$$shift; \
	done

test-random-host:
	PYTHONDONTWRITEBYTECODE=1 python3 tests/host/random_vectors.py

RNG_USER_RV := $(BUILD_DIR)/tests/user/rng-rv
$(RNG_USER_RV): tests/userland/rng.c $(MUSL_STAMP)
	@mkdir -p $(@D)
	$(MUSL_ROOT)/bin/musl-gcc $(MUSL_GCC_FLAGS) -static -O2 -Wall -Wextra -Werror -o $@ $<

.PHONY: test-rng-riscv test-virtio-rng-host
test-rng-riscv: $(KERNEL_RV) $(RNG_USER_RV)
	python3 -B tests/rng-riscv.py --kernel $(KERNEL_RV) --program $(RNG_USER_RV) --qemu $(QEMU_RISCV64) $(TEST_MEMORY_ARGS)

test-virtio-rng-host:
	@mkdir -p build/host
	cc -std=c11 -Wall -Wextra -Werror -DBOAROS_PAGE_SHIFT=12 -Itests/host/random -Iinclude \
		tests/host/virtio_rng_test.c arch/riscv/virtio_mmio_rng.c drivers/virtio/rng.c drivers/virtio/mmio.c drivers/virtio/transport.c drivers/virtio/split_queue.c -o build/host/virtio-rng
	build/host/virtio-rng
	build/host/virtio-rng --generic
	cc -std=c11 -Wall -Wextra -Werror -DBOAROS_PAGE_SHIFT=14 -Itests/host/random -Iinclude \
		tests/host/virtio_rng_test.c arch/riscv/virtio_mmio_rng.c drivers/virtio/rng.c drivers/virtio/mmio.c drivers/virtio/transport.c drivers/virtio/split_queue.c -o build/host/virtio-rng-16k
	build/host/virtio-rng-16k --generic

.PHONY: test-sched-policy-host
test-sched-policy-host:
	@mkdir -p build/host
	cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -Iinclude \
		tests/host/sched_policy_test.c kernel/sched/policy.c -o build/host/sched-policy
	build/host/sched-policy
	cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -Iinclude \
		tests/host/sched_runqueue_test.c kernel/sched/runqueue.c -o build/host/sched-runqueue
	build/host/sched-runqueue
	cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -ffunction-sections -fdata-sections \
		-DBOAROS_PAGE_SHIFT=12 -Itests/host/mm -Itests/host/sync -Iinclude \
		-Wl,--gc-sections -Wl,--wrap=kernel_task_sched_get -Wl,--wrap=kernel_task_sched_set \
		tests/host/sched_syscall_test.c kernel/syscall/sched.c kernel/sched/policy.c \
		kernel/sched/scheduling.c kernel/sched/accounting.c kernel/cpu.c kernel/raw_lock.c -o build/host/sched-syscall
	build/host/sched-syscall

.PHONY: test-multi-disk-rt-riscv
test-multi-disk-rt-riscv: $(KERNEL_RV) $(MULTI_DISK_IO_RV) build/host/nbd-fault
	PYTHONDONTWRITEBYTECODE=1 python3 tests/multi-disk-io-riscv.py \
		--kernel $(KERNEL_RV) --program $(MULTI_DISK_IO_RV) --qemu $(QEMU_RISCV64) --rt-load

.PHONY: test-sched-bandwidth-riscv
test-sched-bandwidth-riscv: $(KERNEL_RV) $(MUSL_STAMP)
	KERNEL_RV=$(KERNEL_RV) QEMU_RISCV64=$(QEMU_RISCV64) sh tests/sched-bandwidth-riscv.sh

# Cost variants never reuse objects compiled with a different observation flag.
.PHONY: force-cost-config test-cost-riscv test-cost-host test-cost-match-host test-cost-match-riscv test-cost-diagnostic-host test-cost-diagnostic-plugin-host
force-cost-config:
# Shared prerequisite content must not depend on whichever target reaches it first.
build-quote = '$(subst ','"'"',$(1))'
BUILD_CONFIG_CC := $(CC)
BUILD_CONFIG_FLAGS := $(CPPFLAGS) $(CFLAGS) $(ASFLAGS) $(LWEXT4_CPPFLAGS) $(LWIP_CPPFLAGS)
BUILD_CONFIG_LINK := $(LDFLAGS) $(OBJECTS)
$(BUILD_DIR)/generated/cost-config.h: force-cost-config Makefile
	@mkdir -p $(dir $@)
	@{ printf '#define BOAROS_COST_DIAGNOSTICS %s\n#define BOAROS_LOCK_DEBUG %s\n/* compile inputs\n' '$(COST_DIAGNOSTICS)' '$(LOCK_DEBUG)'; \
	  printf '%s\n' $(call build-quote,$(BUILD_CONFIG_CC)) $(call build-quote,$(BUILD_CONFIG_FLAGS)); \
	  $(BUILD_CONFIG_CC) --version; sha256sum Makefile; printf '*/\n'; } > $@.tmp
	@cmp -s $@ $@.tmp && rm $@.tmp || mv $@.tmp $@
$(BUILD_DIR)/generated/link-config: force-cost-config Makefile
	@mkdir -p $(dir $@)
	@printf '%s\n' $(call build-quote,$(BUILD_CONFIG_CC)) $(call build-quote,$(BUILD_CONFIG_LINK)) > $@.tmp
	@cmp -s $@ $@.tmp && rm $@.tmp || mv $@.tmp $@
COST_CASE ?= all
test-cost-match-host:
	python3 -B tests/test-cost-match-report.py
	python3 -B tests/test-cost-policy.py
	python3 -B tests/test-cost-policy-guard.py
	python3 -B tests/test-cost-measurement.py
	python3 -B tests/test-cost-match.py
test-cost-diagnostic-host:
	python3 -B tests/test-cost-diagnostic.py
# The optional observer requires QEMU API headers and GLib development files;
# ordinary host COST/CI gates keep their existing dependency footprint.
test-cost-diagnostic-plugin-host:
	@mkdir -p build/cost/diagnostic
	cc -std=c11 -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections -Wall -Wextra -Werror $$(pkg-config --cflags glib-2.0) tests/cost/diagnostic_plugin_test.c $$(pkg-config --libs glib-2.0) -o build/cost/diagnostic/collector-test
	build/cost/diagnostic/collector-test
COST_MATCH_OUTPUT ?= build/cost-match-riscv
test-cost-match-riscv: test-cost-match-host
	python3 -B tests/cost-match-riscv.py --output $(COST_MATCH_OUTPUT)
# Force only this fixture's aggregate atomics through their ordinary ABI so
# the test can handshake at the actual locked poll, without production hooks.
.PHONY: test-cost-boundary-host
test-cost-boundary-host:
	@mkdir -p build/cost/host
	cc -std=c11 -O2 -Wall -Wextra -Werror -fno-inline-atomics -idirafter include -DBOAROS_MAX_CPUS=2 -DBOAROS_USER_SMP=1 -DBOAROS_COST_DIAGNOSTICS=1 -c kernel/cost.c -o build/cost/host/boundary-cost.o
	cc -std=c11 -O2 -Wall -Wextra -Werror -pthread -idirafter include -DBOAROS_MAX_CPUS=2 -DBOAROS_USER_SMP=1 -DBOAROS_COST_DIAGNOSTICS=1 tests/cost/irq_boundary_test.c build/cost/host/boundary-cost.o -latomic -Wl,--wrap=__atomic_load_4,--wrap=__atomic_exchange_4,--wrap=__atomic_load_8,--wrap=__atomic_exchange_8 -o build/cost/host/irq-boundary-test
	for scenario in 0 1 2 3; do timeout 10s build/cost/host/irq-boundary-test $$scenario || exit $$?; done
	cc -std=gnu11 -O2 -g -ffunction-sections -fdata-sections -Wall -Wextra -Werror -pthread -DBOAROS_MAX_CPUS=2 -DBOAROS_PAGE_SHIFT=12 -DBOAROS_COST_DIAGNOSTICS=1 -DBOAROS_USER_SMP=1 -DBOAROS_TEST_IRQ_RESTORE=1 -Itests/host/sync -Ikernel/sched -idirafter include tests/cost/lock_boundary_test.c kernel/cost.c kernel/cpu.c kernel/raw_lock.c tests/host/sync/notifications.c kernel/sched/park.c kernel/sched/sync.c kernel/time.c -Wl,--gc-sections,--wrap=kernel_raw_lock_release,--wrap=kernel_raw_lock_acquire,--wrap=kernel_cost_add -o build/cost/host/lock-boundary-test
	for scenario in 1 2 3 4 5; do timeout 10s build/cost/host/lock-boundary-test $$scenario || exit $$?; done

test-cost-host: test-cost-match-host test-cost-diagnostic-host test-cost-boundary-host
	@mkdir -p build/cost/host
	cc -std=c11 -Wall -Wextra -Werror -idirafter include -DBOAROS_COST_DIAGNOSTICS=1 tests/cost/core_test.c kernel/cost.c -o build/cost/host/core-test
	build/cost/host/core-test
	cc -std=c11 -Wall -Wextra -Werror -idirafter include -DBOAROS_COST_DIAGNOSTICS=1 tests/cost/account_test.c kernel/cost.c -o build/cost/host/account-test
	build/cost/host/account-test
	cc -std=c11 -Wall -Wextra -Werror -idirafter include -DBOAROS_COST_DIAGNOSTICS=1 tests/cost/irq_test.c kernel/cost.c -o build/cost/host/irq-test
	build/cost/host/irq-test
	cc -std=c11 -O2 -Wall -Wextra -Werror -pthread -idirafter include -DBOAROS_MAX_CPUS=2 -DBOAROS_USER_SMP=1 -DBOAROS_COST_DIAGNOSTICS=1 tests/cost/smp_test.c kernel/cost.c -o build/cost/host/smp-test
	build/cost/host/smp-test
	cc -std=c11 -O2 -Wall -Wextra -Werror -Itests/host/sync -idirafter include -DBOAROS_COST_DIAGNOSTICS=1 tests/cost/raw_test.c kernel/cost.c kernel/cpu.c kernel/raw_lock.c -o build/cost/host/raw-test
	build/cost/host/raw-test
	cc -std=c11 -Wall -Wextra -Werror -Itests/host/random -idirafter include -DBOAROS_PAGE_SHIFT=12 -DBOAROS_COST_DIAGNOSTICS=1 tests/cost/page_test.c kernel/cost.c kernel/physical_page.c $(HOST_CPU_SOURCES) -o build/cost/host/page-test
	build/cost/host/page-test
	cc -std=c11 -Wall -Wextra -Werror -Itests/host/random -idirafter include -DBOAROS_PAGE_SHIFT=12 -DBOAROS_COST_DIAGNOSTICS=1 tests/memory/cost_test.c kernel/cost.c kernel/physical_page.c $(HOST_CPU_SOURCES) mm/heap.c mm/uaccess.c -o build/cost/host/memory-test
	build/cost/host/memory-test
	python3 -B tests/test-cost-report.py
	python3 -B tests/test-cost-qmp.py
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
	@if [ -n '$(TEST_MEMORIES)' ]; then \
		for memory in $(TEST_MEMORIES); do \
			KERNEL_RV=$(KERNEL_RV) QEMU_RISCV64=$(QEMU_RISCV64) QEMU_MEMORY=$$memory sh tests/environment-riscv.sh || exit; \
		done; \
	else \
		KERNEL_RV=$(KERNEL_RV) QEMU_RISCV64=$(QEMU_RISCV64) sh tests/environment-riscv.sh; \
	fi

.PHONY: test-platform-profile-host
test-platform-profile-host:
	python3 -B tests/host/qemu_cache.py
	python3 -B tests/host/platform_memory.py

.PHONY: test-rtc-host
test-rtc-host:
	@mkdir -p build/host
	cc -std=c11 -Wall -Wextra -Werror -DBOAROS_PAGE_SHIFT=12 -Itests/host/random -idirafter include tests/host/rtc_device.c fs/rtc_device.c -o build/host/rtc-test
	build/host/rtc-test

.PHONY: test-network-riscv
test-network-riscv: kernel-rv
	python3 -B tests/network-riscv.py $(TEST_MEMORY_ARGS)

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
	cc -std=c11 -Wall -Wextra -Werror -DBOAROS_PAGE_SHIFT=12 -Itests/host/allocator -Iinclude $(HOST_CPU_SOURCES) tests/host/allocator_preemption.c build/host/allocator/page.o build/host/allocator/heap.o -o build/host/allocator/preemption
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
	cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -DBOAROS_PAGE_SHIFT=12 -Itests/host/random -idirafter include -fsanitize=address,undefined tests/tty/endpoints_host.c fs/tty.c -o build/host/tty/endpoints
	build/host/tty/endpoints
	cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -DBOAROS_PAGE_SHIFT=12 -Itests/host/random -idirafter include -fsanitize=address,undefined tests/tty/flags_host.c fs/tty.c -o build/host/tty/flags
	build/host/tty/flags
	cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -DBOAROS_PAGE_SHIFT=12 -Itests/host/sync -idirafter include -ffunction-sections -fdata-sections -Wl,--gc-sections -fsanitize=address,undefined tests/tty/group_host.c kernel/sched/tty.c kernel/pid.c kernel/cpu.c kernel/raw_lock.c -o build/host/tty/group
	build/host/tty/group

TTY_PROBE_RV := $(BUILD_DIR)/tests/user/tty-probe-rv
TTY_JOBCTRL_RV := $(BUILD_DIR)/tests/user/tty-jobctrl-rv
TTY_TERMIOS2_RV := $(BUILD_DIR)/tests/user/tty-termios2-probe-rv
$(TTY_PROBE_RV): tests/tty/probe.c $(MUSL_STAMP)
	@mkdir -p $(dir $@)
	$(MUSL_ROOT)/bin/musl-gcc $(MUSL_GCC_FLAGS) -static -pthread -O2 -Wall -Wextra -Werror $< -o $@
$(TTY_JOBCTRL_RV): tests/tty/jobctrl_probe.c $(MUSL_STAMP)
	@mkdir -p $(dir $@)
	$(MUSL_ROOT)/bin/musl-gcc $(MUSL_GCC_FLAGS) -static -O2 -Wall -Wextra -Werror $< -o $@
$(TTY_TERMIOS2_RV): tests/tty/termios2_probe.c $(MUSL_STAMP)
	@mkdir -p $(dir $@)
	$(MUSL_ROOT)/bin/musl-gcc $(MUSL_GCC_FLAGS) -static -O2 -Wall -Wextra -Werror $< -o $@

.PHONY: test-tty-riscv test-tty-diff-riscv
test-tty-riscv: $(KERNEL_RV) $(MUSL_STAMP)
	python3 -B tests/tty/riscv.py --kernel $(KERNEL_RV) --qemu $(QEMU_RISCV64) $(TEST_MEMORY_ARGS)
test-tty-diff-riscv: $(KERNEL_RV) $(TTY_PROBE_RV) $(TTY_JOBCTRL_RV)
	python3 -B tests/tty/riscv.py --kernel $(KERNEL_RV) --qemu $(QEMU_RISCV64) --probe $(TTY_PROBE_RV) $(TEST_MEMORY_ARGS)
	python3 -B tests/tty/riscv.py --kernel $(KERNEL_RV) --qemu $(QEMU_RISCV64) --probe $(TTY_JOBCTRL_RV) --no-ctty $(TEST_MEMORY_ARGS)

.PHONY: test-pty-host
test-pty-host:
	@mkdir -p build/host/tty
	cc -std=c11 -O1 -g -Wall -Wextra -Werror -DBOAROS_PAGE_SHIFT=12 -idirafter include -fsanitize=address,undefined tests/host/devpts_test.c -o build/host/tty/devpts
	build/host/tty/devpts
	cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -DBOAROS_PAGE_SHIFT=12 -Itests/host/random -idirafter include -fsanitize=address,undefined tests/pty/transport_host.c fs/tty.c fs/pty.c -o build/host/tty/pty-transport
	build/host/tty/pty-transport

.PHONY: test-tty-termios2-riscv
test-tty-termios2-riscv: $(KERNEL_RV) $(TTY_TERMIOS2_RV)
	python3 -B tests/tty/riscv.py --kernel $(KERNEL_RV) --qemu $(QEMU_RISCV64) --probe $(TTY_TERMIOS2_RV) $(TEST_MEMORY_ARGS)

.PHONY: test-pty-riscv test-pty-apps-riscv
test-pty-riscv: $(KERNEL_RV) $(MUSL_STAMP)
	python3 -B tests/tty/pty_riscv.py --case core --kernel $(KERNEL_RV) --qemu $(QEMU_RISCV64) $(TEST_MEMORY_ARGS)
test-pty-apps-riscv: $(KERNEL_RV) $(MUSL_STAMP)
	python3 -B tests/tty/pty_riscv.py --case libc --kernel $(KERNEL_RV) --qemu $(QEMU_RISCV64) $(TEST_MEMORY_ARGS)
	python3 -B tests/tty/pty_riscv.py --case script --kernel $(KERNEL_RV) --qemu $(QEMU_RISCV64) $(TEST_MEMORY_ARGS)

.PHONY: test-poll-host
test-poll-host:
	@mkdir -p build/host
	cc -std=c11 -O1 -g -Wall -Wextra -Werror -DBOAROS_PAGE_SHIFT=12 \
		-DBOAROS_MAX_CPUS=2 -DBOAROS_USER_SMP=1 \
		-ffunction-sections -fdata-sections -Wl,--gc-sections -idirafter include \
		-fsanitize=address,undefined tests/host/poll_snapshot.c -o build/host/poll-snapshot
	build/host/poll-snapshot

.PHONY: test-epoll-host test-epoll-riscv
test-epoll-host:
	@mkdir -p build/host
	cc -std=c11 -O1 -g -Wall -Wextra -Werror -DBOAROS_HOST_IRQ_STATE=1 -DBOAROS_PAGE_SHIFT=12 \
		-DBOAROS_MAX_CPUS=2 -DBOAROS_USER_SMP=1 \
		-ffunction-sections -fdata-sections -Wl,--gc-sections \
		-Itests/host/random -idirafter include -fsanitize=address,undefined \
		tests/host/epoll_delivery.c kernel/raw_lock.c tests/host/cpu_context.c -Wl,--wrap=kernel_raw_lock_acquire -o build/host/epoll-delivery
	build/host/epoll-delivery
test-epoll-riscv: $(KERNEL_RV) $(MUSL_STAMP)
	python3 -B tests/epoll-riscv.py --kernel $(KERNEL_RV) --qemu $(QEMU_RISCV64)

.PHONY: test-cache-growth-riscv
test-cache-growth-riscv:
	$(MAKE) COST_DIAGNOSTICS=1 test-scale-riscv

$(BUILD_DIR)/tests/riscv/scale_main.o: CPPFLAGS += $(LWIP_CPPFLAGS)

.PHONY: test-writeback-batch-riscv
test-writeback-batch-riscv:
	python3 -B tests/writeback-batch-riscv.py

.PHONY: test-readahead-riscv
test-readahead-riscv:
	python3 -B tests/readahead-riscv.py

.PHONY: test-demand-read-riscv
test-demand-read-riscv:
	python3 -B tests/demand-read-riscv.py --qemu $(QEMU_RISCV64)

include arch/loongarch/build.mk

.PHONY: test-build-config-host test-elf-layout-host
test-build-config-host:
	python3 -B tests/host/build_config.py
test-elf-layout-host:
	sh tests/elf-layout-host.sh

.PHONY: test-ci-host
test-ci-host: test-build-config-host
	python3 -B -m unittest discover -s tests/ci -p 'test_*.py'

.PHONY: test-virtio-block-host
.PHONY: test-virtio-framework-host
test-virtio-framework-host:
	@mkdir -p build/host
	cc -std=c11 -Wall -Wextra -Werror -Itests/host/random -Iinclude tests/host/virtio_framework.c drivers/virtio/transport.c drivers/virtio/split_queue.c -o build/host/virtio-framework
	build/host/virtio-framework
	cc -std=c11 -Wall -Wextra -Werror -Itests/host/random -Iinclude tests/host/virtio_adapters.c drivers/virtio/transport.c drivers/virtio/mmio.c drivers/virtio/pci.c kernel/pci.c -o build/host/virtio-adapters
	build/host/virtio-adapters
test-virtio-block-host:
	python3 -B tests/host/virtio_block_diagnostics.py
	HOST_ALTERNATE_TRANSPORT=1 python3 -B tests/host/virtio_block_diagnostics.py

.PHONY: test-lwext4-dir-empty-host
test-lwext4-dir-empty-host:
	python3 -B tests/lwext4-dir-empty-host.py --sanitize

.PHONY: test-sync-host
test-sync-host:
	@mkdir -p build/host
	@set -e; for cpus in 1 2; do for debug in 0 1; do \
		cc -std=gnu11 -O2 -Wall -Wextra -Werror -pthread -DBOAROS_MAX_CPUS=$$cpus -DBOAROS_LOCK_DEBUG=$$debug \
		-Itests/host/sync -idirafter include tests/host/sync.c kernel/cpu.c kernel/raw_lock.c tests/host/sync/notifications.c -o build/host/sync-$$cpus-$$debug; \
		build/host/sync-$$cpus-$$debug; \
	done; done
	python3 -B tests/sync/test_native.py

.PHONY: test-mm-pin-host test-memory-object-concurrency-host test-mm-shared-concurrency-host test-file-mapping-concurrency-host
test-file-mapping-concurrency-host:
	@mkdir -p build/host/mm
	@set -e; for shift in 12 14; do \
		cc -std=gnu11 -O2 -Wall -Wextra -Werror -pthread -DBOAROS_MAX_CPUS=2 -DBOAROS_LOCK_DEBUG=1 -ffunction-sections -fdata-sections \
		-DBOAROS_PAGE_SHIFT=$$shift -Itests/host/mm -Itests/host/sync -idirafter include \
		tests/host/file_mapping_concurrency.c tests/host/mm/context.c fs/file_mapping.c fs/vfs.c \
		kernel/cpu.c kernel/raw_lock.c tests/host/sync/notifications.c kernel/sched/sync.c kernel/sched/park.c kernel/physical_page.c mm/heap.c \
		-Wl,--gc-sections -o build/host/mm/file-mapping-$$shift; \
		timeout 30s build/host/mm/file-mapping-$$shift || exit $$?; \
		cc -std=gnu11 -O2 -Wall -Wextra -Werror -pthread -DBOAROS_MAX_CPUS=2 -ffunction-sections -fdata-sections \
		-DBOAROS_PAGE_SHIFT=$$shift -Itests/host/mm -Itests/host/sync -idirafter include \
		tests/host/memory_file_source.c tests/host/mm/context.c tests/host/mm/file_unavailable.c \
		fs/vfs.c fs/tmpfs.c fs/open_file.c fs/block_file.c kernel/block.c fs/file_mapping.c fs/record_lock.c \
		kernel/cpu.c kernel/raw_lock.c tests/host/sync/notifications.c kernel/sched/sync.c kernel/sched/park.c \
		kernel/physical_page.c mm/heap.c mm/memory_object.c \
		-Wl,--gc-sections,--wrap=kernel_heap_allocate_zeroed -o build/host/mm/file-source-$$shift; \
		timeout 30s build/host/mm/file-source-$$shift; \
	done
test-memory-object-concurrency-host:
	@mkdir -p build/host/mm
	@set -e; for shift in 12 14; do \
		cc -std=gnu11 -O2 -Wall -Wextra -Werror -pthread -DBOAROS_MAX_CPUS=2 -ffunction-sections -fdata-sections \
		-DBOAROS_PAGE_SHIFT=$$shift -Itests/host/mm -Itests/host/sync -idirafter include \
		tests/host/memory_object_concurrency.c tests/host/mm/context.c \
		kernel/cpu.c kernel/raw_lock.c tests/host/sync/notifications.c kernel/sched/sync.c kernel/sched/park.c \
		kernel/physical_page.c mm/heap.c mm/memory_object.c \
		-Wl,--gc-sections,--wrap=physical_page_acquire,--wrap=physical_page_allocate,--wrap=physical_page_resolve,--wrap=kernel_heap_allocate_zeroed \
		-o build/host/mm/object-$$shift; \
		timeout 30s build/host/mm/object-$$shift; \
	done

test-mm-pin-host:
	@mkdir -p build/host/mm
	cc -std=gnu11 -O2 -Wall -Wextra -Werror -pthread -DBOAROS_MAX_CPUS=2 -ffunction-sections -fdata-sections \
		-DBOAROS_PAGE_SHIFT=12 -Itests/host/mm -Itests/host/sync -idirafter include \
		tests/host/mm_pin.c tests/host/mm/unavailable.c tests/host/mm/context.c \
		kernel/cpu.c kernel/raw_lock.c tests/host/sync/notifications.c kernel/sched/sync.c kernel/sched/park.c \
		kernel/physical_page.c mm/heap.c mm/memory_object.c mm/vma.c mm/mm.c mm/uaccess.c arch/riscv/sv39.c \
		-Wl,--gc-sections,--wrap=kernel_raw_lock_acquire,--wrap=physical_page_resolve -o build/host/mm/pin
	build/host/mm/pin

.PHONY: test-mm-concurrency-host
test-mm-concurrency-host:
	@mkdir -p build/host/mm
	cc -std=gnu11 -O2 -Wall -Wextra -Werror -pthread -DBOAROS_MAX_CPUS=2 -ffunction-sections -fdata-sections \
		-DBOAROS_PAGE_SHIFT=12 -Itests/host/mm -Itests/host/sync -idirafter include \
		tests/host/mm_concurrency.c tests/host/mm/unavailable.c tests/host/mm/context.c \
		kernel/cpu.c kernel/raw_lock.c tests/host/sync/notifications.c kernel/sched/sync.c kernel/sched/park.c \
		kernel/physical_page.c mm/heap.c mm/memory_object.c mm/vma.c mm/mm.c mm/uaccess.c arch/riscv/sv39.c \
		-Wl,--gc-sections,--wrap=physical_page_allocate -o build/host/mm/concurrency
	timeout 30s build/host/mm/concurrency
	cc -std=gnu11 -O2 -Wall -Wextra -Werror -pthread -DBOAROS_MAX_CPUS=2 -ffunction-sections -fdata-sections \
		-DBOAROS_PAGE_SHIFT=12 -Itests/host/mm -Itests/host/sync -idirafter include \
		tests/host/mm_activity.c tests/host/mm/unavailable.c tests/host/mm/context.c \
		kernel/cpu.c kernel/raw_lock.c tests/host/sync/notifications.c kernel/sched/sync.c kernel/sched/park.c \
		kernel/physical_page.c mm/heap.c mm/memory_object.c mm/vma.c mm/mm.c mm/uaccess.c arch/riscv/sv39.c \
		-Wl,--gc-sections -o build/host/mm/activity
	timeout 30s build/host/mm/activity
	cc -std=gnu11 -O2 -Wall -Wextra -Werror -pthread -DBOAROS_MAX_CPUS=4 -ffunction-sections -fdata-sections \
		-DBOAROS_PAGE_SHIFT=12 -Itests/host/mm -Itests/host/sync -idirafter include \
		tests/host/mm_activity.c tests/host/mm/unavailable.c tests/host/mm/context.c \
		kernel/cpu.c kernel/raw_lock.c tests/host/sync/notifications.c kernel/sched/sync.c kernel/sched/park.c \
		kernel/physical_page.c mm/heap.c mm/memory_object.c mm/vma.c mm/mm.c mm/uaccess.c arch/riscv/sv39.c \
		-Wl,--gc-sections -o build/host/mm/activity4
	timeout 30s build/host/mm/activity4
	cc -std=gnu11 -O2 -Wall -Wextra -Werror -pthread -DBOAROS_MAX_CPUS=2 -ffunction-sections -fdata-sections \
		-DBOAROS_PAGE_SHIFT=12 -Itests/host/mm -Itests/host/sync -idirafter include \
		tests/host/mm_zero.c tests/host/mm/unavailable.c tests/host/mm/context.c \
		kernel/cpu.c kernel/raw_lock.c tests/host/sync/notifications.c kernel/sched/sync.c kernel/sched/park.c \
		kernel/physical_page.c arch/riscv/sv39.c -Wl,--gc-sections -o build/host/mm/zero
	timeout 30s build/host/mm/zero

test-mm-shared-concurrency-host:
	@mkdir -p build/host/mm
	cc -std=gnu11 -O2 -Wall -Wextra -Werror -pthread -DBOAROS_MAX_CPUS=2 -ffunction-sections -fdata-sections \
		-DBOAROS_PAGE_SHIFT=12 -Itests/host/mm -Itests/host/sync -idirafter include \
		tests/host/mm_shared.c tests/host/mm/unavailable.c tests/host/mm/context.c \
		kernel/cpu.c kernel/raw_lock.c tests/host/sync/notifications.c kernel/sched/sync.c kernel/sched/park.c \
		kernel/physical_page.c mm/heap.c mm/memory_object.c mm/vma.c mm/mm.c mm/uaccess.c arch/riscv/sv39.c \
		-Wl,--gc-sections,--wrap=kernel_memory_object_prepare_page -o build/host/mm/shared
	timeout 30s build/host/mm/shared


.PHONY: test-mm-file-concurrency-host
.PHONY: test-page-cache-concurrency-host
test-page-cache-concurrency-host:
	sh tests/page-cache-concurrency-host.sh

.PHONY: test-tmpfs-host
test-tmpfs-host:
	sh tests/tmpfs-host.sh

test-mm-file-concurrency-host:
	@mkdir -p build/host/mm
	cc -std=gnu11 -O2 -Wall -Wextra -Werror -pthread -DBOAROS_MAX_CPUS=2 -ffunction-sections -fdata-sections \
		-DBOAROS_PAGE_SHIFT=12 -Itests/host/mm -Itests/host/sync -idirafter include \
		tests/host/mm_file.c tests/host/mm/context.c tests/host/mm/file_unavailable.c \
		fs/vfs.c fs/tmpfs.c fs/open_file.c fs/block_file.c kernel/block.c fs/file_mapping.c fs/record_lock.c \
		kernel/cpu.c kernel/raw_lock.c tests/host/sync/notifications.c kernel/sched/sync.c kernel/sched/park.c \
		kernel/physical_page.c mm/heap.c mm/memory_object.c mm/vma.c mm/mm.c mm/uaccess.c arch/riscv/sv39.c \
		-Wl,--gc-sections,--wrap=kernel_open_file_prepare_mapping_page,--wrap=kernel_memory_object_prepare_page,--wrap=kernel_rwlock_write,--wrap=physical_page_allocate,--wrap=kernel_heap_allocate_zeroed -o build/host/mm/file-concurrency
	timeout 30s build/host/mm/file-concurrency
	cc -std=gnu11 -O2 -Wall -Wextra -Werror -pthread -DBOAROS_MAX_CPUS=2 -ffunction-sections -fdata-sections \
		-DBOAROS_PAGE_SHIFT=12 -Itests/host/mm -Itests/host/sync -idirafter include \
		tests/host/mm_file_single_wait.c tests/host/mm/context.c tests/host/mm/file_unavailable.c \
		fs/vfs.c fs/tmpfs.c fs/open_file.c fs/block_file.c kernel/block.c fs/file_mapping.c fs/record_lock.c \
		kernel/cpu.c kernel/raw_lock.c tests/host/sync/notifications.c kernel/sched/sync.c kernel/sched/park.c \
		kernel/physical_page.c mm/heap.c mm/memory_object.c mm/vma.c mm/mm.c mm/uaccess.c arch/riscv/sv39.c \
		-Wl,--gc-sections,--wrap=kernel_open_file_prepare_mapping_page,--wrap=kernel_rwlock_read,--wrap=kernel_rwlock_try_read -o build/host/mm/file-single-wait
	timeout 30s build/host/mm/file-single-wait
	cc -std=gnu11 -O2 -Wall -Wextra -Werror -pthread -DBOAROS_MAX_CPUS=2 -ffunction-sections -fdata-sections -DBOAROS_PAGE_SHIFT=12 -Itests/host/mm -Itests/host/sync -idirafter include tests/host/mm_source_release.c tests/host/mm/context.c tests/host/mm/file_unavailable.c fs/vfs.c fs/tmpfs.c fs/open_file.c fs/block_file.c kernel/block.c fs/file_mapping.c fs/record_lock.c kernel/cpu.c kernel/raw_lock.c tests/host/sync/notifications.c kernel/sched/sync.c kernel/sched/park.c kernel/physical_page.c mm/heap.c mm/memory_object.c mm/vma.c mm/mm.c mm/uaccess.c arch/riscv/sv39.c -Wl,--gc-sections,--wrap=kernel_open_file_release,--wrap=kernel_vfs_close,--wrap=kernel_file_mapping_drain,--wrap=kernel_heap_release -o build/host/mm/source-release
	timeout 30s build/host/mm/source-release

.PHONY: test-allocator-concurrency-host
test-allocator-concurrency-host:
	@mkdir -p build/host
	@for shift in 12 14; do \
		cc -std=gnu11 -O2 -Wall -Wextra -Werror -pthread -DBOAROS_MAX_CPUS=2 -DBOAROS_PAGE_SHIFT=$$shift \
			-Itests/host/sync -idirafter include tests/host/allocator_concurrency.c \
			kernel/cpu.c kernel/raw_lock.c tests/host/sync/notifications.c kernel/physical_page.c mm/heap.c -o build/host/allocator-concurrency-$$shift && \
		build/host/allocator-concurrency-$$shift || exit; \
	done

$(BUILD_DIR)/sync-%.o: tests/sync/native.c Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -DBOAROS_SYNC_CASE=$* -c $< -o $@
$(BUILD_DIR)/sync-%: $(OBJECTS) $(BUILD_DIR)/sync-%.o arch/riscv/linker.ld
	$(CC) $(LDFLAGS) -Wl,--wrap=kernel_main -o $@ $(OBJECTS) $(BUILD_DIR)/sync-$*.o
.PHONY: test-sync-riscv
test-sync-riscv: $(foreach case,0 1 2 3 4,$(BUILD_DIR)/sync-$(case))
	python3 -B tests/sync/native.py --arch riscv --qemu $(QEMU_RISCV64) --kernel-dir $(BUILD_DIR)
test-riscv: test-sync-riscv

.PHONY: test-wait-host
test-wait-host:
	@mkdir -p build/host
	cc -std=gnu11 -O2 -Wall -Wextra -Werror -pthread -DBOAROS_MAX_CPUS=2 -DBOAROS_PAGE_SHIFT=12 -Itests/host/sync -idirafter include tests/host/wait.c kernel/cpu.c kernel/raw_lock.c tests/host/sync/notifications.c kernel/sched/park.c kernel/sched/runqueue.c -Wl,--wrap=kernel_raw_lock_release -o build/host/wait
	build/host/wait

$(BUILD_DIR)/wait-native.o: tests/wait/native.c Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@
$(BUILD_DIR)/wait-native: $(OBJECTS) $(BUILD_DIR)/wait-native.o arch/riscv/linker.ld
	$(CC) $(LDFLAGS) -Wl,--wrap=kernel_scheduler_init,--wrap=kernel_wait_backend_switch -o $@ $(OBJECTS) $(BUILD_DIR)/wait-native.o
.PHONY: test-wait-riscv
test-wait-riscv: $(BUILD_DIR)/wait-native
	python3 -B tests/wait/native.py --arch riscv --qemu $(QEMU_RISCV64) --kernel-dir $(BUILD_DIR)

$(BUILD_DIR)/sched-lifecycle-native.o: tests/wait/native.c Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -DBOAROS_SCHED_LIFECYCLE=1 -c $< -o $@
$(BUILD_DIR)/sched-lifecycle-native: $(OBJECTS) $(BUILD_DIR)/sched-lifecycle-native.o arch/riscv/linker.ld
	$(CC) $(LDFLAGS) -Wl,--wrap=kernel_scheduler_init,--wrap=kernel_wait_backend_switch -o $@ $(OBJECTS) $(BUILD_DIR)/sched-lifecycle-native.o
.PHONY: test-sched-lifecycle-riscv
test-sched-lifecycle-riscv: $(BUILD_DIR)/sched-lifecycle-native
	python3 -B tests/wait/native.py --arch riscv --qemu $(QEMU_RISCV64) --kernel-dir $(BUILD_DIR) --case sched-lifecycle
-include $(BUILD_DIR)/sched-lifecycle-native.d
test-riscv: test-sched-lifecycle-riscv

.PHONY: test-sleep-lock-host
test-sleep-lock-host:
	@mkdir -p build/host
	cc -std=gnu11 -O2 -Wall -Wextra -Werror -pthread -DBOAROS_MAX_CPUS=2 -DBOAROS_PAGE_SHIFT=12 -DBOAROS_TEST_IRQ_RESTORE=1 -Itests/host/sync -idirafter include tests/host/sleep_lock.c kernel/cpu.c kernel/raw_lock.c tests/host/sync/notifications.c kernel/sched/park.c kernel/sched/sync.c kernel/time.c -Wl,--wrap=kernel_raw_lock_release,--wrap=kernel_raw_lock_acquire -o build/host/sleep-lock
	build/host/sleep-lock

test-riscv: test-wait-riscv

$(BUILD_DIR)/sync-%.d: ;
-include $(BUILD_DIR)/wait-native.d $(wildcard $(BUILD_DIR)/sync-*.d)

.PHONY: test-files-concurrency-host
.PHONY: test-file-io-concurrency-host
test-file-io-concurrency-host:
	sh tests/file-io-concurrency-host.sh

.PHONY: test-ext4-node-resource-host
test-ext4-node-resource-host:
	FILE_IO_HOST_BUILD=$${FILE_IO_HOST_BUILD:-build/host/ext4-node-resource} \
	FILE_IO_HOST_TEST_SOURCE=tests/host/ext4_node_resource.c FILE_IO_HOST_TCP=0 \
	FILE_IO_HOST_LDFLAGS='-Wl,--wrap=kernel_heap_allocate_zeroed,--wrap=kernel_heap_release,--wrap=kernel_rwlock_init,--wrap=jbd_trans_set_block_dirty_range,--wrap=boaros_lwext4_inode_get,--wrap=ext4_metadata_execute,--wrap=ext4_fclose' \
	sh tests/file-io-concurrency-host.sh

test-files-concurrency-host:
	sh tests/files-concurrency-host.sh

.PHONY: test-pipe-concurrency-host
test-pipe-concurrency-host:
	sh tests/pipe-concurrency-host.sh

.PHONY: test-scheduler-stack-slots-host
test-scheduler-stack-slots-host:
	@mkdir -p build/host
	@set -e; for shift in 12 14; do \
		cc -std=gnu11 -O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections -DBOAROS_PAGE_SHIFT=$$shift -Itests/host/sync -idirafter include tests/host/kernel_stack_slots.c kernel/cpu.c kernel/raw_lock.c kernel/sched/park.c -Wl,--gc-sections -o build/host/kernel-stack-slots-$$shift; \
		build/host/kernel-stack-slots-$$shift; \
	done

.PHONY: test-scheduler-handoff-host
test-scheduler-handoff-host: test-scheduler-stack-slots-host
	@mkdir -p build/host
	@set -e; for shift in 12 14; do \
	cc -std=gnu11 -O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections -DBOAROS_PAGE_SHIFT=$$shift -Itests/host/sync -idirafter include tests/host/sched_handoff.c kernel/cpu.c kernel/raw_lock.c kernel/sched/park.c kernel/sched/wait.c kernel/sched/core.c kernel/sched/process.c kernel/sched/scheduling.c kernel/sched/accounting.c kernel/sched/lifetime.c kernel/sched/identity.c kernel/pid.c kernel/sched/policy.c kernel/sched/runqueue.c -Wl,--gc-sections,--wrap=scheduler_switch_claimed,--wrap=kernel_raw_lock_acquire -o build/host/scheduler-handoff-$$shift; \
		build/host/scheduler-handoff-$$shift; \
	done

.PHONY: test-signal-pending-host
test-signal-pending-host:
	@mkdir -p build/host/signal-pending
	@set -e; for shift in 12 14; do \
	cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -pthread -DBOAROS_HOST_IRQ_STATE=1 -DBOAROS_PAGE_SHIFT=$$shift -DBOAROS_MAX_CPUS=2 -Itests/host/random -idirafter include -fsanitize=address,undefined tests/host/signal_pending.c kernel/signal_pending.c kernel/raw_lock.c tests/host/cpu_context.c -o build/host/signal-pending/cases-$$shift; \
	build/host/signal-pending/cases-$$shift; done

.PHONY: test-signal-stack-host
test-signal-stack-host:
	@mkdir -p build/host
	@for shift in 12 14; do \
		cc -std=gnu11 -O2 -Wall -Wextra -Werror -DBOAROS_PAGE_SHIFT=$$shift -Itests/host/random -idirafter include tests/host/signal_stack.c kernel/signal_stack.c -o build/host/signal-stack-$$shift && build/host/signal-stack-$$shift || exit 1; \
	done

.PHONY: test-sched-concurrency-host
test-sched-concurrency-host:
	@mkdir -p build/host
	cc -std=gnu11 -O2 -Wall -Wextra -Werror -pthread -DBOAROS_MAX_CPUS=2 -ffunction-sections -fdata-sections -DBOAROS_PAGE_SHIFT=12 -Itests/host/sync -idirafter include tests/host/sched_concurrency.c tests/host/signal_owner.c kernel/signal_pending.c kernel/cpu.c kernel/raw_lock.c tests/host/sync/notifications.c kernel/sched/park.c kernel/sched/scheduling.c kernel/sched/accounting.c kernel/sched/lifetime.c kernel/sched/identity.c kernel/sched/process.c kernel/pid.c kernel/sched/policy.c kernel/sched/runqueue.c -Wl,--gc-sections,--wrap=kernel_sched_policy_charge,--wrap=kernel_sched_policy_set,--wrap=kernel_raw_lock_acquire,--wrap=kernel_raw_lock_release -o build/host/sched-concurrency
	build/host/sched-concurrency
	cc -std=gnu11 -O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections -DBOAROS_PAGE_SHIFT=12 -Itests/host/sync -idirafter include tests/host/process_child_links.c kernel/cpu.c kernel/raw_lock.c -Wl,--gc-sections -o build/host/process-child-links
	build/host/process-child-links

test-riscv: test-scheduler-handoff-riscv

.PHONY: kernel-rv-smp run-smp-riscv test-smp-host test-smp-riscv
.PHONY: kernel-rv-user-smp run-user-smp-riscv test-user-smp-host
kernel-rv-user-smp:
	$(MAKE) SMP=1 USER_SMP=1 COST_DIAGNOSTICS=0 all
run-user-smp-riscv: kernel-rv-user-smp
	$(QEMU_RISCV64) -machine virt -accel tcg,thread=multi -smp $(SMP_CPUS) -m $(QEMU_MEMORY) -nographic -bios $(QEMU_RISCV_FIRMWARE) -kernel build/riscv-user-smp$(SMP_BUILD_SUFFIX)/kernel-rv
test-user-smp-host:
	@mkdir -p build/host/mm
	$(MAKE) test-service-scope-host
	cc -std=gnu11 -O2 -Wall -Wextra -Werror -pthread -DBOAROS_MAX_CPUS=2 -ffunction-sections -fdata-sections -DBOAROS_PAGE_SHIFT=12 -DBOAROS_USER_SMP=1 -Itests/host/sync -idirafter include tests/host/user_smp.c tests/host/user_stop.c kernel/signal_pending.c kernel/cpu.c kernel/raw_lock.c tests/host/sync/notifications.c kernel/sched/sync.c kernel/sched/park.c kernel/sched/wait.c kernel/sched/scheduling.c kernel/sched/accounting.c kernel/sched/lifetime.c kernel/sched/policy.c kernel/sched/runqueue.c -Wl,--gc-sections,--wrap=kernel_raw_lock_release,--wrap=kernel_scheduler_wake_signal,--wrap=kernel_wait_backend_quiesce -o build/host/user-smp
	build/host/user-smp
	python3 -B tests/smp/test_user.py
	cc -std=gnu11 -O2 -Wall -Wextra -Werror -pthread -DBOAROS_MAX_CPUS=2 -ffunction-sections -fdata-sections -DBOAROS_PAGE_SHIFT=12 -DBOAROS_USER_SMP=1 -DREMOTE_USER_CASE=1 -Itests/host/mm -Itests/host/sync -idirafter include tests/host/mm_activity.c tests/host/mm/unavailable.c tests/host/mm/context.c kernel/sched/proc.c kernel/sched/accounting.c kernel/cpu.c kernel/raw_lock.c tests/host/sync/notifications.c kernel/sched/sync.c kernel/sched/park.c kernel/physical_page.c mm/heap.c mm/memory_object.c mm/vma.c mm/mm.c mm/uaccess.c arch/riscv/sv39.c -Wl,--gc-sections,--wrap=riscv_sv39_user_detach_range,--wrap=physical_page_allocate,--wrap=physical_page_map_owned -o build/host/mm/remote-user
	timeout 30s build/host/mm/remote-user

.PHONY: test-user-smp-riscv
USER_SMP_ELF := build/riscv-user-smp$(SMP_BUILD_SUFFIX)/user-smp-rv
$(USER_SMP_ELF): tests/userland/user_smp.c tests/userland/user_smp_many.h tests/userland/user_smp_probe.S build/riscv/musl-root/.shared-built
	@mkdir -p $(dir $@)
	build/riscv/musl-root/bin/musl-gcc $(shell build/riscv/musl-root/bin/musl-gcc -fno-link-libatomic -E -x c /dev/null >/dev/null 2>&1 && echo -fno-link-libatomic) -static -O2 -pthread -Wall -Wextra -Werror -o $@ tests/userland/user_smp.c tests/userland/user_smp_probe.S
USER_SMP_DYNAMIC_ELF := build/riscv-user-smp$(SMP_BUILD_SUFFIX)/user-smp-dynamic-rv
$(USER_SMP_DYNAMIC_ELF): tests/userland/user_smp.c tests/userland/user_smp_many.h tests/userland/user_smp_probe.S $(MUSL_LDSO)
	@mkdir -p $(dir $@)
	build/riscv/musl-root/bin/musl-gcc $(shell build/riscv/musl-root/bin/musl-gcc -fno-link-libatomic -E -x c /dev/null >/dev/null 2>&1 && echo -fno-link-libatomic) -DBOAROS_SMP_DYNAMIC=1 -Wl,--dynamic-linker=/lib/ld-musl-riscv64.so.1 -fPIC -pie -O2 -pthread -Wall -Wextra -Werror -o $@ tests/userland/user_smp.c tests/userland/user_smp_probe.S
test-user-smp-riscv: $(USER_SMP_ELF) $(USER_SMP_DYNAMIC_ELF) $(PTHREAD_TLS_DSO_RV)
	$(MAKE) kernel-rv-user-smp
	python3 -B tests/smp/user.py --qemu $(QEMU_RISCV64) --firmware $(QEMU_RISCV_FIRMWARE) --kernel $(USER_SMP_BUILD)/kernel-rv --program $(USER_SMP_ELF) $(if $(filter 4,$(SMP_CPUS)),--cpus 4 --cpus 2 --cpus 1,) $(TEST_MEMORY_ARGS)
	python3 -B tests/smp/user.py --qemu $(QEMU_RISCV64) --firmware $(QEMU_RISCV_FIRMWARE) --kernel $(USER_SMP_BUILD)/kernel-rv $(if $(filter 4,$(SMP_CPUS)),--cpus 4 --cpus 2 --cpus 1,) --program $(USER_SMP_DYNAMIC_ELF) --loader $(MUSL_LDSO) --library $(PTHREAD_TLS_DSO_RV) $(TEST_MEMORY_ARGS)
.PHONY: test-user-smp-runtime-riscv
.PHONY: test-user-smp-programs-riscv
.PHONY: test-user-smp-composition-host test-user-smp-composition-riscv test-wait-process-smp-riscv
COMPOSITION_ELF := build/riscv-user-smp$(SMP_BUILD_SUFFIX)-composition/composition-rv
$(COMPOSITION_ELF): tests/userland/user_smp_composition.c tests/smp/composition_protocol.h $(MUSL_STAMP)
	@mkdir -p $(dir $@)
	build/riscv/musl-root/bin/musl-gcc $(shell build/riscv/musl-root/bin/musl-gcc -fno-link-libatomic -E -x c /dev/null >/dev/null 2>&1 && echo -fno-link-libatomic) -static -O2 -pthread -Wall -Wextra -Werror -o $@ tests/userland/user_smp_composition.c
test-user-smp-composition-host:
	python3 -B tests/smp/test_composition.py
test-user-smp-composition-riscv: $(COMPOSITION_ELF) $(SQLITE_ROLLBACK_RV) $(SQLITE_WAL_RV) $(SQLITE_CLI_INIT_RV) $(SQLITE_CLI_STATIC_RV) $(SQLITE_CLI_DYNAMIC_RV) $(MUSL_LDSO) test-user-smp-composition-host
	python3 -B tests/smp/composition.py --firmware $(QEMU_RISCV_FIRMWARE) --qemu $(QEMU_RISCV64) --program $(COMPOSITION_ELF) $(TEST_MEMORY_ARGS)
test-wait-process-smp-riscv: $(COMPOSITION_ELF) test-user-smp-composition-host
	python3 -B tests/smp/composition.py --scene wait --firmware $(QEMU_RISCV_FIRMWARE) --qemu $(QEMU_RISCV64) --program $(COMPOSITION_ELF) $(TEST_MEMORY_ARGS)
test-user-smp-programs-riscv: test-program-inventory-host
	$(MAKE) kernel-rv-user-smp
	python3 -B tests/program-inventory/run.py --execution-profile rv-user-smp --firmware $(QEMU_RISCV_FIRMWARE) --qemu $(QEMU_RISCV64) --reuse-builds $(TEST_MEMORY_ARGS)
test-user-smp-runtime-riscv: $(REAL_USERLAND_RV) $(PTHREAD_USERLAND_RV) $(PTHREAD_TLS_DSO_RV) $(MUSL_LDSO) $(DIFF_ABI_RV)
	$(MAKE) kernel-rv-user-smp
	python3 -B tests/smp/user.py --qemu $(QEMU_RISCV64) --firmware $(QEMU_RISCV_FIRMWARE) --kernel $(USER_SMP_BUILD)/kernel-rv --program $(DIFF_ABI_RV) --manifest tests/diff-abi/cases.txt $(TEST_MEMORY_ARGS)
	python3 -B tests/smp/runtime.py --qemu $(QEMU_RISCV64) --firmware $(QEMU_RISCV_FIRMWARE) --kernel $(USER_SMP_BUILD)/kernel-rv --program $(REAL_USERLAND_RV) --pthread-program $(PTHREAD_USERLAND_RV) --tls $(PTHREAD_TLS_DSO_RV) --loader $(MUSL_LDSO) $(TEST_MEMORY_ARGS)
.PHONY: test-mm-smp-riscv
test-mm-smp-riscv:
	$(MAKE) SMP=1 SMP_TEST=1 MM_SMP_TEST=1 COST_DIAGNOSTICS=0 all
	python3 -B tests/smp/riscv.py --contract mm --qemu $(QEMU_RISCV64) \
		--firmware $(QEMU_RISCV_FIRMWARE) --kernel build/riscv-mm-smp-test$(SMP_BUILD_SUFFIX)/kernel-rv \
		--output build/riscv-mm-smp-test$(SMP_BUILD_SUFFIX)/runs
	@set -e; for profile in rfence-error mm-fence-i; do \
		$(MAKE) SMP=1 SMP_TEST=1 MM_SMP_TEST=1 SMP_TEST_PROFILE=$$profile COST_DIAGNOSTICS=0 all; \
		python3 -B tests/smp/riscv.py --contract mm --qemu $(QEMU_RISCV64) --firmware $(QEMU_RISCV_FIRMWARE) --kernel build/riscv-mm-smp-test$(SMP_BUILD_SUFFIX)/$$profile/kernel-rv --profile $$profile --output build/riscv-mm-smp-test$(SMP_BUILD_SUFFIX)/runs/$$profile; \
	done
QEMU_RISCV_FIRMWARE ?= default
kernel-rv-smp:
	$(MAKE) SMP=1 SMP_TEST=0 COST_DIAGNOSTICS=0 all
run-smp-riscv: kernel-rv-smp
	$(QEMU_RISCV64) -machine virt -accel tcg,thread=multi -smp $(SMP_CPUS) -m $(QEMU_MEMORY) -nographic -bios $(QEMU_RISCV_FIRMWARE) -kernel build/riscv-smp$(SMP_BUILD_SUFFIX)/kernel-rv
test-smp-host:
	python3 -B tests/host/smp_dtb_test.py
	python3 -B tests/smp/mm_contract_test.py
	@mkdir -p build/host
	cc -std=gnu11 -O2 -Wall -Wextra -Werror -pthread -DBOAROS_MAX_CPUS=2 -ffunction-sections -fdata-sections -DBOAROS_PAGE_SHIFT=12 -Itests/host/sync -idirafter include tests/host/smp_sched.c kernel/cpu.c kernel/raw_lock.c tests/host/sync/notifications.c kernel/sched/wait.c kernel/sched/scheduling.c kernel/sched/accounting.c kernel/sched/policy.c kernel/sched/runqueue.c -Wl,--gc-sections -o build/host/smp-sched
	build/host/smp-sched
	cc -std=gnu11 -O2 -Wall -Wextra -Werror -pthread -DBOAROS_MAX_CPUS=4 -ffunction-sections -fdata-sections -DBOAROS_PAGE_SHIFT=12 -Itests/host/sync -idirafter include tests/host/smp_sched.c kernel/cpu.c kernel/raw_lock.c tests/host/sync/notifications.c kernel/sched/wait.c kernel/sched/scheduling.c kernel/sched/accounting.c kernel/sched/policy.c kernel/sched/runqueue.c -Wl,--gc-sections -o build/host/smp-sched4
	build/host/smp-sched4
test-smp-riscv:
	$(MAKE) SMP=1 SMP_TEST=1 SMP_TEST_PROFILE=normal COST_DIAGNOSTICS=0 all
	python3 -B tests/smp/riscv.py --qemu $(QEMU_RISCV64) --firmware $(QEMU_RISCV_FIRMWARE) --kernel build/riscv-smp$(SMP_BUILD_SUFFIX)-test/kernel-rv $(if $(filter 4,$(SMP_CPUS)),--cpus 4 --cpus 2 --cpus 1,)
	@set -e; for profile in missing-rfence denied-start handshake-timeout guard denied-stop rfence-error; do \
		$(MAKE) SMP=1 SMP_TEST=1 SMP_TEST_PROFILE=$$profile COST_DIAGNOSTICS=0 all; \
		python3 -B tests/smp/riscv.py --qemu $(QEMU_RISCV64) --firmware $(QEMU_RISCV_FIRMWARE) --kernel build/riscv-smp$(SMP_BUILD_SUFFIX)-test/$$profile/kernel-rv --profile $$profile $(if $(filter 4,$(SMP_CPUS)),--cpus 4,) --output build/riscv-smp$(SMP_BUILD_SUFFIX)/runs/$$profile; \
	done

.PHONY: test-smp-fpu-setup-riscv
test-smp-fpu-setup-riscv:
	$(MAKE) SMP=1 SMP_TEST=1 COST_DIAGNOSTICS=0 BUILD_DIR=build/riscv-smp$(SMP_BUILD_SUFFIX)-test/fpu-setup/riscv KERNEL_RV=build/riscv-smp$(SMP_BUILD_SUFFIX)-test/fpu-setup/kernel-rv CFLAGS_EXTRA=-DBOAROS_SMP_FPU_SETUP_TEST=1 all
	python3 -B tests/smp/riscv.py --qemu $(QEMU_RISCV64) --firmware $(QEMU_RISCV_FIRMWARE) --kernel build/riscv-smp$(SMP_BUILD_SUFFIX)-test/fpu-setup/kernel-rv --output build/riscv-smp$(SMP_BUILD_SUFFIX)/runs/fpu-setup --require-fpu-setup

.PHONY: test-smp-userland-riscv
.PHONY: test-mm-smp-userland-riscv
test-mm-smp-userland-riscv: $(REAL_USERLAND_RV) $(PTHREAD_USERLAND_RV) $(PTHREAD_TLS_DSO_RV) $(MUSL_LDSO)
	$(MAKE) SMP=1 SMP_TEST=1 MM_SMP_TEST=1 COST_DIAGNOSTICS=0 all
	@set -e; for memory in 512M 1G; do \
		BOAROS_SMP_QEMU=$(QEMU_RISCV64) BOAROS_SMP_FIRMWARE=$(QEMU_RISCV_FIRMWARE) \
		BOAROS_SMP_OUTPUT=$(CURDIR)/build/riscv-mm-smp-test$(SMP_BUILD_SUFFIX)/runs/user-$$memory \
		QEMU_RISCV64=$(CURDIR)/tests/smp/qemu.py KERNEL_RV=$(CURDIR)/build/riscv-mm-smp-test$(SMP_BUILD_SUFFIX)/kernel-rv \
		REAL_USERLAND_RV=$(REAL_USERLAND_RV) PTHREAD_USERLAND_RV=$(PTHREAD_USERLAND_RV) \
		PTHREAD_TLS_DSO_RV=$(PTHREAD_TLS_DSO_RV) MUSL_LDSO=$(MUSL_LDSO) \
		QEMU_MEMORY=$$memory USERLAND_TIMEOUT=185s tests/userland-riscv.sh; \
	done
test-smp-userland-riscv: $(REAL_USERLAND_RV) $(PTHREAD_USERLAND_RV) $(PTHREAD_TLS_DSO_RV) $(MUSL_LDSO)
	$(MAKE) SMP=1 SMP_TEST=1 SMP_TEST_PROFILE=normal COST_DIAGNOSTICS=0 all
	@set -e; for memory in 512M 1G; do \
		BOAROS_SMP_QEMU=$(QEMU_RISCV64) BOAROS_SMP_FIRMWARE=$(QEMU_RISCV_FIRMWARE) \
		BOAROS_SMP_OUTPUT=$(CURDIR)/build/riscv-smp$(SMP_BUILD_SUFFIX)/runs/user-$$memory \
		QEMU_RISCV64=$(CURDIR)/tests/smp/qemu.py KERNEL_RV=$(CURDIR)/build/riscv-smp$(SMP_BUILD_SUFFIX)-test/kernel-rv \
		REAL_USERLAND_RV=$(REAL_USERLAND_RV) PTHREAD_USERLAND_RV=$(PTHREAD_USERLAND_RV) \
		PTHREAD_TLS_DSO_RV=$(PTHREAD_TLS_DSO_RV) MUSL_LDSO=$(MUSL_LDSO) \
		QEMU_MEMORY=$$memory USERLAND_TIMEOUT=185s tests/userland-riscv.sh; \
	done

.PHONY: test-mm-shared-smp-riscv test-mm-shared-smp-userland-riscv
test-mm-shared-smp-riscv:
	$(MAKE) SMP=1 SMP_TEST=1 MM_SMP_TEST=1 MM_SHARED_SMP_TEST=1 COST_DIAGNOSTICS=0 all
	python3 -B tests/smp/riscv.py --contract mm-shared --qemu $(QEMU_RISCV64) --firmware $(QEMU_RISCV_FIRMWARE) --kernel build/riscv-mm-shared-smp-test$(SMP_BUILD_SUFFIX)/kernel-rv --output build/riscv-mm-shared-smp-test$(SMP_BUILD_SUFFIX)/runs
	$(MAKE) SMP=1 SMP_TEST=1 MM_SMP_TEST=1 MM_SHARED_SMP_TEST=1 SMP_TEST_PROFILE=rfence-error COST_DIAGNOSTICS=0 all
	python3 -B tests/smp/riscv.py --contract mm-shared --qemu $(QEMU_RISCV64) --firmware $(QEMU_RISCV_FIRMWARE) --kernel build/riscv-mm-shared-smp-test$(SMP_BUILD_SUFFIX)/rfence-error/kernel-rv --profile rfence-error --output build/riscv-mm-shared-smp-test$(SMP_BUILD_SUFFIX)/runs/rfence-error

test-mm-shared-smp-userland-riscv: $(REAL_USERLAND_RV) $(PTHREAD_USERLAND_RV) $(PTHREAD_TLS_DSO_RV) $(MUSL_LDSO)
	$(MAKE) SMP=1 SMP_TEST=1 MM_SMP_TEST=1 MM_SHARED_SMP_TEST=1 COST_DIAGNOSTICS=0 all
	@set -e; for memory in 512M 1G; do \
		BOAROS_SMP_QEMU=$(QEMU_RISCV64) BOAROS_SMP_FIRMWARE=$(QEMU_RISCV_FIRMWARE) \
		BOAROS_SMP_OUTPUT=$(CURDIR)/build/riscv-mm-shared-smp-test$(SMP_BUILD_SUFFIX)/runs/user-$$memory \
		QEMU_RISCV64=$(CURDIR)/tests/smp/qemu.py KERNEL_RV=$(CURDIR)/build/riscv-mm-shared-smp-test$(SMP_BUILD_SUFFIX)/kernel-rv \
		REAL_USERLAND_RV=$(REAL_USERLAND_RV) PTHREAD_USERLAND_RV=$(PTHREAD_USERLAND_RV) \
		PTHREAD_TLS_DSO_RV=$(PTHREAD_TLS_DSO_RV) MUSL_LDSO=$(MUSL_LDSO) \
		QEMU_MEMORY=$$memory USERLAND_TIMEOUT=185s tests/userland-riscv.sh; \
	done


.PHONY: test-mm-file-smp-riscv test-mm-file-smp-userland-riscv
test-mm-file-smp-riscv:
	$(MAKE) SMP=1 SMP_TEST=1 MM_SMP_TEST=1 MM_SHARED_SMP_TEST=1 MM_FILE_SMP_TEST=1 COST_DIAGNOSTICS=0 all
	python3 -B tests/smp/riscv.py --contract mm-file --qemu $(QEMU_RISCV64) --firmware $(QEMU_RISCV_FIRMWARE) --kernel build/riscv-mm-file-smp-test$(SMP_BUILD_SUFFIX)/kernel-rv --output build/riscv-mm-file-smp-test$(SMP_BUILD_SUFFIX)/runs
	$(MAKE) SMP=1 SMP_TEST=1 MM_SMP_TEST=1 MM_SHARED_SMP_TEST=1 MM_FILE_SMP_TEST=1 SMP_TEST_PROFILE=rfence-error COST_DIAGNOSTICS=0 all
	python3 -B tests/smp/riscv.py --contract mm-file --qemu $(QEMU_RISCV64) --firmware $(QEMU_RISCV_FIRMWARE) --kernel build/riscv-mm-file-smp-test$(SMP_BUILD_SUFFIX)/rfence-error/kernel-rv --profile rfence-error --output build/riscv-mm-file-smp-test$(SMP_BUILD_SUFFIX)/runs/rfence-error

test-mm-file-smp-userland-riscv: $(REAL_USERLAND_RV) $(PTHREAD_USERLAND_RV) $(PTHREAD_TLS_DSO_RV) $(MUSL_LDSO)
	$(MAKE) SMP=1 SMP_TEST=1 MM_SMP_TEST=1 MM_SHARED_SMP_TEST=1 MM_FILE_SMP_TEST=1 COST_DIAGNOSTICS=0 all
	@set -e; for memory in 512M 1G; do \
		BOAROS_SMP_QEMU=$(QEMU_RISCV64) BOAROS_SMP_FIRMWARE=$(QEMU_RISCV_FIRMWARE) \
		BOAROS_SMP_OUTPUT=$(CURDIR)/build/riscv-mm-file-smp-test$(SMP_BUILD_SUFFIX)/runs/user-$$memory \
		QEMU_RISCV64=$(CURDIR)/tests/smp/qemu.py KERNEL_RV=$(CURDIR)/build/riscv-mm-file-smp-test$(SMP_BUILD_SUFFIX)/kernel-rv \
		REAL_USERLAND_RV=$(REAL_USERLAND_RV) PTHREAD_USERLAND_RV=$(PTHREAD_USERLAND_RV) \
		PTHREAD_TLS_DSO_RV=$(PTHREAD_TLS_DSO_RV) MUSL_LDSO=$(MUSL_LDSO) \
		QEMU_MEMORY=$$memory USERLAND_TIMEOUT=185s tests/userland-riscv.sh; \
	done

.PHONY: test-mm-disk-concurrency-host test-mm-disk-smp-riscv test-mm-disk-smp-userland-riscv
test-mm-disk-concurrency-host:
	sh tests/mm-disk-concurrency-host.sh

test-mm-disk-smp-riscv:
	$(MAKE) SMP=1 SMP_TEST=1 MM_SMP_TEST=1 MM_SHARED_SMP_TEST=1 MM_FILE_SMP_TEST=1 MM_DISK_SMP_TEST=1 COST_DIAGNOSTICS=0 all
	$(CC) $(CPPFLAGS) $(CFLAGS) -nostdlib -static -Wl,-e,_start tests/riscv/mm_disk_init.S -o build/riscv-mm-disk-smp-test$(SMP_BUILD_SUFFIX)/init
	python3 -B tests/smp/riscv.py --contract mm-disk --qemu $(QEMU_RISCV64) --firmware $(QEMU_RISCV_FIRMWARE) --kernel build/riscv-mm-disk-smp-test$(SMP_BUILD_SUFFIX)/kernel-rv --init build/riscv-mm-disk-smp-test$(SMP_BUILD_SUFFIX)/init --output build/riscv-mm-disk-smp-test$(SMP_BUILD_SUFFIX)/runs
	$(MAKE) SMP=1 SMP_TEST=1 MM_SMP_TEST=1 MM_SHARED_SMP_TEST=1 MM_FILE_SMP_TEST=1 MM_DISK_SMP_TEST=1 SMP_TEST_PROFILE=rfence-error COST_DIAGNOSTICS=0 all
	python3 -B tests/smp/riscv.py --contract mm-disk --qemu $(QEMU_RISCV64) --firmware $(QEMU_RISCV_FIRMWARE) --kernel build/riscv-mm-disk-smp-test$(SMP_BUILD_SUFFIX)/rfence-error/kernel-rv --init build/riscv-mm-disk-smp-test$(SMP_BUILD_SUFFIX)/init --profile rfence-error --output build/riscv-mm-disk-smp-test$(SMP_BUILD_SUFFIX)/runs/rfence-error

test-mm-disk-smp-userland-riscv: $(REAL_USERLAND_RV) $(PTHREAD_USERLAND_RV) $(PTHREAD_TLS_DSO_RV) $(MUSL_LDSO)
	$(MAKE) SMP=1 SMP_TEST=1 MM_SMP_TEST=1 MM_SHARED_SMP_TEST=1 MM_FILE_SMP_TEST=1 MM_DISK_SMP_TEST=1 COST_DIAGNOSTICS=0 all
	@set -e; for memory in 512M 1G; do \
		BOAROS_SMP_DISK=1 BOAROS_SMP_QEMU=$(QEMU_RISCV64) BOAROS_SMP_FIRMWARE=$(QEMU_RISCV_FIRMWARE) \
		BOAROS_SMP_OUTPUT=$(CURDIR)/build/riscv-mm-disk-smp-test$(SMP_BUILD_SUFFIX)/runs/user-$$memory \
		QEMU_RISCV64=$(CURDIR)/tests/smp/qemu.py KERNEL_RV=$(CURDIR)/build/riscv-mm-disk-smp-test$(SMP_BUILD_SUFFIX)/kernel-rv \
		REAL_USERLAND_RV=$(REAL_USERLAND_RV) PTHREAD_USERLAND_RV=$(PTHREAD_USERLAND_RV) \
		PTHREAD_TLS_DSO_RV=$(PTHREAD_TLS_DSO_RV) MUSL_LDSO=$(MUSL_LDSO) \
		QEMU_MEMORY=$$memory USERLAND_TIMEOUT=185s tests/userland-riscv.sh; \
	done

.PHONY: test-file-io-smp-riscv test-file-io-smp-userland-riscv
test-file-io-smp-riscv:
	$(MAKE) SMP=1 SMP_TEST=1 MM_SMP_TEST=1 MM_SHARED_SMP_TEST=1 MM_FILE_SMP_TEST=1 MM_DISK_SMP_TEST=1 FILES_SMP_TEST=1 FILE_IO_SMP_TEST=1 COST_DIAGNOSTICS=0 all
	$(CC) $(CPPFLAGS) $(CFLAGS) -nostdlib -static -Wl,-e,_start tests/riscv/mm_disk_init.S -o $(FILE_IO_SMP_BUILD)/init
	python3 -B tests/smp/riscv.py --contract file-io --qemu $(QEMU_RISCV64) --firmware $(QEMU_RISCV_FIRMWARE) --kernel $(FILE_IO_SMP_BUILD)/kernel-rv --init $(FILE_IO_SMP_BUILD)/init --output $(FILE_IO_SMP_BUILD)/runs

test-file-io-smp-userland-riscv: $(REAL_USERLAND_RV) $(PTHREAD_USERLAND_RV) $(PTHREAD_TLS_DSO_RV) $(MUSL_LDSO)
	$(MAKE) SMP=1 SMP_TEST=1 MM_SMP_TEST=1 MM_SHARED_SMP_TEST=1 MM_FILE_SMP_TEST=1 MM_DISK_SMP_TEST=1 FILES_SMP_TEST=1 FILE_IO_SMP_TEST=1 COST_DIAGNOSTICS=0 all
	@set -e; for memory in 512M 1G; do \
		BOAROS_SMP_FILE_IO=1 BOAROS_SMP_FILES=1 BOAROS_SMP_DISK=1 BOAROS_SMP_QEMU=$(QEMU_RISCV64) BOAROS_SMP_FIRMWARE=$(QEMU_RISCV_FIRMWARE) \
		BOAROS_SMP_OUTPUT=$(abspath $(FILE_IO_SMP_BUILD))/runs/user-$$memory \
		QEMU_RISCV64=$(CURDIR)/tests/smp/qemu.py KERNEL_RV=$(abspath $(FILE_IO_SMP_BUILD))/kernel-rv \
		REAL_USERLAND_RV=$(REAL_USERLAND_RV) PTHREAD_USERLAND_RV=$(PTHREAD_USERLAND_RV) \
		PTHREAD_TLS_DSO_RV=$(PTHREAD_TLS_DSO_RV) MUSL_LDSO=$(MUSL_LDSO) \
		QEMU_MEMORY=$$memory USERLAND_TIMEOUT=185s tests/userland-riscv.sh; \
	done

.PHONY: test-files-smp-riscv test-files-smp-userland-riscv
test-files-smp-riscv:
	$(MAKE) SMP=1 SMP_TEST=1 MM_SMP_TEST=1 MM_SHARED_SMP_TEST=1 MM_FILE_SMP_TEST=1 MM_DISK_SMP_TEST=1 FILES_SMP_TEST=1 COST_DIAGNOSTICS=0 all
	$(CC) $(CPPFLAGS) $(CFLAGS) -nostdlib -static -Wl,-e,_start tests/riscv/mm_disk_init.S -o $(FILES_SMP_BUILD)/init
	python3 -B tests/smp/riscv.py --contract files --qemu $(QEMU_RISCV64) --firmware $(QEMU_RISCV_FIRMWARE) --kernel $(FILES_SMP_BUILD)/kernel-rv --init $(FILES_SMP_BUILD)/init --output $(FILES_SMP_BUILD)/runs

test-files-smp-userland-riscv: $(REAL_USERLAND_RV) $(PTHREAD_USERLAND_RV) $(PTHREAD_TLS_DSO_RV) $(MUSL_LDSO)
	$(MAKE) SMP=1 SMP_TEST=1 MM_SMP_TEST=1 MM_SHARED_SMP_TEST=1 MM_FILE_SMP_TEST=1 MM_DISK_SMP_TEST=1 FILES_SMP_TEST=1 COST_DIAGNOSTICS=0 all
	@set -e; for memory in 512M 1G; do \
		BOAROS_SMP_FILES=1 BOAROS_SMP_DISK=1 BOAROS_SMP_QEMU=$(QEMU_RISCV64) BOAROS_SMP_FIRMWARE=$(QEMU_RISCV_FIRMWARE) \
		BOAROS_SMP_OUTPUT=$(abspath $(FILES_SMP_BUILD))/runs/user-$$memory \
		QEMU_RISCV64=$(CURDIR)/tests/smp/qemu.py KERNEL_RV=$(abspath $(FILES_SMP_BUILD))/kernel-rv \
		REAL_USERLAND_RV=$(REAL_USERLAND_RV) PTHREAD_USERLAND_RV=$(PTHREAD_USERLAND_RV) \
		PTHREAD_TLS_DSO_RV=$(PTHREAD_TLS_DSO_RV) MUSL_LDSO=$(MUSL_LDSO) \
		QEMU_MEMORY=$$memory USERLAND_TIMEOUT=185s tests/userland-riscv.sh; \
	done

.PHONY: test-instruction-sync-host
test-instruction-sync-host:
	python3 -B tests/test-instruction-sync.py

# Fixed CPU-accounting cost workloads; supply saved baseline kernels to cost.py.
.PHONY: build-accounting-cost
build-accounting-cost: $(BUILD_DIR)/tests/user/cpu-account-cost-rv $(LA_BUILD)/cpu-account-cost $(SQLITE_CLI_STATIC_RV) $(LA_BUILD)/sqlite3-static
$(BUILD_DIR)/tests/user/cpu-account-cost-rv: tests/accounting/cost.c $(MUSL_STAMP)
	@mkdir -p $(dir $@)
	$(MUSL_ROOT)/bin/musl-gcc $(MUSL_GCC_FLAGS) -static -pthread -O2 -Wall -Wextra -Werror $< -o $@
$(LA_BUILD)/cpu-account-cost: tests/accounting/cost.c $(LA_SQLITE_CC)
	REALGCC=$(LA_CC) $(LA_SQLITE_CC) $(LA_SQLITE_FLAGS) -static -pthread -O2 -Wall -Wextra -Werror $< -o $@

.PHONY: test-accounting-cost-host
test-accounting-cost-host:
	python3 -B tests/accounting/cost_test.py

.PHONY: test-service-scope-host
test-service-scope-host:
	@mkdir -p build/host/scope
	@for mode in 0 1; do \
	  cc -std=gnu11 -O2 -Wall -Wextra -Werror -DBOAROS_PAGE_SHIFT=12 -DBOAROS_MAX_CPUS=2 -DBOAROS_USER_SMP=$$mode -Itests/host/sync -idirafter include tests/host/service_scope.c -o build/host/scope/service-$$mode || exit; \
	  build/host/scope/service-$$mode || exit; \
	done

.PHONY: test-event-fd-host
test-event-fd-host:
	@mkdir -p build/host/event-fd
	@set -e; for shift in 12 14; do \
	cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -pthread \
		-DBOAROS_HOST_IRQ_STATE=1 -DBOAROS_PAGE_SHIFT=$$shift -DBOAROS_MAX_CPUS=2 \
		-ffunction-sections -fdata-sections -Wl,--gc-sections -Itests/host/random -idirafter include \
		-fsanitize=address,undefined tests/host/event_fd.c kernel/raw_lock.c tests/host/cpu_context.c \
		-o build/host/event-fd/cases-$$shift; \
	build/host/event-fd/cases-$$shift; done
