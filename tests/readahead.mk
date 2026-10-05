include Makefile
READAHEAD_KERNEL := $(BUILD_DIR)/tests/kernel-readahead-rv
READAHEAD_OBJECTS := $(TEST_RUNTIME_OBJECTS) $(BUILD_DIR)/kernel/dtb.o $(BUILD_DIR)/tests/riscv/readahead_main.o
-include $(BUILD_DIR)/tests/riscv/readahead_main.d
$(READAHEAD_KERNEL): $(READAHEAD_OBJECTS) arch/riscv/linker.ld
	$(CC) $(LDFLAGS) -Wl,--wrap=kernel_vfs_node_pread_batch -o $@ $(READAHEAD_OBJECTS)
