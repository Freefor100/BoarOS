LA_CROSS_COMPILE ?= loongarch64-unknown-linux-gnu-
QEMU_LOONGARCH64 ?= build/qemu-la/qemu-system-loongarch64
LA_BUILD := build/loongarch
LA_CC := $(LA_CROSS_COMPILE)gcc
LA_FLAGS := -march=loongarch64 -mabi=lp64s -msoft-float -mno-lsx -mno-lasx -mcmodel=normal
LA_CPPFLAGS := -Iinclude -DBOAROS_ARCH_LOONGARCH=1 -DBOAROS_PAGE_SHIFT=14 -DBOAROS_COST_DIAGNOSTICS=0 -DBOAROS_UTS_MACHINE=\"loongarch64\"
LA_CFLAGS := $(LA_FLAGS) -std=gnu11 -O2 -g3 -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -ffunction-sections -fdata-sections -Wall -Wextra -Werror -fstack-usage
LA_C_SOURCES := $(filter-out arch/% kernel/main.c net/ethernet.c,$(C_SOURCES)) \
    arch/loongarch/main.c arch/loongarch/mmu.c arch/loongarch/context.c \
    arch/loongarch/timer.c arch/loongarch/trap.c arch/loongarch/signal.c platform/loongarch_virt.c \
    platform/loongarch_pci.c platform/loongarch_root.c drivers/virtio/pci_block.c kernel/pci.c \
    tests/loongarch/mmu.c tests/loongarch/heap.c tests/loongarch/user_boot.c tests/loongarch/elf_failures.c
LA_ASM_SOURCES := arch/loongarch/boot.S arch/loongarch/context_switch.S \
    arch/loongarch/tlb_refill.S arch/loongarch/trap_entry.S arch/loongarch/signal_trampoline.S tests/loongarch/user_blob.S
