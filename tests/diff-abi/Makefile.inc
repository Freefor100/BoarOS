DIFF_ABI_RV := build/diff-abi/cases-rv
DIFF_ABI_SOURCES := tests/diff-abi/selected.c tests/diff-abi/sigreturn.c tests/diff-abi/fault_count.c tests/diff-abi/exec_boundaries.c tests/diff-abi/boundaries.c tests/diff-abi/path_only.c tests/diff-abi/sched_stat.c tests/diff-abi/sched_policy.c tests/diff-abi/rt_controls.c tests/diff-abi/session.c tests/diff-abi/random.c tests/diff-abi/coarse.c tests/diff-abi/cases.c tests/diff-abi/environment.c tests/diff-abi/child_tid.c tests/diff-abi/statx.c tests/diff-abi/exec_script.c tests/diff-abi/mknod.c tests/diff-abi/proc.c tests/diff-abi/tmpfs.c tests/diff-abi/tmpfs_options.c tests/diff-abi/start.S tests/diff-abi/truncate.c tests/diff-abi/timestamps.c tests/diff-abi/readv.c tests/diff-abi/links.c tests/diff-abi/hardlink.c tests/diff-abi/signals.c tests/diff-abi/limits.c tests/diff-abi/devices.c tests/diff-abi/sync.c tests/diff-abi/namespace.c tests/diff-abi/metadata.c tests/diff-abi/ownership.c tests/diff-abi/robust.c tests/diff-abi/shared_mapping.c tests/diff-abi/futex_shared.c tests/diff-abi/file_locks.c tests/diff-abi/access.c tests/diff-abi/mode.c tests/diff-abi/socket.c tests/diff-abi/shm.c tests/diff-abi/waitid.c tests/diff-abi/usage.c tests/diff-abi/cpu_clock.c tests/diff-abi/itimer.c

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

build/diff-abi/sched-handoff-rv-delayed: ABI_HANDOFF_FLAGS := -DABI_HANDOFF_DELAY_NS=20000000
build/diff-abi/sched-handoff-rv build/diff-abi/sched-handoff-rv-delayed: $(DIFF_ABI_SOURCES) tests/diff-abi/sched_handoff.c tests/diff-abi/abi.h tests/riscv/user_elf.ld tests/diff-abi/Makefile.inc
	@mkdir -p $(@D)
	$(CC) $(ARCH_FLAGS) -DABI_SCHED_HANDOFF_ONLY $(ABI_HANDOFF_FLAGS) -Wno-unused-function -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -static -Wl,--build-id=none -Wl,-T,tests/riscv/user_elf.ld -Wl,--defsym,__global_pointer\$$=0 -o $@ $(DIFF_ABI_SOURCES) tests/diff-abi/sched_handoff.c
build/loongarch/sched-handoff-la-delayed: ABI_HANDOFF_FLAGS := -DABI_HANDOFF_DELAY_NS=20000000
build/loongarch/sched-handoff-la build/loongarch/sched-handoff-la-delayed: $(DIFF_ABI_SOURCES) tests/diff-abi/sched_handoff.c tests/diff-abi/abi.h tests/common/user.ld tests/diff-abi/Makefile.inc
	@mkdir -p $(@D)
	$(LA_CC) $(LA_FLAGS) -DABI_SCHED_HANDOFF_ONLY $(ABI_HANDOFF_FLAGS) -Wno-unused-function -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -nostartfiles -static -no-pie -Wl,--build-id=none -Wl,-z,max-page-size=16384 -Wl,-T,tests/common/user.ld -o $@ $(DIFF_ABI_SOURCES) tests/diff-abi/sched_handoff.c
.PHONY: test-scheduler-handoff-riscv test-scheduler-handoff-loongarch
test-scheduler-handoff-riscv: $(KERNEL_RV) build/diff-abi/sched-handoff-rv build/diff-abi/sched-handoff-rv-delayed
	python3 -B tests/diff-abi/harness.py --arch riscv --kernel $(KERNEL_RV) --program build/diff-abi/sched-handoff-rv --case-manifest tests/diff-abi/sched-handoff-cases.txt --memory 512M --memory 1G --timeout 60 --output build/sched-handoff-rv
	python3 -B tests/diff-abi/harness.py --arch riscv --kernel $(KERNEL_RV) --program build/diff-abi/sched-handoff-rv-delayed --case-manifest tests/diff-abi/sched-handoff-cases.txt --memory 512M --memory 1G --timeout 60 --output build/sched-handoff-rv-delayed
