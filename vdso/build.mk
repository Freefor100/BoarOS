# The freestanding compiler produces PIC objects. Linking ET_DYN requires
# a target linker with shared-ELF support (the RV bare-metal ld lacks it).
RV_VDSO_LD ?= $(or $(shell command -v riscv64-linux-gnu-ld),$(shell command -v riscv64-linux-musl-ld),$(wildcard /opt/riscv64-linux-musl-cross/bin/riscv64-linux-musl-ld),riscv64-linux-gnu-ld)
LA_VDSO_LD ?= $(LA_CROSS_COMPILE)ld

define vdso_build
$(1)/vdso/time.o: vdso/time.c include/kernel/vdso.h vdso/build.mk
	@mkdir -p $$(dir $$@)
	$(2) $(3) -Iinclude -Os -fPIC -ffreestanding -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -c $$< -o $$@
$(1)/vdso/sigreturn.o: vdso/sigreturn.S vdso/build.mk
	@mkdir -p $$(dir $$@)
	$(2) $(3) -fPIC -c $$< -o $$@
$(1)/vdso/image.lds: vdso/image.lds.S include/kernel/vdso.h vdso/build.mk
	@mkdir -p $$(dir $$@)
	$(2) $(3) -Iinclude -DBOAROS_PAGE_SHIFT=$(4) -E -P -x assembler-with-cpp $$< -o $$@
$(1)/vdso/linux-vdso.so.1: $(1)/vdso/time.o $(1)/vdso/sigreturn.o $(1)/vdso/image.lds
	$(5) -shared -Bsymbolic --no-undefined --hash-style=both -s -soname linux-vdso.so.1 -z max-page-size=$(6) -T $(1)/vdso/image.lds -o $$@ $(1)/vdso/sigreturn.o $(1)/vdso/time.o
$(1)/arch/$(7)/signal_trampoline.o: arch/$(7)/signal_trampoline.S $(1)/vdso/linux-vdso.so.1 vdso/build.mk
	@mkdir -p $$(dir $$@)
	$(2) $(3) -DBOAROS_PAGE_SHIFT=$(4) -DBOAROS_VDSO_IMAGE=\"$(1)/vdso/linux-vdso.so.1\" -c $$< -o $$@
endef

$(eval $(call vdso_build,$(BUILD_DIR),$(CC),$(ARCH_FLAGS),12,$(RV_VDSO_LD),4096,riscv))
$(eval $(call vdso_build,$(LA_BUILD),$(LA_CC),$(LA_FLAGS),14,$(LA_VDSO_LD),16384,loongarch))
