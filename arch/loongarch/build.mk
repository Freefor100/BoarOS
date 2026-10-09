LA_CROSS_COMPILE ?= $(shell \
	if command -v loongarch64-unknown-linux-gnu-gcc >/dev/null 2>&1; then \
		printf '%s' loongarch64-unknown-linux-gnu-; \
	elif command -v loongarch64-linux-gnu-gcc >/dev/null 2>&1; then \
		printf '%s' loongarch64-linux-gnu-; \
	else \
		printf '%s' loongarch64-unknown-linux-gnu-; \
	fi)
QEMU_LOONGARCH64 ?= build/qemu-la-rtc/qemu-system-loongarch64
LA_BUILD := build/loongarch
LA_CC := $(LA_CROSS_COMPILE)gcc
LA_FLAGS ?= -march=loongarch64 -mabi=lp64s -msoft-float -mno-lsx -mno-lasx -mcmodel=normal
LA_CPPFLAGS := -Iinclude -DBOAROS_ARCH_LOONGARCH=1 -DBOAROS_PAGE_SHIFT=14 -DBOAROS_COST_DIAGNOSTICS=0 -DBOAROS_UTS_MACHINE=\"loongarch64\"
LA_CFLAGS := $(LA_FLAGS) -std=gnu11 -O2 -g3 -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -ffunction-sections -fdata-sections -Wall -Wextra -Werror -fstack-usage -MMD -MP
LA_C_SOURCES := $(filter-out arch/% kernel/main.c,$(C_SOURCES)) \
    arch/loongarch/main.c arch/loongarch/mmu.c arch/loongarch/context.c \
    arch/loongarch/timer.c arch/loongarch/trap.c arch/loongarch/signal.c arch/loongarch/fpu.c platform/loongarch_virt.c \
    platform/loongarch_pci.c platform/loongarch_root.c platform/loongarch_rtc.c drivers/virtio/pci_block.c drivers/virtio/pci.c drivers/virtio/pci_rng.c drivers/virtio/pci_net.c kernel/pci.c \
    tests/loongarch/mmu.c tests/loongarch/heap.c tests/loongarch/user_boot.c tests/loongarch/elf_failures.c
LA_ASM_SOURCES := arch/loongarch/boot.S arch/loongarch/context_switch.S \
    arch/loongarch/tlb_refill.S arch/loongarch/trap_entry.S arch/loongarch/signal_trampoline.S arch/loongarch/fpu_state.S tests/loongarch/user_blob.S
LA_OBJECTS = $(patsubst %.c,$(LA_BUILD)/%.o,$(LA_C_SOURCES)) $(patsubst %.S,$(LA_BUILD)/%.o,$(LA_ASM_SOURCES))
$(LA_BUILD)/%.o: %.c arch/loongarch/build.mk
	@mkdir -p $(dir $@)
	$(LA_CC) $(LA_CPPFLAGS) $(LWEXT4_CPPFLAGS) $(LWIP_CPPFLAGS) $(LA_CFLAGS) -c $< -o $@
$(LA_BUILD)/%.o: %.S arch/loongarch/build.mk
	@mkdir -p $(dir $@)
	$(LA_CC) $(LA_CPPFLAGS) $(LA_FLAGS) -g3 -MMD -MP -c $< -o $@
kernel-la: $(LA_OBJECTS) arch/loongarch/linker.ld
	$(LA_CC) $(LA_FLAGS) -nostdlib -nostartfiles -static -no-pie -T arch/loongarch/linker.ld -Wl,--build-id=none,--gc-sections,--wrap=physical_page_allocate,--wrap=physical_page_allocate_order,--wrap=kernel_syscall_dispatch -Wl,-Map,$(LA_BUILD)/kernel.map -o $@ $(LA_OBJECTS) -lgcc
.PHONY: run-loongarch test-loongarch test-loongarch-boot prepare-la-tools prepare-la-linux test-stack-usage-la
prepare-la-tools:
	python3 -B tests/loongarch/qemu_rtc.py
.PHONY: prepare-la-original-tools test-rtc-model-loongarch test-rtc-alarm-loongarch
prepare-la-original-tools:
	python3 -B tests/loongarch/prepare.py --component tools
test-rtc-model-loongarch: prepare-la-tools
	python3 -B tests/loongarch/rtc_model.py --qemu $(QEMU_LOONGARCH64)
test-rtc-alarm-loongarch: prepare-la-tools prepare-la-linux-platform prepare-la-userland
	python3 -B tests/loongarch/rtc_alarm.py
prepare-la-linux:
	python3 -B tests/loongarch/prepare.py --component linux --cross $(LA_CROSS_COMPILE)
.PHONY: prepare-la-linux-platform
prepare-la-linux-platform:
	python3 -B tests/loongarch/prepare.py --component linux --profile platform --cross $(LA_CROSS_COMPILE)