test-scheduler-handoff-loongarch: kernel-la build/loongarch/sched-handoff-la build/loongarch/sched-handoff-la-delayed prepare-la-tools prepare-la-linux-platform
	python3 -B tests/diff-abi/harness.py --arch loongarch --kernel kernel-la --program build/loongarch/sched-handoff-la --case-manifest tests/diff-abi/sched-handoff-cases.txt --memory 512M --memory 1G --timeout 60 --output build/sched-handoff-la
	python3 -B tests/diff-abi/harness.py --arch loongarch --kernel kernel-la --program build/loongarch/sched-handoff-la-delayed --case-manifest tests/diff-abi/sched-handoff-cases.txt --memory 512M --memory 1G --timeout 60 --output build/sched-handoff-la-delayed

# Focused syscall boundaries use the same executable and records on Linux/BoarOS.
build/diff-abi/boundary-rv: $(DIFF_ABI_SOURCES) tests/diff-abi/abi.h tests/riscv/user_elf.ld
	@mkdir -p $(@D)
	$(CC) $(ARCH_FLAGS) -DABI_BOUNDARY_ONLY -Wno-unused-function -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -static -Wl,--build-id=none -Wl,-T,tests/riscv/user_elf.ld -Wl,--defsym,__global_pointer\$$=0 -o $@ $(DIFF_ABI_SOURCES)
build/loongarch/boundary-la: $(DIFF_ABI_SOURCES) tests/diff-abi/abi.h tests/common/user.ld
	@mkdir -p $(@D)
	$(LA_CC) $(LA_FLAGS) -DABI_BOUNDARY_ONLY -Wno-unused-function -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -nostartfiles -static -no-pie -Wl,--build-id=none -Wl,-z,max-page-size=16384 -Wl,-T,tests/common/user.ld -o $@ $(DIFF_ABI_SOURCES)
.PHONY: test-abi-boundary-riscv test-abi-boundary-loongarch
test-abi-boundary-riscv: $(KERNEL_RV) build/diff-abi/boundary-rv
	python3 -B tests/diff-abi/harness.py --arch riscv --kernel $(KERNEL_RV) --program build/diff-abi/boundary-rv --case-manifest tests/diff-abi/boundary-cases.txt --timeout 180 $(TEST_MEMORY_ARGS) --output build/abi-boundary-rv
test-abi-boundary-loongarch: kernel-la build/loongarch/boundary-la prepare-la-tools prepare-la-linux-platform
	python3 -B tests/diff-abi/harness.py --arch loongarch --kernel kernel-la --program build/loongarch/boundary-la --case-manifest tests/diff-abi/boundary-cases.txt --timeout 180 $(TEST_MEMORY_ARGS) --output build/abi-boundary-la

build/diff-abi/exec-boundary-rv: $(DIFF_ABI_SOURCES) tests/diff-abi/abi.h tests/riscv/user_elf.ld
	@mkdir -p $(@D)
	$(CC) $(ARCH_FLAGS) -DABI_EXEC_BOUNDARY_ONLY -Wno-unused-function -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -static -Wl,--build-id=none -Wl,-T,tests/riscv/user_elf.ld -Wl,--defsym,__global_pointer\$$=0 -o $@ $(DIFF_ABI_SOURCES)
build/loongarch/exec-boundary-la: $(DIFF_ABI_SOURCES) tests/diff-abi/abi.h tests/common/user.ld
	@mkdir -p $(@D)
	$(LA_CC) $(LA_FLAGS) -DABI_EXEC_BOUNDARY_ONLY -Wno-unused-function -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -nostartfiles -static -no-pie -Wl,--build-id=none -Wl,-z,max-page-size=16384 -Wl,-T,tests/common/user.ld -o $@ $(DIFF_ABI_SOURCES)