LA_OBJECTS = $(patsubst %.c,$(LA_BUILD)/%.o,$(LA_C_SOURCES)) $(patsubst %.S,$(LA_BUILD)/%.o,$(LA_ASM_SOURCES))
$(LA_BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(LA_CC) $(LA_CPPFLAGS) $(LWEXT4_CPPFLAGS) $(LWIP_CPPFLAGS) $(LA_CFLAGS) -MMD -MP -c $< -o $@
$(LA_BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(LA_CC) $(LA_CPPFLAGS) $(LA_FLAGS) -g3 -c $< -o $@
kernel-la: $(LA_OBJECTS) arch/loongarch/linker.ld
	$(LA_CC) $(LA_FLAGS) -nostdlib -nostartfiles -static -no-pie -T arch/loongarch/linker.ld -Wl,--build-id=none,--gc-sections,--wrap=physical_page_allocate,--wrap=physical_page_allocate_order,--wrap=kernel_syscall_dispatch -Wl,-Map,$(LA_BUILD)/kernel.map -o $@ $(LA_OBJECTS) -lgcc
.PHONY: run-loongarch test-loongarch test-loongarch-boot prepare-la-tools prepare-la-linux test-stack-usage-la
prepare-la-tools:
	python3 -B tests/loongarch/prepare.py --component tools
prepare-la-linux:
	python3 -B tests/loongarch/prepare.py --component linux --cross $(LA_CROSS_COMPILE)
ifeq ($(QEMU_LOONGARCH64),build/qemu-la/qemu-system-loongarch64)
run-loongarch test-loongarch test-loongarch-boot: prepare-la-tools
endif

$(LA_BUILD)/kernel-block-la: $(LA_OBJECTS) $(LA_BUILD)/tests/loongarch/block.o arch/loongarch/linker.ld
	$(LA_CC) $(LA_FLAGS) -nostdlib -nostartfiles -static -no-pie -T arch/loongarch/linker.ld -Wl,--build-id=none,--gc-sections,--wrap=physical_page_allocate,--wrap=physical_page_allocate_order,--wrap=kernel_syscall_dispatch,--wrap=la_boot_tasks -o $@ $(LA_OBJECTS) $(LA_BUILD)/tests/loongarch/block.o -lgcc
.PHONY: test-block-loongarch test-pci-host
test-block-loongarch: $(LA_BUILD)/kernel-block-la prepare-la-tools
	python3 -B tests/loongarch/block.py --qemu $(QEMU_LOONGARCH64)
test-pci-host:
	@mkdir -p build/host
	cc -std=c11 -Wall -Wextra -Werror -idirafter include tests/host/pci.c kernel/pci.c -o build/host/pci
	build/host/pci
	python3 -B tests/host/loongarch_irq.py
run-loongarch: kernel-la
	$(QEMU_LOONGARCH64) -machine virt -cpu la464 -smp 1 -m 1G -kernel kernel-la -nographic -no-reboot
test-loongarch: kernel-la prepare-la-linux test-stack-usage-la test-loongarch-fatal
	python3 -B tests/loongarch/run.py --qemu $(QEMU_LOONGARCH64) --log $(LA_BUILD)/boaros.log
	python3 -B tests/loongarch/reference.py --qemu $(QEMU_LOONGARCH64) --cc $(LA_CC) --boaros-log $(LA_BUILD)/boaros.log
test-loongarch-boot: kernel-la
	python3 -B tests/loongarch/run.py --qemu $(QEMU_LOONGARCH64) --stage boot
test-stack-usage-la: kernel-la
	python3 -B tests/stack-usage.py $(LA_BUILD) --stack-bytes 32768 --trap-frame-bytes 304
-include $(LA_OBJECTS:.o=.d)

$(LA_BUILD)/user-probe: tests/loongarch/user.c tests/loongarch/user_start.S tests/loongarch/registers.S tests/loongarch/user.ld
	@mkdir -p $(dir $@)
	$(LA_CC) $(LA_FLAGS) -std=gnu11 -O2 -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -nostartfiles -static -no-pie -Wl,--build-id=none -Wl,-z,max-page-size=16384 -T tests/loongarch/user.ld -o $@ tests/loongarch/user.c tests/loongarch/user_start.S tests/loongarch/registers.S -lgcc
$(LA_BUILD)/tests/loongarch/user_blob.o: $(LA_BUILD)/user-probe

$(LA_BUILD)/fatal-%.o: tests/loongarch/fatal.c
	$(LA_CC) $(LA_CPPFLAGS) $(LA_CFLAGS) -DLA_FATAL_CASE=$* -c $< -o $@
$(LA_BUILD)/kernel-fatal-%: $(LA_BUILD)/fatal-%.o $(LA_OBJECTS) arch/loongarch/linker.ld
	$(LA_CC) $(LA_FLAGS) -nostdlib -nostartfiles -static -no-pie -T arch/loongarch/linker.ld -Wl,--build-id=none,--gc-sections,--wrap=physical_page_allocate,--wrap=physical_page_allocate_order,--wrap=kernel_syscall_dispatch,--wrap=la_boot_tasks -o $@ $(LA_OBJECTS) $< -lgcc
.PHONY: test-loongarch-fatal
test-loongarch-fatal: $(LA_BUILD)/kernel-fatal-1 $(LA_BUILD)/kernel-fatal-2 $(LA_BUILD)/kernel-fatal-3
	python3 -B tests/loongarch/fatal.py --qemu $(QEMU_LOONGARCH64)
ifeq ($(QEMU_LOONGARCH64),build/qemu-la/qemu-system-loongarch64)
test-loongarch-fatal: prepare-la-tools
endif

$(LA_BUILD)/generated/init-config.h: force-init-config $(INIT_CONFIG) tools/init-config.py
	python3 -B tools/init-config.py $(INIT_CONFIG) $@
$(LA_BUILD)/platform/loongarch_root.o: $(LA_BUILD)/generated/init-config.h
$(LA_BUILD)/platform/loongarch_root.o: LA_CPPFLAGS += -I$(LA_BUILD)/generated
.PHONY: prepare-la-userland test-root-loongarch
prepare-la-userland:
	python3 -B tests/loongarch/prepare_userland.py --cross $(LA_CROSS_COMPILE)
test-root-loongarch: kernel-la prepare-la-tools prepare-la-linux prepare-la-userland
	python3 -B tests/loongarch/root.py --qemu $(QEMU_LOONGARCH64) --cc $(LA_CC) --oom-kernel $(LA_BUILD)/kernel-root-oom-1 --fault-program $(LA_BUILD)/root-fault
	python3 -B tests/loongarch/root.py --qemu $(QEMU_LOONGARCH64) --cc $(LA_CC) --smoke --oom-kernel $(LA_BUILD)/kernel-root-oom-2
	python3 -B tests/loongarch/root.py --qemu $(QEMU_LOONGARCH64) --cc $(LA_CC) --smoke --oom-kernel $(LA_BUILD)/kernel-root-oom-3
	python3 -B tests/loongarch/root.py --qemu $(QEMU_LOONGARCH64) --cc $(LA_CC) --linux
LA_USER_CC := $(LA_BUILD)/gcc-sf/root/bin/loongarch64-unknown-linux-gnusf-gcc
LA_MUSL_CC := $(LA_BUILD)/musl-root/bin/musl-gcc
$(LA_BUILD)/root-probe: tests/loongarch/root_probe.c prepare-la-userland
	REALGCC=$(abspath $(LA_USER_CC)) $(LA_MUSL_CC) $(LA_FLAGS) -O2 -static -Wall -Wextra -Werror -Wl,-z,max-page-size=16384 -o $@ $<
test-root-loongarch: $(LA_BUILD)/root-probe
$(LA_BUILD)/root-oom-%.o: tests/loongarch/root_oom.c
	$(LA_CC) $(LA_CPPFLAGS) $(LA_CFLAGS) -DROOT_OOM_CASE=$* -c $< -o $@
$(LA_BUILD)/kernel-root-oom-%: $(LA_BUILD)/root-oom-%.o $(LA_OBJECTS) arch/loongarch/linker.ld
	$(LA_CC) $(LA_FLAGS) -nostdlib -nostartfiles -static -no-pie -T arch/loongarch/linker.ld -Wl,--build-id=none,--gc-sections,--wrap=physical_page_allocate,--wrap=physical_page_allocate_order,--wrap=kernel_syscall_dispatch,--wrap=$(if $(filter 1,$*),kernel_exec_image_prepare,$(if $(filter 2,$*),kernel_user_thread_create,kernel_page_cache_start_worker)) -o $@ $(LA_OBJECTS) $< -lgcc

$(LA_BUILD)/root-fault: tests/loongarch/root_fault.c prepare-la-userland
	REALGCC=$(abspath $(LA_USER_CC)) $(LA_MUSL_CC) $(LA_FLAGS) -O2 -static -Wl,-z,max-page-size=16384 -o $@ $<
test-root-loongarch: $(LA_BUILD)/root-fault $(LA_BUILD)/kernel-root-oom-1 $(LA_BUILD)/kernel-root-oom-2 $(LA_BUILD)/kernel-root-oom-3

.PHONY: test-root-io-loongarch
test-root-io-loongarch: kernel-la $(LA_BUILD)/root-probe build/host/nbd-fault prepare-la-tools
	python3 -B tests/loongarch/root_io.py --qemu $(QEMU_LOONGARCH64)

.PHONY: test-la-userland-host
test-la-userland-host: prepare-la-userland
	python3 -B tests/host/la_userland_cache.py
$(LA_BUILD)/signal-probe: tests/loongarch/signals.c tests/loongarch/signal_registers.S prepare-la-userland
	REALGCC=$(abspath $(LA_USER_CC)) $(LA_MUSL_CC) $(LA_FLAGS) -O2 -static -Wall -Wextra -Werror -Wl,-z,max-page-size=16384 -o $@ tests/loongarch/signals.c tests/loongarch/signal_registers.S
.PHONY: test-signal-loongarch
test-signal-loongarch: kernel-la $(LA_BUILD)/signal-probe prepare-la-tools prepare-la-linux
	python3 -B tests/loongarch/userland.py --qemu $(QEMU_LOONGARCH64) --cc $(LA_CC) --program $(LA_BUILD)/signal-probe --marker 'LA signal layout/mask/nesting passed' --marker 'LA signal fault/recovery/badframe passed' --marker 'LA signal relocated frame passed' --marker 'LA signal integer registers passed' --marker 'LA signal wait/restart/suspend passed'

$(LA_BUILD)/pthread-probe: tests/loongarch/pthread.c tests/userland/pthread.c tests/loongarch/registers.S prepare-la-userland
	REALGCC=$(abspath $(LA_USER_CC)) $(LA_MUSL_CC) $(LA_FLAGS) -O2 -static -pthread -Wall -Wextra -Werror -Wl,-z,max-page-size=16384 -o $@ tests/loongarch/pthread.c tests/loongarch/registers.S
.PHONY: test-pthread-loongarch
test-pthread-loongarch: kernel-la $(LA_BUILD)/pthread-probe prepare-la-tools prepare-la-linux
	python3 -B tests/loongarch/userland.py --qemu $(QEMU_LOONGARCH64) --cc $(LA_CC) --program $(LA_BUILD)/pthread-probe --marker 'LA static pthread catalogue passed' --marker 'LA original BusyBox ash signal/wait passed'

$(LA_BUILD)/pthread-oom-probe: tests/loongarch/pthread_oom.c prepare-la-userland
	REALGCC=$(abspath $(LA_USER_CC)) $(LA_MUSL_CC) $(LA_FLAGS) -O2 -static -pthread -Wall -Wextra -Werror -Wl,-z,max-page-size=16384 -o $@ $<
$(LA_BUILD)/clone-oom-%.o: tests/loongarch/clone_oom.c
	$(LA_CC) $(LA_CPPFLAGS) $(LA_CFLAGS) -DCLONE_OOM_AFTER=$* -c $< -o $@
$(LA_BUILD)/kernel-clone-oom-%: $(LA_BUILD)/clone-oom-%.o $(LA_OBJECTS) arch/loongarch/linker.ld
	$(LA_CC) $(LA_FLAGS) -nostdlib -nostartfiles -static -no-pie -T arch/loongarch/linker.ld -Wl,--build-id=none,--gc-sections,--wrap=physical_page_allocate,--wrap=physical_page_allocate_order,--wrap=kernel_syscall_dispatch,--wrap=arch_process_clone_current -o $@ $(LA_OBJECTS) $< -lgcc
.PHONY: test-pthread-oom-loongarch
test-pthread-oom-loongarch: $(LA_BUILD)/kernel-clone-oom-0 $(LA_BUILD)/kernel-clone-oom-1 $(LA_BUILD)/pthread-oom-probe prepare-la-tools prepare-la-linux
	python3 -B tests/loongarch/userland.py --platform BoarOS --qemu $(QEMU_LOONGARCH64) --cc $(LA_CC) --kernel $(LA_BUILD)/kernel-clone-oom-0 --program $(LA_BUILD)/pthread-oom-probe --marker 'LA pthread creation OOM rollback/retry passed' --marker 'LA clone failed before publication; task/stack pages restored'
	python3 -B tests/loongarch/userland.py --platform BoarOS --qemu $(QEMU_LOONGARCH64) --cc $(LA_CC) --kernel $(LA_BUILD)/kernel-clone-oom-1 --program $(LA_BUILD)/pthread-oom-probe --marker 'LA pthread creation OOM rollback/retry passed' --marker 'LA clone failed before publication; task/stack pages restored'
