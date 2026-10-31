#!/bin/sh
set -eu
FILE_IO_HOST_TCP=0 FILE_IO_HOST_SHIFTS=12 \
FILE_IO_HOST_BUILD=${MADVISE_REMOVE_HOST_BUILD:-build/host/mm/madvise-remove} \
FILE_IO_HOST_TEST_SOURCE=tests/host/mm_madvise_remove.c \
FILE_IO_HOST_SOURCES=mm/shm.c \
FILE_IO_HOST_FLAGS='-DMADVISE_REMOVE_CASES -DBOAROS_LOCK_DEBUG=1' \
FILE_IO_HOST_LDFLAGS=-Wl,--wrap=kernel_memory_object_prepare_page,--wrap=riscv_sv39_user_space_fork_prepare,--wrap=kernel_memory_object_remove,--wrap=kernel_open_file_fallocate,--wrap=kernel_memory_object_wait \
exec sh tests/file-io-concurrency-host.sh