ifeq ($(QEMU_LOONGARCH64),build/qemu-la-rtc/qemu-system-loongarch64)
run-loongarch test-loongarch test-loongarch-boot: prepare-la-tools
endif

$(LA_BUILD)/kernel-block-la: $(LA_OBJECTS) $(LA_BUILD)/tests/loongarch/block.o arch/loongarch/linker.ld
	$(LA_CC) $(LA_FLAGS) -nostdlib -nostartfiles -static -no-pie -T arch/loongarch/linker.ld -Wl,--build-id=none,--gc-sections,--wrap=physical_page_allocate,--wrap=physical_page_allocate_order,--wrap=kernel_syscall_dispatch,--wrap=la_boot_tasks -o $@ $(LA_OBJECTS) $(LA_BUILD)/tests/loongarch/block.o -lgcc
.PHONY: test-block-loongarch test-pci-host
test-block-loongarch: $(LA_BUILD)/kernel-block-la prepare-la-tools
	python3 -B tests/loongarch/block.py --qemu $(QEMU_LOONGARCH64)
$(LA_BUILD)/kernel-pci-reset-owner: $(LA_OBJECTS) $(LA_BUILD)/tests/loongarch/block.o $(LA_BUILD)/tests/loongarch/pci_reset_owner.o arch/loongarch/linker.ld
	$(LA_CC) $(LA_FLAGS) -nostdlib -nostartfiles -static -no-pie -T arch/loongarch/linker.ld -Wl,--build-id=none,--gc-sections,--wrap=physical_page_allocate,--wrap=physical_page_allocate_order,--wrap=kernel_syscall_dispatch,--wrap=la_boot_tasks,--wrap=virtio_transport_reset,--wrap=virtio_pci_block_init,--wrap=virtio_pci_block_destroy -o $@ $(LA_OBJECTS) $(LA_BUILD)/tests/loongarch/block.o $(LA_BUILD)/tests/loongarch/pci_reset_owner.o -lgcc
.PHONY: test-pci-reset-owner-loongarch
test-pci-reset-owner-loongarch: $(LA_BUILD)/kernel-pci-reset-owner prepare-la-tools
	python3 -B tests/loongarch/block.py --qemu $(QEMU_LOONGARCH64) --kernel $(LA_BUILD)/kernel-pci-reset-owner
	python3 -B tests/loongarch/root_reset.py
$(LA_BUILD)/kernel-pci-root-reset: $(LA_OBJECTS) $(LA_BUILD)/tests/loongarch/pci_root_reset.o arch/loongarch/linker.ld
	$(LA_CC) $(LA_FLAGS) -nostdlib -nostartfiles -static -no-pie -T arch/loongarch/linker.ld -Wl,--build-id=none,--gc-sections,--wrap=physical_page_allocate,--wrap=physical_page_allocate_order,--wrap=kernel_syscall_dispatch,--wrap=virtio_transport_reset -o $@ $(LA_OBJECTS) $(LA_BUILD)/tests/loongarch/pci_root_reset.o -lgcc
test-pci-reset-owner-loongarch: $(LA_BUILD)/kernel-pci-root-reset $(LA_BUILD)/root-probe
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
$(LA_BUILD)/kernel-boot-random: $(LA_OBJECTS) $(LA_BUILD)/tests/loongarch/boot_random.o arch/loongarch/linker.ld
	$(LA_CC) $(LA_FLAGS) -nostdlib -nostartfiles -static -no-pie -T arch/loongarch/linker.ld -Wl,--build-id=none,--gc-sections,--wrap=physical_page_allocate,--wrap=physical_page_allocate_order,--wrap=kernel_syscall_dispatch,--wrap=la_boot_tasks -o $@ $(LA_OBJECTS) $(LA_BUILD)/tests/loongarch/boot_random.o -lgcc
.PHONY: test-boot-random-loongarch
test-boot-random-loongarch: $(LA_BUILD)/kernel-boot-random prepare-la-tools
	python3 -B tests/loongarch/run.py --kernel $(LA_BUILD)/kernel-boot-random --marker 'LA untrusted DTB random material passed'
test-stack-usage-la: kernel-la
	python3 -B tests/stack-usage.py $(LA_BUILD) --stack-bytes 32768 --trap-frame-bytes 304

$(LA_BUILD)/stack-guard-%.o: tests/loongarch/stack_guard.c
	$(LA_CC) $(LA_CPPFLAGS) $(LA_CFLAGS) -DLA_STACK_CASE=$* -c $< -o $@