build/diff-abi/stack-rv-rwx: STACK_FLAGS := 7
build/diff-abi/stack-rv-rw: STACK_FLAGS := 6
build/diff-abi/stack-rv-absent: STACK_LD := tests/riscv/user_elf.ld
build/diff-abi/stack-rv-rwx build/diff-abi/stack-rv-rw: STACK_LD := tests/diff-abi/stack.ld
build/diff-abi/stack-rv-rwx build/diff-abi/stack-rv-rw build/diff-abi/stack-rv-absent: $(DIFF_ABI_SOURCES) tests/diff-abi/abi.h tests/diff-abi/stack.ld tests/riscv/user_elf.ld
	@mkdir -p $(@D)
	$(CC) $(ARCH_FLAGS) -DABI_STACK_ONLY -Wno-unused-function -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -static -Wl,--build-id=none -Wl,-T,$(STACK_LD) -Wl,--defsym=STACK_FLAGS=$(or $(STACK_FLAGS),0) -Wl,--defsym,__global_pointer\$$=0 -o $@ $(DIFF_ABI_SOURCES)
build/loongarch/stack-la-rwx: STACK_FLAGS := 7
build/loongarch/stack-la-rw: STACK_FLAGS := 6
build/loongarch/stack-la-absent: STACK_LD := tests/common/user.ld
build/loongarch/stack-la-rwx build/loongarch/stack-la-rw: STACK_LD := tests/diff-abi/stack.ld
build/loongarch/stack-la-rwx build/loongarch/stack-la-rw build/loongarch/stack-la-absent: $(DIFF_ABI_SOURCES) tests/diff-abi/abi.h tests/diff-abi/stack.ld tests/common/user.ld
	@mkdir -p $(@D)
	$(LA_CC) $(LA_FLAGS) -DABI_STACK_ONLY -Wno-unused-function -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -nostartfiles -static -no-pie -Wl,--build-id=none -Wl,-z,max-page-size=16384 -Wl,-T,$(STACK_LD) -Wl,--defsym=STACK_FLAGS=$(or $(STACK_FLAGS),0) -o $@ $(DIFF_ABI_SOURCES)
.PHONY: test-elf-boundary-riscv test-elf-boundary-loongarch
test-elf-boundary-riscv: $(KERNEL_RV) build/diff-abi/exec-boundary-rv build/diff-abi/stack-rv-rwx build/diff-abi/stack-rv-rw build/diff-abi/stack-rv-absent
	python3 -B tests/diff-abi/harness.py --arch riscv --kernel $(KERNEL_RV) --program build/diff-abi/exec-boundary-rv --case-manifest tests/diff-abi/exec-boundary-cases.txt --timeout 180 $(TEST_MEMORY_ARGS) --output build/exec-boundary-rv
	@for mode in rwx rw absent; do python3 -B tests/diff-abi/harness.py --arch riscv --kernel $(KERNEL_RV) --program build/diff-abi/stack-rv-$$mode --case-manifest tests/diff-abi/stack-cases.txt --timeout 180 $(TEST_MEMORY_ARGS) --output build/stack-elf-rv-$$mode || exit $$?; done
test-elf-boundary-loongarch: kernel-la build/loongarch/exec-boundary-la build/loongarch/stack-la-rwx build/loongarch/stack-la-rw build/loongarch/stack-la-absent prepare-la-tools prepare-la-linux-platform
	python3 -B tests/diff-abi/harness.py --arch loongarch --kernel kernel-la --program build/loongarch/exec-boundary-la --case-manifest tests/diff-abi/exec-boundary-cases.txt --timeout 180 $(TEST_MEMORY_ARGS) --output build/exec-boundary-la
	@for mode in rwx rw absent; do python3 -B tests/diff-abi/harness.py --arch loongarch --kernel kernel-la --program build/loongarch/stack-la-$$mode --case-manifest tests/diff-abi/stack-cases.txt --timeout 180 $(TEST_MEMORY_ARGS) --output build/stack-elf-la-$$mode || exit $$?; done

