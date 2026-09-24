DIFF_ABI_RV := build/diff-abi/cases-rv
DIFF_ABI_SOURCES := tests/diff-abi/cases.c tests/diff-abi/start.S tests/diff-abi/truncate.c tests/diff-abi/timestamps.c tests/diff-abi/readv.c tests/diff-abi/links.c tests/diff-abi/signals.c tests/diff-abi/limits.c tests/diff-abi/devices.c tests/diff-abi/sync.c tests/diff-abi/namespace.c tests/diff-abi/metadata.c tests/diff-abi/robust.c tests/diff-abi/shared_mapping.c

$(DIFF_ABI_RV): $(DIFF_ABI_SOURCES) tests/diff-abi/abi.h tests/riscv/user_elf.ld
	@mkdir -p $(@D)
	$(CC) -march=rv64gc -mabi=lp64d -O2 -Wall -Wextra -Werror \
		-ffreestanding -fno-builtin -fno-stack-protector -nostdlib -static \
		-Wl,--build-id=none -Wl,-T,tests/riscv/user_elf.ld -Wl,--defsym,__global_pointer\$$=0 \
		-o $@ $(DIFF_ABI_SOURCES)

.PHONY: test-diff-abi-host test-diff-abi-riscv build-diff-abi-linux

test-diff-abi-host:
	PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests/diff-abi -p 'test_*.py'

build-diff-abi-linux:
	python3 tests/diff-abi/harness.py --build-linux

test-diff-abi-riscv: test-diff-abi-host $(KERNEL_RV) $(DIFF_ABI_RV)
	QEMU_RISCV64=$(QEMU_RISCV64) python3 tests/diff-abi/harness.py \
		--kernel $(KERNEL_RV) --program $(DIFF_ABI_RV)
