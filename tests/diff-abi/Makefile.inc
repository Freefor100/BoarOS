DIFF_ABI_RV := build/diff-abi/cases-rv
DIFF_ABI_SOURCES := tests/diff-abi/path_only.c tests/diff-abi/sched_stat.c tests/diff-abi/sched_policy.c tests/diff-abi/rt_controls.c tests/diff-abi/session.c tests/diff-abi/random.c tests/diff-abi/coarse.c tests/diff-abi/cases.c tests/diff-abi/environment.c tests/diff-abi/child_tid.c tests/diff-abi/statx.c tests/diff-abi/exec_script.c tests/diff-abi/mknod.c tests/diff-abi/proc.c tests/diff-abi/tmpfs.c tests/diff-abi/tmpfs_options.c tests/diff-abi/start.S tests/diff-abi/truncate.c tests/diff-abi/timestamps.c tests/diff-abi/readv.c tests/diff-abi/links.c tests/diff-abi/hardlink.c tests/diff-abi/signals.c tests/diff-abi/limits.c tests/diff-abi/devices.c tests/diff-abi/sync.c tests/diff-abi/namespace.c tests/diff-abi/metadata.c tests/diff-abi/ownership.c tests/diff-abi/robust.c tests/diff-abi/shared_mapping.c tests/diff-abi/futex_shared.c tests/diff-abi/file_locks.c tests/diff-abi/access.c tests/diff-abi/mode.c tests/diff-abi/socket.c tests/diff-abi/shm.c

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
		--kernel $(KERNEL_RV) --program $(DIFF_ABI_RV) \
		$(if $(DIFF_ABI_LINUX_KERNEL),--linux-kernel $(DIFF_ABI_LINUX_KERNEL)) $(TEST_MEMORY_ARGS)

# Focused path ABI uses the same record and syscall implementation.
build/diff-abi/path-rv: $(DIFF_ABI_SOURCES) tests/diff-abi/abi.h tests/riscv/user_elf.ld
	@mkdir -p $(@D)
	$(CC) -DABI_PATH_ONLY -Wno-unused-function -march=rv64gc -mabi=lp64d -O2 -Wall -Wextra -Werror \
		-ffreestanding -fno-builtin -fno-stack-protector -nostdlib -static \
		-Wl,--build-id=none -Wl,-T,tests/riscv/user_elf.ld -Wl,--defsym,__global_pointer\$$=0 \
		-o $@ $(DIFF_ABI_SOURCES)

build/diff-abi/sync-rv: $(DIFF_ABI_SOURCES) tests/diff-abi/abi.h tests/riscv/user_elf.ld
	@mkdir -p $(@D)
	$(CC) -DABI_SYNC_ONLY -Wno-unused-function -march=rv64gc -mabi=lp64d -O2 -Wall -Wextra -Werror \
		-ffreestanding -fno-builtin -fno-stack-protector -nostdlib -static \
		-Wl,--build-id=none -Wl,-T,tests/riscv/user_elf.ld -Wl,--defsym,__global_pointer\$$=0 \
		-o $@ $(DIFF_ABI_SOURCES)

build/loongarch/diff-abi-cases: $(DIFF_ABI_SOURCES) tests/diff-abi/abi.h tests/common/raw_syscall.h tests/common/user.ld
	@mkdir -p $(@D)
	$(LA_CC) $(LA_FLAGS) -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -nostartfiles -static -no-pie -Wl,--build-id=none -Wl,-z,max-page-size=16384 -Wl,-T,tests/common/user.ld -o $@ $(DIFF_ABI_SOURCES)
.PHONY: test-diff-abi-loongarch
test-diff-abi-loongarch: test-diff-abi-host kernel-la build/loongarch/diff-abi-cases prepare-la-tools prepare-la-linux-platform
	python3 -B tests/diff-abi/harness.py --arch loongarch --kernel kernel-la --program build/loongarch/diff-abi-cases --timeout 180 $(TEST_MEMORY_ARGS)

build/diff-abi/sched-handoff-rv: $(DIFF_ABI_SOURCES) tests/diff-abi/sched_handoff.c tests/diff-abi/abi.h tests/riscv/user_elf.ld
	@mkdir -p $(@D)
	$(CC) $(ARCH_FLAGS) -DABI_SCHED_HANDOFF_ONLY -Wno-unused-function -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -static -Wl,--build-id=none -Wl,-T,tests/riscv/user_elf.ld -Wl,--defsym,__global_pointer\$$=0 -o $@ $(DIFF_ABI_SOURCES) tests/diff-abi/sched_handoff.c
build/loongarch/sched-handoff-la: $(DIFF_ABI_SOURCES) tests/diff-abi/sched_handoff.c tests/diff-abi/abi.h tests/common/user.ld
	@mkdir -p $(@D)
	$(LA_CC) $(LA_FLAGS) -DABI_SCHED_HANDOFF_ONLY -Wno-unused-function -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -nostartfiles -static -no-pie -Wl,--build-id=none -Wl,-z,max-page-size=16384 -Wl,-T,tests/common/user.ld -o $@ $(DIFF_ABI_SOURCES) tests/diff-abi/sched_handoff.c
.PHONY: test-scheduler-handoff-riscv test-scheduler-handoff-loongarch
test-scheduler-handoff-riscv: $(KERNEL_RV) build/diff-abi/sched-handoff-rv
	python3 -B tests/diff-abi/harness.py --arch riscv --kernel $(KERNEL_RV) --program build/diff-abi/sched-handoff-rv --case-manifest tests/diff-abi/sched-handoff-cases.txt --memory 512M --memory 1G --timeout 60 --output build/sched-handoff-rv
test-scheduler-handoff-loongarch: kernel-la build/loongarch/sched-handoff-la prepare-la-tools prepare-la-linux-platform
	python3 -B tests/diff-abi/harness.py --arch loongarch --kernel kernel-la --program build/loongarch/sched-handoff-la --case-manifest tests/diff-abi/sched-handoff-cases.txt --memory 512M --memory 1G --timeout 60 --output build/sched-handoff-la
