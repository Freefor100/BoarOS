LA_CROSS_COMPILE ?= loongarch64-unknown-linux-gnu-
QEMU_LOONGARCH64 ?= build/qemu-la/qemu-system-loongarch64
LA_BUILD := build/loongarch
LA_CC := $(LA_CROSS_COMPILE)gcc
LA_FLAGS := -march=loongarch64 -mabi=lp64s -msoft-float -mno-lsx -mno-lasx -mcmodel=normal
LA_CPPFLAGS := -Iinclude -DBOAROS_ARCH_LOONGARCH=1 -DBOAROS_PAGE_SHIFT=14 -DBOAROS_COST_DIAGNOSTICS=0 -DBOAROS_UTS_MACHINE=\"loongarch64\"
LA_CFLAGS := $(LA_FLAGS) -std=gnu11 -O2 -g3 -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -ffunction-sections -fdata-sections -Wall -Wextra -Werror -fstack-usage
LA_C_SOURCES := arch/loongarch/main.c platform/loongarch_virt.c kernel/dtb.c kernel/physical_page.c lib/string.c
LA_ASM_SOURCES := arch/loongarch/boot.S
LA_OBJECTS = $(patsubst %.c,$(LA_BUILD)/%.o,$(LA_C_SOURCES)) $(patsubst %.S,$(LA_BUILD)/%.o,$(LA_ASM_SOURCES))
$(LA_BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(LA_CC) $(LA_CPPFLAGS) $(LA_CFLAGS) -MMD -MP -c $< -o $@
$(LA_BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(LA_CC) $(LA_CPPFLAGS) $(LA_FLAGS) -g3 -c $< -o $@
kernel-la: $(LA_OBJECTS) arch/loongarch/linker.ld
	$(LA_CC) $(LA_FLAGS) -nostdlib -nostartfiles -static -no-pie -T arch/loongarch/linker.ld -Wl,--build-id=none,--gc-sections -Wl,-Map,$(LA_BUILD)/kernel.map -o $@ $(LA_OBJECTS) -lgcc
.PHONY: run-loongarch test-loongarch test-loongarch-boot
run-loongarch: kernel-la
	$(QEMU_LOONGARCH64) -machine virt -cpu la464 -smp 1 -m 1G -kernel $< -nographic -no-reboot
test-loongarch: kernel-la
	python3 -B tests/loongarch/run.py --qemu $(QEMU_LOONGARCH64)
test-loongarch-boot: kernel-la
	python3 -B tests/loongarch/run.py --qemu $(QEMU_LOONGARCH64) --stage boot
-include $(LA_OBJECTS:.o=.d)
