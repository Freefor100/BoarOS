.PHONY: prepare-program-environment build-competition-busybox test-program-environment-host
prepare-program-environment: $(MUSL_STAMP)
	PATH="$(abspath build/diff-abi/tools/usr/bin):$$PATH" python3 tests/program-inventory/environment.py

build-competition-busybox: $(MUSL_STAMP) test-program-environment-host
	PATH="$(abspath build/diff-abi/tools/usr/bin):$$PATH" python3 tests/program-inventory/environment.py --busybox

test-program-environment-host:
	PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests/program-inventory -p test_environment.py

.PHONY: test-busybox-tmpfs-riscv
test-busybox-tmpfs-riscv: $(KERNEL_RV) $(MUSL_STAMP) build-competition-busybox
	PYTHONDONTWRITEBYTECODE=1 python3 tests/program-inventory/tmpfs.py \
		--kernel $(KERNEL_RV) --qemu $(QEMU_RISCV64) \
		$(if $(BUSYBOX_TMPFS_LINUX_KERNEL),--linux-kernel $(BUSYBOX_TMPFS_LINUX_KERNEL))