$(LA_BUILD)/kernel-stack-guard-%: $(LA_OBJECTS) $(LA_BUILD)/stack-guard-%.o arch/loongarch/linker.ld
	$(LA_CC) $(LA_FLAGS) -nostdlib -nostartfiles -static -no-pie -T arch/loongarch/linker.ld -Wl,--build-id=none,--gc-sections,--wrap=physical_page_allocate,--wrap=physical_page_allocate_order,--wrap=kernel_syscall_dispatch,--wrap=la_boot_tasks -o $@ $(LA_OBJECTS) $(LA_BUILD)/stack-guard-$*.o -lgcc
.PHONY: test-stack-guard-loongarch
test-stack-guard-loongarch: $(foreach case,0 1 2 3,$(LA_BUILD)/kernel-stack-guard-$(case)) prepare-la-tools
	python3 -B tests/loongarch/stack_guard.py
	python3 -B tests/loongarch/userland.py --program $(LA_BUILD)/stack-window-user --marker 'LA kernel window user access denied'
	python3 -B tests/loongarch/run.py --kernel $(LA_BUILD)/kernel-stack-window-oom --log $(LA_BUILD)/stack-window-oom.log --marker 'LA kernel stack skeleton OOM rollback passed'
$(LA_BUILD)/stack-window-user: tests/loongarch/stack_window_user.c prepare-la-userland
	REALGCC=$(abspath $(LA_USER_CC)) $(LA_MUSL_CC) $(LA_FLAGS) -O2 -static -Wall -Wextra -Werror -Wl,-z,max-page-size=16384 -o $@ $<
test-stack-guard-loongarch: kernel-la $(LA_BUILD)/stack-window-user prepare-la-linux
$(LA_BUILD)/kernel-stack-window-oom: $(LA_OBJECTS) $(LA_BUILD)/tests/loongarch/stack_window_oom.o arch/loongarch/linker.ld
	$(LA_CC) $(LA_FLAGS) -nostdlib -nostartfiles -static -no-pie -T arch/loongarch/linker.ld -Wl,--build-id=none,--gc-sections,--wrap=physical_page_allocate,--wrap=physical_page_allocate_order,--wrap=kernel_syscall_dispatch,--wrap=la_mmu_kernel_window_initialize -o $@ $(LA_OBJECTS) $(LA_BUILD)/tests/loongarch/stack_window_oom.o -lgcc