build/diff-abi/fault-count-rv: $(DIFF_ABI_SOURCES) tests/diff-abi/abi.h tests/riscv/user_elf.ld
	@mkdir -p $(@D)
	$(CC) $(ARCH_FLAGS) -DABI_FAULT_COUNT_ONLY -Wno-unused-function -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -static -Wl,--build-id=none -Wl,-T,tests/riscv/user_elf.ld -Wl,--defsym,__global_pointer\$$=0 -o $@ $(DIFF_ABI_SOURCES)
build/loongarch/fault-count-la: $(DIFF_ABI_SOURCES) tests/diff-abi/abi.h tests/common/user.ld
	@mkdir -p $(@D)
	$(LA_CC) $(LA_FLAGS) -DABI_FAULT_COUNT_ONLY -Wno-unused-function -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -nostartfiles -static -no-pie -Wl,--build-id=none -Wl,-z,max-page-size=16384 -Wl,-T,tests/common/user.ld -o $@ $(DIFF_ABI_SOURCES)
.PHONY: test-fault-count-riscv test-fault-count-loongarch
test-fault-count-riscv: $(KERNEL_RV) build/diff-abi/fault-count-rv
	python3 -B tests/diff-abi/harness.py --arch riscv --kernel $(KERNEL_RV) --program build/diff-abi/fault-count-rv --case-manifest tests/diff-abi/fault-count-cases.txt --timeout 180 $(TEST_MEMORY_ARGS) --output build/fault-count-rv
test-fault-count-loongarch: kernel-la build/loongarch/fault-count-la prepare-la-tools prepare-la-linux-platform
	python3 -B tests/diff-abi/harness.py --arch loongarch --kernel kernel-la --program build/loongarch/fault-count-la --case-manifest tests/diff-abi/fault-count-cases.txt --timeout 180 $(TEST_MEMORY_ARGS) --output build/fault-count-la

build/diff-abi/sigreturn-rv: $(DIFF_ABI_SOURCES) tests/diff-abi/abi.h tests/riscv/user_elf.ld
	@mkdir -p $(@D)
	$(CC) -march=rv64gc -mabi=lp64d -DABI_SIGRETURN_ONLY -Wno-unused-function -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -static -Wl,--build-id=none -Wl,-T,tests/riscv/user_elf.ld -Wl,--defsym,__global_pointer\$$=0 -o $@ $(DIFF_ABI_SOURCES)
build/loongarch/sigreturn-la: $(DIFF_ABI_SOURCES) tests/diff-abi/abi.h tests/common/user.ld
	@mkdir -p $(@D)
	$(LA_CC) $(LA_FLAGS) -DABI_SIGRETURN_ONLY -Wno-unused-function -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -nostartfiles -static -no-pie -Wl,--build-id=none -Wl,-z,max-page-size=16384 -Wl,-T,tests/common/user.ld -o $@ $(DIFF_ABI_SOURCES)
.PHONY: test-sigreturn-boundary-riscv test-sigreturn-boundary-loongarch
test-sigreturn-boundary-riscv: $(KERNEL_RV) build/diff-abi/sigreturn-rv
	python3 -B tests/diff-abi/harness.py --arch riscv --kernel $(KERNEL_RV) --program build/diff-abi/sigreturn-rv --case-manifest tests/diff-abi/sigreturn-cases.txt --timeout 180 $(TEST_MEMORY_ARGS) --output build/sigreturn-boundary-rv
test-sigreturn-boundary-loongarch: kernel-la build/loongarch/sigreturn-la prepare-la-tools prepare-la-linux-platform
	python3 -B tests/diff-abi/harness.py --arch loongarch --kernel kernel-la --program build/loongarch/sigreturn-la --case-manifest tests/diff-abi/sigreturn-cases.txt --timeout 180 $(TEST_MEMORY_ARGS) --output build/sigreturn-boundary-la

