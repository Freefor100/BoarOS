LA_CROSS_COMPILE ?= loongarch64-unknown-linux-gnu-
QEMU_LOONGARCH64 ?= build/qemu-la/qemu-system-loongarch64
LA_BUILD := build/loongarch
LA_CC := $(LA_CROSS_COMPILE)gcc
LA_FLAGS := -march=loongarch64 -mabi=lp64s -msoft-float -mno-lsx -mno-lasx -mcmodel=normal
LA_CPPFLAGS := -Iinclude -DBOAROS_ARCH_LOONGARCH=1 -DBOAROS_PAGE_SHIFT=14 -DBOAROS_COST_DIAGNOSTICS=0 -DBOAROS_UTS_MACHINE=\"loongarch64\"
LA_CFLAGS := $(LA_FLAGS) -std=gnu11 -O2 -g3 -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -ffunction-sections -fdata-sections -Wall -Wextra -Werror -fstack-usage
LA_C_SOURCES := $(filter-out arch/% kernel/main.c net/ethernet.c,$(C_SOURCES)) \
    arch/loongarch/main.c arch/loongarch/mmu.c arch/loongarch/context.c \
    arch/loongarch/timer.c arch/loongarch/trap.c platform/loongarch_virt.c \
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
	$(LA_CC) $(LA_FLAGS) -nostdlib -nostartfiles -static -no-pie -T arch/loongarch/linker.ld -Wl,--build-id=none,--gc-sections,--wrap=physical_page_allocate,--wrap=physical_page_allocate_order,--wrap=kernel_syscall_dispatch,--wrap=la_user_contract -o $@ $(LA_OBJECTS) $< -lgcc
.PHONY: test-loongarch-fatal
test-loongarch-fatal: $(LA_BUILD)/kernel-fatal-1 $(LA_BUILD)/kernel-fatal-2 $(LA_BUILD)/kernel-fatal-3
	python3 -B tests/loongarch/fatal.py --qemu $(QEMU_LOONGARCH64)
ifeq ($(QEMU_LOONGARCH64),build/qemu-la/qemu-system-loongarch64)
test-loongarch-fatal: prepare-la-tools
endif
