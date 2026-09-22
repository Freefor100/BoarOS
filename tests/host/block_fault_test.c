#define _POSIX_C_SOURCE 200809L
#include "block_fault.h"
#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    char path[] = "/tmp/boaros-powercut-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0 && ftruncate(fd, 4096) == 0);
    close(fd);
    struct fault_block disk;
    assert(fault_block_open(&disk, path) == 0);
    unsigned char a[512], b[512], out[512];
    memset(a, 0x35, sizeof(a));
    memset(b, 0x72, sizeof(b));
    assert(kernel_block_write_at(&disk.device, 0, a, 512) == KERNEL_BLOCK_STATUS_OK);
    assert(kernel_block_read_at(&disk.device, 0, out, 512) == KERNEL_BLOCK_STATUS_OK);
    assert(memcmp(a, out, 512) == 0);
    assert(fault_block_crash(&disk) == 0);
    assert(kernel_block_read_at(&disk.device, 0, out, 512) == KERNEL_BLOCK_STATUS_OK);
    for (size_t i = 0; i < 512; i++) assert(out[i] == 0);
    assert(kernel_block_write_at(&disk.device, 0, a, 512) == KERNEL_BLOCK_STATUS_OK);
    assert(kernel_block_write_at(&disk.device, 512, b, 512) == KERNEL_BLOCK_STATUS_OK);
    /* A later sector may reach stable storage while an earlier one is lost. */
    assert(fault_block_persist(&disk, 1) == 0);
    assert(fault_block_crash(&disk) == 0);
    assert(kernel_block_read_at(&disk.device, 512, out, 512) == KERNEL_BLOCK_STATUS_OK);
    assert(memcmp(b, out, 512) == 0);
    disk.fail_write = disk.writes + 1;
    assert(kernel_block_write_at(&disk.device, 0, a, 512) == KERNEL_BLOCK_STATUS_IO);
    disk.fail_write = 0;
    assert(kernel_block_write_at(&disk.device, 0, a, 512) == KERNEL_BLOCK_STATUS_OK);
    disk.fail_flush = disk.flushes + 1;
    assert(kernel_block_flush(&disk.device) == KERNEL_BLOCK_STATUS_IO);
    assert(fault_block_crash(&disk) == 0);
    assert(kernel_block_read_at(&disk.device, 0, out, 512) == KERNEL_BLOCK_STATUS_OK);
    for (size_t i = 0; i < 512; i++) assert(out[i] == 0);
    disk.fail_flush = 0;
    assert(kernel_block_write_at(&disk.device, 0, a, 512) == KERNEL_BLOCK_STATUS_OK);
    assert(kernel_block_flush(&disk.device) == KERNEL_BLOCK_STATUS_OK);
    fault_block_close(&disk);
    assert(fault_block_open(&disk, path) == 0);
    assert(kernel_block_read_at(&disk.device, 0, out, 512) == KERNEL_BLOCK_STATUS_OK);
    assert(memcmp(a, out, 512) == 0);
    fault_block_close(&disk);
    unlink(path);
    puts("volatile block writes, power cuts, reorder and flush faults passed");
}