# Approved ABI delivery regressions, using the identical ELF on Linux and BoarOS.
build/diff-abi/selected-rv: $(DIFF_ABI_SOURCES) tests/diff-abi/abi.h tests/riscv/user_elf.ld
	@mkdir -p $(@D)
	$(CC) -march=rv64gc -mabi=lp64d -DABI_SELECTED_ONLY -Wno-unused-function -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -static -Wl,--build-id=none -Wl,-T,tests/riscv/user_elf.ld -Wl,--defsym,__global_pointer\$$=0 -o $@ $(DIFF_ABI_SOURCES)
build/loongarch/selected-la: $(DIFF_ABI_SOURCES) tests/diff-abi/abi.h tests/common/user.ld
	@mkdir -p $(@D)
	$(LA_CC) $(LA_FLAGS) -DABI_SELECTED_ONLY -Wno-unused-function -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -nostartfiles -static -no-pie -Wl,--build-id=none -Wl,-z,max-page-size=16384 -Wl,-T,tests/common/user.ld -o $@ $(DIFF_ABI_SOURCES)
.PHONY: test-selected-abi-riscv test-selected-abi-loongarch
test-selected-abi-riscv: $(KERNEL_RV) build/diff-abi/selected-rv
	python3 -B tests/diff-abi/harness.py --arch riscv --kernel $(KERNEL_RV) --program build/diff-abi/selected-rv --case-manifest tests/diff-abi/selected-cases.txt --timeout 180 $(TEST_MEMORY_ARGS) --output build/selected-abi-rv
test-selected-abi-loongarch: kernel-la build/loongarch/selected-la prepare-la-tools prepare-la-linux-platform
	python3 -B tests/diff-abi/harness.py --arch loongarch --kernel kernel-la --program build/loongarch/selected-la --case-manifest tests/diff-abi/selected-cases.txt --timeout 180 $(TEST_MEMORY_ARGS) --output build/selected-abi-la

build/diff-abi/waitid-rv: $(DIFF_ABI_SOURCES) tests/diff-abi/abi.h tests/riscv/user_elf.ld
	@mkdir -p $(@D)
	$(CC) -march=rv64gc -mabi=lp64d -DABI_WAITID_ONLY -Wno-unused-function -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -static -Wl,--build-id=none -Wl,-T,tests/riscv/user_elf.ld -Wl,--defsym,__global_pointer\$$=0 -o $@ $(DIFF_ABI_SOURCES)
build/loongarch/waitid-la: $(DIFF_ABI_SOURCES) tests/diff-abi/abi.h tests/common/user.ld
	@mkdir -p $(@D)
	$(LA_CC) $(LA_FLAGS) -DABI_WAITID_ONLY -Wno-unused-function -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -nostartfiles -static -no-pie -Wl,--build-id=none -Wl,-z,max-page-size=16384 -Wl,-T,tests/common/user.ld -o $@ $(DIFF_ABI_SOURCES)
.PHONY: test-waitid-abi-riscv test-waitid-abi-loongarch
test-waitid-abi-riscv: $(KERNEL_RV) build/diff-abi/waitid-rv
	python3 -B tests/diff-abi/harness.py --arch riscv --kernel $(KERNEL_RV) --program build/diff-abi/waitid-rv --case-manifest tests/diff-abi/waitid-cases.txt --timeout 180 $(TEST_MEMORY_ARGS) --output build/waitid-abi-rv
test-waitid-abi-loongarch: kernel-la build/loongarch/waitid-la prepare-la-tools prepare-la-linux-platform
	python3 -B tests/diff-abi/harness.py --arch loongarch --kernel kernel-la --program build/loongarch/waitid-la --case-manifest tests/diff-abi/waitid-cases.txt --timeout 180 $(TEST_MEMORY_ARGS) --output build/waitid-abi-la

# Resource and CPU-clock queries share the same raw syscall case pack.
build/diff-abi/resource-rv: USAGE_CASE_FLAGS := -DABI_RESOURCE_ONLY
build/diff-abi/usage-rv build/diff-abi/resource-rv: $(DIFF_ABI_SOURCES) tests/diff-abi/abi.h tests/riscv/user_elf.ld
	@mkdir -p $(@D)
	$(CC) -march=rv64gc -mabi=lp64d -DABI_USAGE_ONLY $(USAGE_CASE_FLAGS) -Wno-unused-function -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -static -Wl,--build-id=none -Wl,-T,tests/riscv/user_elf.ld -Wl,--defsym,__global_pointer\$$=0 -o $@ $(DIFF_ABI_SOURCES)
