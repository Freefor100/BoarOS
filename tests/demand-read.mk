include Makefile
DEMAND_READ_KERNEL := $(BUILD_DIR)/tests/kernel-demand-read-rv
DEMAND_READ_OBJECTS := $(TEST_RUNTIME_OBJECTS) $(BUILD_DIR)/kernel/dtb.o $(BUILD_DIR)/tests/riscv/demand_read_main.o
-include $(BUILD_DIR)/tests/riscv/demand_read_main.d
$(DEMAND_READ_KERNEL): $(DEMAND_READ_OBJECTS) arch/riscv/linker.ld
	$(CC) $(LDFLAGS) -Wl,--wrap=riscv_sv39_current_satp -Wl,--wrap=kernel_task_current \
		-Wl,--wrap=kernel_copy_to_user -Wl,--wrap=kernel_copy_from_user \
		-Wl,--wrap=kernel_uaccess_write_cursor_copy \
		-Wl,--wrap=kernel_vfs_node_pread_batch -o $@ $(DEMAND_READ_OBJECTS)
