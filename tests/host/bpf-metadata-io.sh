#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
trap 'rm -f build/host/bpf/metadata-io/image-12.img build/host/bpf/metadata-io/image-14.img' EXIT
FILE_IO_HOST_TCP=0 \
FILE_IO_HOST_BUILD=build/host/bpf/metadata-io \
FILE_IO_HOST_TEST_SOURCE=tests/host/bpf_metadata_io.c \
FILE_IO_HOST_SOURCES='fs/bpffs.c fs/tracefs.c fs/sysfs.c' \
FILE_IO_HOST_SHIFTS="${BPF_HOST_SHIFTS:-12 14}" \
sh tests/host/file-io-concurrency.sh
