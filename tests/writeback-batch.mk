include Makefile

WRITEBACK_BATCH_KERNEL := $(BUILD_DIR)/tests/kernel-writeback-batch-rv
WRITEBACK_BATCH_OBJECTS := $(TEST_RUNTIME_OBJECTS) $(BUILD_DIR)/kernel/dtb.o \
    $(BUILD_DIR)/tests/riscv/writeback_batch_main.o
-include $(BUILD_DIR)/tests/riscv/writeback_batch_main.d
$(WRITEBACK_BATCH_KERNEL): $(WRITEBACK_BATCH_OBJECTS) arch/riscv/linker.ld
	$(CC) $(LDFLAGS) -Wl,--wrap=kernel_vfs_node_writeback \
		-Wl,--wrap=physical_page_allocate_order -Wl,--wrap=physical_page_allocate \
		-o $@ $(WRITEBACK_BATCH_OBJECTS)