build/loongarch/resource-la: USAGE_CASE_FLAGS := -DABI_RESOURCE_ONLY
build/loongarch/usage-la build/loongarch/resource-la: $(DIFF_ABI_SOURCES) tests/diff-abi/abi.h tests/common/user.ld
	@mkdir -p $(@D)
	$(LA_CC) $(LA_FLAGS) -DABI_USAGE_ONLY $(USAGE_CASE_FLAGS) -Wno-unused-function -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -nostartfiles -static -no-pie -Wl,--build-id=none -Wl,-z,max-page-size=16384 -Wl,-T,tests/common/user.ld -o $@ $(DIFF_ABI_SOURCES)

.PHONY: test-resource-abi-riscv test-resource-abi-loongarch test-resource-smp-riscv
test-resource-abi-riscv: $(KERNEL_RV) build/diff-abi/resource-rv
	python3 -B tests/diff-abi/harness.py --kernel $(KERNEL_RV) --program build/diff-abi/resource-rv --case-manifest tests/diff-abi/resource-cases.txt --timeout 90 $(TEST_MEMORY_ARGS) --output build/resource-abi-rv
test-resource-abi-loongarch: kernel-la build/loongarch/resource-la prepare-la-tools prepare-la-linux-platform
	python3 -B tests/diff-abi/harness.py --arch loongarch --kernel kernel-la --program build/loongarch/resource-la --case-manifest tests/diff-abi/resource-cases.txt --timeout 90 $(TEST_MEMORY_ARGS) --output build/resource-abi-la
test-resource-smp-riscv: build/diff-abi/resource-rv
	$(MAKE) kernel-rv-user-smp
	python3 -B tests/smp/user.py --kernel build/riscv-user-smp/kernel-rv --program build/diff-abi/resource-rv --manifest tests/diff-abi/resource-cases.txt --require-parallel --firmware $(QEMU_RISCV_FIRMWARE) $(TEST_MEMORY_ARGS)

.PHONY: test-usage-abi-riscv test-usage-abi-loongarch test-usage-smp-riscv test-cpu-clock-process-smp-riscv
test-usage-abi-riscv: $(KERNEL_RV) build/diff-abi/usage-rv
	python3 -B tests/diff-abi/harness.py --kernel $(KERNEL_RV) --program build/diff-abi/usage-rv --case-manifest tests/diff-abi/usage-cases.txt --timeout 90 $(TEST_MEMORY_ARGS) --output build/usage-abi-rv
test-usage-abi-loongarch: kernel-la build/loongarch/usage-la prepare-la-tools prepare-la-linux-platform
	python3 -B tests/diff-abi/harness.py --arch loongarch --kernel kernel-la --program build/loongarch/usage-la --case-manifest tests/diff-abi/usage-cases.txt --timeout 90 $(TEST_MEMORY_ARGS) --output build/usage-abi-la
test-usage-smp-riscv: build/diff-abi/usage-rv
	$(MAKE) kernel-rv-user-smp
	python3 -B tests/smp/user.py --kernel build/riscv-user-smp/kernel-rv --program build/diff-abi/usage-rv --manifest tests/diff-abi/usage-cases.txt --require-parallel --firmware $(QEMU_RISCV_FIRMWARE) $(TEST_MEMORY_ARGS)
test-cpu-clock-process-smp-riscv: build/riscv-user-smp-composition/composition-rv test-user-smp-composition-host
	python3 -B tests/smp/composition.py --scene cpu-clock --firmware $(QEMU_RISCV_FIRMWARE) --program $(COMPOSITION_ELF) $(TEST_MEMORY_ARGS)