test-stack-guard-loongarch: $(LA_BUILD)/kernel-stack-window-oom
LA_DEPENDENCIES := $(sort $(LA_OBJECTS:.o=.d) $(wildcard $(LA_BUILD)/*.d $(LA_BUILD)/tests/loongarch/*.d))
# 依赖由编译器生成；禁止GNU隐式链接规则把带.d后缀的stem当成fixture编号。
$(LA_DEPENDENCIES): ;
-include $(LA_DEPENDENCIES)

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
ifeq ($(QEMU_LOONGARCH64),build/qemu-la-rtc/qemu-system-loongarch64)
test-loongarch-fatal: prepare-la-tools
endif

$(LA_BUILD)/generated/init-config.h: force-init-config $(INIT_CONFIG_LA) tools/init-config.py
	python3 -B tools/init-config.py $(INIT_CONFIG_LA) $@
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
.PHONY: test-root-checksum-loongarch
test-root-checksum-loongarch: kernel-la $(LA_BUILD)/root-probe prepare-la-tools
	python3 -B tests/loongarch/root.py --qemu $(QEMU_LOONGARCH64) --cc $(LA_CC) --smoke --metadata-csum
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
.PHONY: test-rng-loongarch
$(LA_BUILD)/rng-probe: tests/userland/rng.c prepare-la-userland
	REALGCC=$(abspath $(LA_USER_CC)) $(LA_MUSL_CC) $(LA_FLAGS) -O2 -static -Wall -Wextra -Werror -Wl,-z,max-page-size=16384 -o $@ $<
test-rng-loongarch: kernel-la $(LA_BUILD)/rng-probe prepare-la-tools prepare-la-linux-platform
	python3 -B tests/rng_runner.py --arch loongarch $(TEST_MEMORY_ARGS)
$(LA_BUILD)/rng-fail-%.o: tests/loongarch/rng_failures.c arch/loongarch/build.mk
	$(LA_CC) $(LA_CPPFLAGS) $(LA_CFLAGS) -DRNG_FAIL_AFTER=$* -c $< -o $@
$(LA_BUILD)/kernel-rng-fail-%: $(LA_OBJECTS) $(LA_BUILD)/rng-fail-%.o arch/loongarch/linker.ld
	$(LA_CC) $(LA_FLAGS) -nostdlib -nostartfiles -static -no-pie -T arch/loongarch/linker.ld -Wl,--build-id=none,--gc-sections,--wrap=physical_page_allocate,--wrap=physical_page_allocate_order,--wrap=kernel_syscall_dispatch,--wrap=virtio_rng_start -o $@ $(LA_OBJECTS) $(LA_BUILD)/rng-fail-$*.o -lgcc
.PHONY: test-rng-failures-loongarch
test-rng-failures-loongarch: $(foreach case,0 1 2 3,$(LA_BUILD)/kernel-rng-fail-$(case)) $(LA_BUILD)/rng-probe prepare-la-tools prepare-la-linux-platform
	@for case in 0 1 2 3; do python3 -B tests/rng_runner.py --arch loongarch --platform BoarOS --mode a --force-device --kernel $(LA_BUILD)/kernel-rng-fail-$$case --marker 'LA RNG construction DMA/task/stack/IRQ rollback passed' || exit; done
test-la-userland-host: prepare-la-userland
	python3 -B tests/host/la_userland_cache.py

.PHONY: prepare-la-glibc test-glibc-loongarch test-glibc-profile-host
prepare-la-glibc:
	python3 -B -c "import sys; sys.path.insert(0,'tests/userland/glibc'); from profiles import checked_inputs; checked_inputs('loongarch')"
test-glibc-profile-host:
	python3 -B tests/host/glibc_profile.py
test-glibc-loongarch: kernel-la prepare-la-glibc prepare-la-tools prepare-la-linux prepare-la-userland
	python3 -B tests/userland/glibc/run.py --arch loongarch
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

$(LA_BUILD)/permissions-probe: tests/loongarch/permissions.c prepare-la-userland
	REALGCC=$(abspath $(LA_USER_CC)) $(LA_MUSL_CC) $(LA_FLAGS) -O2 -static -Wall -Wextra -Werror -Wl,-z,max-page-size=16384 -o $@ $<
.PHONY: test-permissions-loongarch
test-permissions-loongarch: kernel-la $(LA_BUILD)/permissions-probe prepare-la-tools prepare-la-linux
	python3 -B tests/loongarch/userland.py --qemu $(QEMU_LOONGARCH64) --cc $(LA_CC) --program $(LA_BUILD)/permissions-probe --marker 'LA permissions cold/resident/exec/fork/uaccess/revoke passed'

.PHONY: prepare-la-dynamic
prepare-la-dynamic:
	python3 -B tests/loongarch/prepare_dynamic.py --cross $(LA_CROSS_COMPILE)
.PHONY: test-la-dynamic-host
test-la-dynamic-host: prepare-la-dynamic
	python3 -B tests/host/la_dynamic_cache.py
LA_DYNAMIC_ROOT := $(LA_BUILD)/dynamic-dp-v2/root
LA_DP_FLAGS := -march=loongarch64 -mabi=lp64d -mdouble-float -mno-lsx -mno-lasx -mcmodel=normal
LA_DYNAMIC_CC := $(LA_DYNAMIC_ROOT)/bin/musl-gcc
LA_DYNAMIC_LINK := -Wl,--dynamic-linker=/lib/ld-musl-loongarch64.so.1 -Wl,-z,max-page-size=16384
LA_DYNAMIC_FILES := --file /lib/ld-musl-loongarch64.so.1=$(LA_DYNAMIC_ROOT)/lib/libc.so --file /lib/libboaros-tls.so=$(LA_BUILD)/tls-dso.so
$(LA_BUILD)/dynamic-probe: tests/loongarch/dynamic.c tests/loongarch/pthread.c tests/userland/pthread.c tests/loongarch/registers.S prepare-la-dynamic
	REALGCC=$(LA_CC) $(LA_DYNAMIC_CC) $(LA_DP_FLAGS) -O2 -pthread -fPIE -pie $(LA_DYNAMIC_LINK) -o $@ tests/loongarch/dynamic.c tests/loongarch/registers.S -ldl
$(LA_BUILD)/dynamic-exec-probe: tests/loongarch/dynamic.c tests/loongarch/pthread.c tests/userland/pthread.c tests/loongarch/registers.S prepare-la-dynamic
	REALGCC=$(LA_CC) $(LA_DYNAMIC_CC) $(LA_DP_FLAGS) -O2 -pthread -fno-pie -no-pie $(LA_DYNAMIC_LINK) -o $@ tests/loongarch/dynamic.c tests/loongarch/registers.S -ldl
$(LA_BUILD)/tls-dso.so: tests/userland/tls_dso.c prepare-la-dynamic
	REALGCC=$(LA_CC) $(LA_DYNAMIC_CC) $(LA_DP_FLAGS) -O2 -fPIC -shared -Wl,-z,max-page-size=16384 -Wl,-soname,libboaros-tls.so -o $@ $<
.PHONY: test-dynamic-loongarch
test-dynamic-loongarch: kernel-la $(LA_BUILD)/dynamic-probe $(LA_BUILD)/dynamic-exec-probe $(LA_BUILD)/tls-dso.so $(LA_BUILD)/dso-link-probe prepare-la-tools prepare-la-linux
	python3 -B tests/loongarch/userland.py --qemu $(QEMU_LOONGARCH64) --cc $(LA_CC) --program $(LA_BUILD)/dynamic-probe $(LA_DYNAMIC_FILES) --marker 'LA dynamic DSO TLS passed' --marker 'LA static pthread catalogue passed' --marker 'LA original BusyBox ash signal/wait passed'
	python3 -B tests/loongarch/userland.py --qemu $(QEMU_LOONGARCH64) --cc $(LA_CC) --program $(LA_BUILD)/dynamic-exec-probe $(LA_DYNAMIC_FILES) --marker 'LA dynamic DSO TLS passed' --marker 'LA static pthread catalogue passed' --marker 'LA original BusyBox ash signal/wait passed'
	python3 -B tests/loongarch/userland.py --qemu $(QEMU_LOONGARCH64) --cc $(LA_CC) --program $(LA_BUILD)/dso-link-probe $(LA_DYNAMIC_FILES) --marker 'LA DT_NEEDED/RPATH/initial DSO TLS passed'
$(LA_BUILD)/dso-link-probe: tests/loongarch/dso_link.c $(LA_BUILD)/tls-dso.so prepare-la-dynamic
	REALGCC=$(LA_CC) $(LA_DYNAMIC_CC) $(LA_DP_FLAGS) -O2 -pthread -fPIE -pie $(LA_DYNAMIC_LINK) -Wl,-rpath,'$$ORIGIN/lib' -L$(LA_BUILD) -Wl,-z,relro,-z,now -o $@ $< -l:tls-dso.so

$(LA_BUILD)/fpu-probe: tests/loongarch/fpu.c tests/loongarch/fp_registers.S tests/loongarch/registers.S prepare-la-dynamic
	REALGCC=$(LA_CC) $(LA_DYNAMIC_CC) $(LA_DP_FLAGS) -O2 -static -pthread -Wall -Wextra -Werror -Wl,-z,max-page-size=16384 -o $@ tests/loongarch/fpu.c tests/loongarch/fp_registers.S tests/loongarch/registers.S
.PHONY: test-fpu-loongarch
test-fpu-loongarch: kernel-la $(LA_BUILD)/fpu-probe prepare-la-tools prepare-la-linux
	python3 -B tests/loongarch/userland.py --qemu $(QEMU_LOONGARCH64) --cc $(LA_CC) --program $(LA_BUILD)/fpu-probe --marker 'LA FPU arithmetic/fenv/fork/signal passed' --marker 'LA FPU registers/FCC/timer/exec passed' --marker 'LA FPU exception signal passed' --marker 'LA FPU extension/badframe/pending/cross-page END passed'

$(LA_BUILD)/simd-probe: tests/loongarch/simd.c tests/loongarch/simd_registers.S prepare-la-dynamic
	REALGCC=$(LA_CC) $(LA_DYNAMIC_CC) $(LA_DP_FLAGS) -O2 -static -pthread -Wall -Wextra -Werror -Wl,-z,max-page-size=16384 -o $@ tests/loongarch/simd.c tests/loongarch/simd_registers.S
.PHONY: test-simd-loongarch
test-simd-loongarch: kernel-la $(LA_BUILD)/simd-probe prepare-la-tools prepare-la-linux
	python3 -B tests/loongarch/userland.py --program $(LA_BUILD)/simd-probe --marker 'LA SIMD first-use/all lanes/timer/signal/fork/exec/HWCAP passed'
	python3 -B tests/loongarch/userland.py --program $(LA_BUILD)/simd-probe --cpu la464,lasx=off --marker 'LA SIMD first-use/all lanes/timer/signal/fork/exec/HWCAP passed'
	python3 -B tests/loongarch/userland.py --program $(LA_BUILD)/simd-probe --cpu la464,lasx=off,lsx=off --marker 'LA SIMD first-use/all lanes/timer/signal/fork/exec/HWCAP passed'

$(LA_BUILD)/exec-errors-probe: tests/loongarch/exec_errors.c prepare-la-userland
	REALGCC=$(abspath $(LA_USER_CC)) $(LA_MUSL_CC) $(LA_FLAGS) -O2 -static -pthread -Wall -Wextra -Werror -Wl,-z,max-page-size=16384 -o $@ $<
.PHONY: test-exec-errors-loongarch
test-exec-errors-loongarch: kernel-la $(LA_BUILD)/exec-errors-probe $(LA_BUILD)/dynamic-probe $(LA_BUILD)/kernel-root-oom-1 prepare-la-tools prepare-la-linux
	python3 -B tests/loongarch/exec_failures.py --qemu $(QEMU_LOONGARCH64) --cc $(LA_CC)

$(LA_BUILD)/generated/net-config.h: force-net-config
	@mkdir -p $(dir $@)
	@printf '#define BOAROS_NET_IPV4 %sU\n#define BOAROS_NET_NETMASK %sU\n' '$(NET_IPV4)' '$(NET_NETMASK)' > $@.tmp
	@cmp -s $@ $@.tmp && rm $@.tmp || mv $@.tmp $@
$(LA_BUILD)/net/ethernet.o: $(LA_BUILD)/generated/net-config.h
$(LA_BUILD)/net/ethernet.o: LA_CPPFLAGS += -I$(LA_BUILD)/generated

.PHONY: test-network-loongarch
test-network-loongarch: kernel-la prepare-la-userland prepare-la-linux-platform prepare-la-tools
	python3 -B tests/network-loongarch.py --workload contract $(TEST_MEMORY_ARGS)
	python3 -B tests/network-loongarch.py --workload content $(TEST_MEMORY_ARGS)
	python3 -B tests/network-loongarch.py --workload timer $(TEST_MEMORY_ARGS)
	python3 -B tests/network-loongarch.py --workload admission $(TEST_MEMORY_ARGS)
	python3 -B tests/network-loongarch.py --workload sendfile $(TEST_MEMORY_ARGS)
	python3 -B tests/network-loongarch.py --workload budget $(TEST_MEMORY_ARGS)
	python3 -B tests/network-loongarch.py --workload unix_sender $(TEST_MEMORY_ARGS)
$(LA_BUILD)/net-failure-%.o: tests/loongarch/net_failures.c
	$(LA_CC) $(LA_CPPFLAGS) $(LA_CFLAGS) -DNET_FAIL_CASE=$* -c $< -o $@
$(LA_BUILD)/kernel-net-failure-%: $(LA_OBJECTS) $(LA_BUILD)/net-failure-%.o arch/loongarch/linker.ld
	$(LA_CC) $(LA_FLAGS) -nostdlib -nostartfiles -static -no-pie -T arch/loongarch/linker.ld -Wl,--build-id=none,--gc-sections,--wrap=physical_page_allocate,--wrap=physical_page_allocate_order,--wrap=kernel_syscall_dispatch,--wrap=virtio_net_init,--wrap=kernel_network_start,--wrap=kernel_heap_allocate_zeroed,--wrap=kernel_thread_create_joinable,--wrap=virtio_transport_reset -o $@ $(LA_OBJECTS) $(LA_BUILD)/net-failure-$*.o -lgcc
.PHONY: test-net-failures-loongarch test-network-external-loongarch
$(LA_BUILD)/network-contract: tests/workloads/network/contract.c prepare-la-userland
	REALGCC=$(abspath $(LA_USER_CC)) $(LA_MUSL_CC) $(LA_FLAGS) -O2 -static -Wall -Wextra -Werror -Wl,-z,max-page-size=16384 -o $@ $<
test-net-failures-loongarch: $(foreach case,0 1 2 3 4 5 6 7 8,$(LA_BUILD)/kernel-net-failure-$(case)) $(LA_BUILD)/network-contract prepare-la-tools
	python3 -B tests/loongarch/net_failures.py
test-network-external-loongarch: kernel-la prepare-la-userland prepare-la-linux-platform prepare-la-tools
	python3 -B tests/network-external.py --arch loongarch

$(LA_BUILD)/uart-failure-%.o: tests/loongarch/uart_failures.c
	$(LA_CC) $(LA_CPPFLAGS) $(LA_CFLAGS) -DUART_FAIL_CASE=$* -c $< -o $@
$(LA_BUILD)/kernel-uart-failure-%: $(LA_OBJECTS) $(LA_BUILD)/uart-failure-%.o arch/loongarch/linker.ld
	$(LA_CC) $(LA_FLAGS) -nostdlib -nostartfiles -static -no-pie -T arch/loongarch/linker.ld -Wl,--build-id=none,--gc-sections,--wrap=physical_page_allocate,--wrap=physical_page_allocate_order,--wrap=kernel_syscall_dispatch,--wrap=ns16550_start,--wrap=kernel_heap_allocate_zeroed,--wrap=kernel_thread_create_joinable -o $@ $(LA_OBJECTS) $(LA_BUILD)/uart-failure-$*.o -lgcc
$(LA_BUILD)/tty-probe: tests/tty/probe.c $(LA_BUILD)/musl-root/bin/musl-gcc
	REALGCC=$(CURDIR)/$(LA_BUILD)/gcc-sf/root/bin/loongarch64-unknown-linux-gnusf-gcc $(LA_BUILD)/musl-root/bin/musl-gcc -static -O2 -Wall -Wextra -Werror $< -o $@
$(LA_BUILD)/tty-jobctrl: tests/tty/jobctrl_probe.c $(LA_BUILD)/musl-root/bin/musl-gcc
	REALGCC=$(CURDIR)/$(LA_BUILD)/gcc-sf/root/bin/loongarch64-unknown-linux-gnusf-gcc $(LA_BUILD)/musl-root/bin/musl-gcc -static -O2 -Wall -Wextra -Werror $< -o $@
$(LA_BUILD)/tty-termios2-probe: tests/tty/termios2_probe.c $(LA_BUILD)/musl-root/bin/musl-gcc
	REALGCC=$(CURDIR)/$(LA_BUILD)/gcc-sf/root/bin/loongarch64-unknown-linux-gnusf-gcc $(LA_BUILD)/musl-root/bin/musl-gcc -static -O2 -Wall -Wextra -Werror $< -o $@
.PHONY: test-uart-failures-loongarch test-tty-loongarch test-tty-diff-loongarch test-tty-termios2-loongarch test-pty-loongarch test-pty-apps-loongarch
test-uart-failures-loongarch: $(foreach case,0 1 2 3 4 5,$(LA_BUILD)/kernel-uart-failure-$(case)) $(LA_BUILD)/network-contract
	python3 -B tests/loongarch/uart_failures.py
test-tty-loongarch: kernel-la
	python3 -B tests/tty/riscv.py --arch loongarch $(TEST_MEMORY_ARGS)
test-tty-diff-loongarch: kernel-la $(LA_BUILD)/tty-probe $(LA_BUILD)/tty-jobctrl
	python3 -B tests/tty/riscv.py --arch loongarch --probe $(LA_BUILD)/tty-probe $(TEST_MEMORY_ARGS)
	python3 -B tests/tty/riscv.py --arch loongarch --probe $(LA_BUILD)/tty-jobctrl --no-ctty $(TEST_MEMORY_ARGS)
test-tty-termios2-loongarch: kernel-la $(LA_BUILD)/tty-termios2-probe
	python3 -B tests/tty/riscv.py --arch loongarch --probe $(LA_BUILD)/tty-termios2-probe $(TEST_MEMORY_ARGS)
test-pty-loongarch: kernel-la
	python3 -B tests/tty/pty_riscv.py --arch loongarch --case core $(TEST_MEMORY_ARGS)
test-pty-apps-loongarch: kernel-la
	python3 -B tests/tty/pty_riscv.py --arch loongarch --case libc $(TEST_MEMORY_ARGS)
	python3 -B tests/tty/pty_riscv.py --arch loongarch --case script $(TEST_MEMORY_ARGS)

.PHONY: test-rtc-loongarch-host
test-rtc-loongarch-host:
	@mkdir -p build/host
	cc -std=c11 -O1 -g -Wall -Wextra -Werror -idirafter include -fsanitize=address,undefined tests/host/loongarch_rtc.c platform/loongarch_rtc.c -o build/host/loongarch-rtc
	build/host/loongarch-rtc

.PHONY: test-environment-loongarch
test-environment-loongarch: kernel-la
	python3 -B tests/environment.py --arch loongarch $(TEST_MEMORY_ARGS)

$(LA_BUILD)/pipe-geometry: tests/workloads/pipe_geometry.c $(LA_BUILD)/musl-root/bin/musl-gcc
	REALGCC=$(CURDIR)/$(LA_BUILD)/gcc-sf/root/bin/loongarch64-unknown-linux-gnusf-gcc $(LA_BUILD)/musl-root/bin/musl-gcc -static -O2 -Wall -Wextra -Werror $< -o $@
.PHONY: test-pipe-loongarch
test-pipe-loongarch: kernel-la $(LA_BUILD)/pipe-geometry
	python3 -B tests/loongarch/userland.py --program $(LA_BUILD)/pipe-geometry --marker 'PIPE GEOMETRY PASS'

LA_SQLITE_SOURCE := $(LA_BUILD)/sqlite/sqlite-amalgamation-3530400/sqlite3.c
LA_SQLITE_CC := $(LA_BUILD)/dynamic-dp-v2/root/bin/musl-gcc
LA_SQLITE_FLAGS := -march=loongarch64 -mabi=lp64d -mdouble-float -mno-lsx -mno-lasx -Wl,-z,max-page-size=16384
$(LA_SQLITE_SOURCE): $(SQLITE_ARCHIVE)
	@mkdir -p $(LA_BUILD)/sqlite
	unzip -oq $< -d $(LA_BUILD)/sqlite
	@test -f $@ && test -f $(dir $@)/shell.c
	@touch $@ $(dir $@)/shell.c
$(LA_BUILD)/sqlite-rollback: tests/workloads/sqlite/rollback.c $(LA_SQLITE_SOURCE) $(LA_SQLITE_CC)
	$(LA_SQLITE_CC) $(LA_SQLITE_FLAGS) -static -O2 -pthread -I$(dir $(LA_SQLITE_SOURCE)) -o $@ $< $(LA_SQLITE_SOURCE) -ldl
$(LA_BUILD)/sqlite-wal: tests/workloads/sqlite/wal.c $(LA_SQLITE_SOURCE) $(LA_SQLITE_CC)
	$(LA_SQLITE_CC) $(LA_SQLITE_FLAGS) -static -O2 -pthread -I$(dir $(LA_SQLITE_SOURCE)) -o $@ $< $(LA_SQLITE_SOURCE) -ldl
$(LA_BUILD)/sqlite3-static: $(LA_SQLITE_SOURCE) $(LA_SQLITE_CC)
	$(LA_SQLITE_CC) $(LA_SQLITE_FLAGS) -static -O2 -pthread -o $@ $(dir $(LA_SQLITE_SOURCE))/shell.c $(LA_SQLITE_SOURCE) -ldl
$(LA_BUILD)/sqlite3-dynamic: $(LA_SQLITE_SOURCE) $(LA_SQLITE_CC)
	$(LA_SQLITE_CC) $(LA_SQLITE_FLAGS) -fPIE -pie -O2 -pthread -Wl,--dynamic-linker=/lib/ld-musl-loongarch64.so.1 -o $@ $(dir $(LA_SQLITE_SOURCE))/shell.c $(LA_SQLITE_SOURCE) -ldl
$(LA_BUILD)/sqlite-cli-init: tests/workloads/sqlite/cli_init.c $(LA_SQLITE_CC)
	$(LA_SQLITE_CC) $(LA_SQLITE_FLAGS) -static -O2 -Wall -Wextra -Werror -o $@ $<
.PHONY: test-sqlite-wal-loongarch
test-sqlite-wal-loongarch: $(LA_BUILD)/sqlite-wal kernel-la
	python3 -B tests/sqlite-wal-riscv.py --arch loongarch --kernel kernel-la --program $(LA_BUILD)/sqlite-wal $(TEST_MEMORY_ARGS)
.PHONY: test-sqlite-rollback-loongarch
test-sqlite-rollback-loongarch: $(LA_BUILD)/sqlite-rollback $(LA_BUILD)/sqlite3-static $(LA_BUILD)/sqlite3-dynamic $(LA_BUILD)/sqlite-cli-init kernel-la
	python3 -B tests/sqlite-rollback.py --arch loongarch $(TEST_MEMORY_ARGS)

$(LA_BUILD)/sync-%.o: tests/sync/native.c
	@mkdir -p $(dir $@)
	$(LA_CC) $(LA_CPPFLAGS) $(LA_CFLAGS) -DBOAROS_SYNC_CASE=$* -c $< -o $@
$(LA_BUILD)/sync-%: $(LA_OBJECTS) $(LA_BUILD)/sync-%.o arch/loongarch/linker.ld
	$(LA_CC) $(LA_FLAGS) -nostdlib -nostartfiles -static -no-pie -T arch/loongarch/linker.ld -Wl,--gc-sections,--wrap=physical_page_allocate,--wrap=physical_page_allocate_order,--wrap=kernel_syscall_dispatch,--wrap=la_boot_tasks -o $@ $(LA_OBJECTS) $(LA_BUILD)/sync-$*.o -lgcc
.PHONY: test-sync-loongarch
test-sync-loongarch: $(foreach case,0 1 2 3 4,$(LA_BUILD)/sync-$(case)) prepare-la-tools
	python3 -B tests/sync/native.py --arch loongarch --qemu $(QEMU_LOONGARCH64) --kernel-dir $(LA_BUILD)

$(LA_BUILD)/wait-native.o: tests/wait/native.c
	@mkdir -p $(dir $@)
	$(LA_CC) $(LA_CPPFLAGS) $(LA_CFLAGS) -c $< -o $@
$(LA_BUILD)/wait-native: $(LA_OBJECTS) $(LA_BUILD)/wait-native.o arch/loongarch/linker.ld
	$(LA_CC) $(LA_FLAGS) -nostdlib -nostartfiles -static -no-pie -T arch/loongarch/linker.ld -Wl,--gc-sections,--wrap=physical_page_allocate,--wrap=physical_page_allocate_order,--wrap=kernel_syscall_dispatch,--wrap=la_boot_tasks,--wrap=kernel_wait_backend_switch -o $@ $(LA_OBJECTS) $(LA_BUILD)/wait-native.o -lgcc
.PHONY: test-wait-loongarch
test-wait-loongarch: $(LA_BUILD)/wait-native prepare-la-tools
	python3 -B tests/wait/native.py --arch loongarch --qemu $(QEMU_LOONGARCH64) --kernel-dir $(LA_BUILD)