# Process CPU interval timers, including actual group/exec ownership.
build/diff-abi/itimer-rv: $(DIFF_ABI_SOURCES) tests/diff-abi/abi.h tests/riscv/user_elf.ld
	@mkdir -p $(@D)
	$(CC) -march=rv64gc -mabi=lp64d -DABI_ITIMER_ONLY -Wno-unused-function -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -static -Wl,--build-id=none -Wl,-T,tests/riscv/user_elf.ld -Wl,--defsym,__global_pointer\$$=0 -o $@ $(DIFF_ABI_SOURCES)
build/loongarch/itimer-la: $(DIFF_ABI_SOURCES) tests/diff-abi/abi.h tests/common/user.ld
	@mkdir -p $(@D)
	$(LA_CC) $(LA_FLAGS) -DABI_ITIMER_ONLY -Wno-unused-function -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -nostartfiles -static -no-pie -Wl,--build-id=none -Wl,-z,max-page-size=16384 -Wl,-T,tests/common/user.ld -o $@ $(DIFF_ABI_SOURCES)
.PHONY: test-itimer-abi-riscv test-itimer-abi-loongarch test-itimer-smp-riscv
test-itimer-abi-riscv: $(KERNEL_RV) build/diff-abi/itimer-rv
	python3 -B tests/diff-abi/harness.py --kernel $(KERNEL_RV) --program build/diff-abi/itimer-rv --case-manifest tests/diff-abi/itimer-cases.txt --timeout 30 $(TEST_MEMORY_ARGS) --output build/itimer-abi-rv
test-itimer-abi-loongarch: kernel-la build/loongarch/itimer-la prepare-la-tools prepare-la-linux-platform
	python3 -B tests/diff-abi/harness.py --arch loongarch --kernel kernel-la --program build/loongarch/itimer-la --case-manifest tests/diff-abi/itimer-cases.txt --timeout 30 $(TEST_MEMORY_ARGS) --output build/itimer-abi-la
test-itimer-smp-riscv: build/diff-abi/itimer-rv
	$(MAKE) kernel-rv-user-smp
	python3 -B tests/smp/user.py --kernel build/riscv-user-smp/kernel-rv --program build/diff-abi/itimer-rv --manifest tests/diff-abi/itimer-cases.txt --require-parallel --firmware $(QEMU_RISCV_FIRMWARE) $(TEST_MEMORY_ARGS)

build/diff-abi/signal-wait-rv: $(DIFF_ABI_SOURCES) tests/diff-abi/abi.h tests/riscv/user_elf.ld
	@mkdir -p $(@D)
	$(CC) -march=rv64gc -mabi=lp64d -DABI_SIGNAL_WAIT_ONLY -Wno-unused-function -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -static -Wl,--build-id=none -Wl,-T,tests/riscv/user_elf.ld -Wl,--defsym,__global_pointer\$$=0 -o $@ $(DIFF_ABI_SOURCES)
build/loongarch/signal-wait-la: $(DIFF_ABI_SOURCES) tests/diff-abi/abi.h tests/common/user.ld
	@mkdir -p $(@D)
	$(LA_CC) $(LA_FLAGS) -DABI_SIGNAL_WAIT_ONLY -Wno-unused-function -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -nostartfiles -static -no-pie -Wl,--build-id=none -Wl,-z,max-page-size=16384 -Wl,-T,tests/common/user.ld -o $@ $(DIFF_ABI_SOURCES)

.PHONY: test-signal-wait-abi-riscv test-signal-wait-abi-loongarch
test-signal-wait-abi-riscv: $(KERNEL_RV) build/diff-abi/signal-wait-rv
	python3 -B tests/diff-abi/harness.py --kernel $(KERNEL_RV) --program build/diff-abi/signal-wait-rv --case-manifest tests/diff-abi/signal-wait-cases.txt --timeout 30 $(TEST_MEMORY_ARGS) --output build/signal-wait-abi-rv
test-signal-wait-abi-loongarch: kernel-la build/loongarch/signal-wait-la prepare-la-tools prepare-la-linux-platform
	python3 -B tests/diff-abi/harness.py --arch loongarch --kernel kernel-la --program build/loongarch/signal-wait-la --case-manifest tests/diff-abi/signal-wait-cases.txt --timeout 30 $(TEST_MEMORY_ARGS) --output build/signal-wait-abi-la
