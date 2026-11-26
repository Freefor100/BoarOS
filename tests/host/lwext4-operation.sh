#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$root"
work=${EXT4_OPERATION_HOST_BUILD:-build/host/ext4-operation}
mkdir -p "$work"
${HOST_CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Werror \
    -Wno-unused-but-set-variable -Wno-stringop-truncation \
    -DBOAROS_MAX_CPUS=2 -DCONFIG_USE_DEFAULT_CFG=1 -DCONFIG_USE_USER_MALLOC=1 \
    -DTEST_OPERATION_LIFECYCLE -Wl,--wrap=jbd_journal_accept_state_held -Wl,--wrap=jbd_journal_free_trans_state_held -Wl,--wrap=ext4_operation_publish_super \
    -Wl,--wrap=ext4_fs_alloc_inode,--wrap=ext4_fs_get_inode_ref,--wrap=ext4_fs_put_inode_ref,--wrap=ext4_dir_find_entry,--wrap=ext4_dir_add_entry \
    -Wl,--wrap=jbd_trans_get_write_access_range,--wrap=jbd_trans_prepare_unowned_range \
    -include tests/host/lwext4_memory.h -Ithird_party/lwext4/include -idirafter include \
    third_party/lwext4/src/*.c tests/host/lwext4_metadata.c \
    tests/host/block_fault.c kernel/block.c tests/host/block_runtime.c kernel/raw_lock.c \
    tests/host/sync/notifications.c -Itests/host/block_model -pthread -o "$work/probe"
nojournal_symlink_cases() {
    block=$1
    for fault in 1 2 3; do
        image="$work/nojournal-symlink-$block-$fault.img"
        truncate -s 32M "$image"
        mkfs.ext4 -q -F -b "$block" -O ^has_journal "$image"
        log="$work/nojournal-symlink-$block-$fault"
        "$work/probe" "$image" nojournal-symlink-fault "$fault" > "$log.log"
        cat "$log.log"
        if [ "$fault" -eq 1 ]; then
            e2fsck -fn "$image" > "$log-fsck.log" 2>&1
            rm "$image"
            continue
        fi
        inode=$(sed -n 's/.*inode=\([0-9]*\) retained.*/\1/p' "$log.log")
        test -n "$inode"
        "$work/probe" "$image" nojournal-reject
        "$work/probe" "$image" nojournal-orphan-reopen
        LC_ALL=C debugfs -R "testi <$inode>" "$image" > "$log-inode-bitmap.log" 2>&1
        grep -F "Inode $inode is marked in use" "$log-inode-bitmap.log"
        LC_ALL=C debugfs -R 'stat /ordered-link' "$image" > "$log-path.log" 2>&1
        if [ "$fault" -eq 2 ]; then
            grep -F "File not found" "$log-path.log"
        else
            grep -E "^Inode: $inode[[:space:]]+Type: symlink" "$log-path.log"
            grep -E 'User:[[:space:]]+70001[[:space:]]+Group:[[:space:]]+80002' "$log-path.log"
            grep -F 'Links: 1 ' "$log-path.log"
            LC_ALL=C debugfs -R 'cat /ordered-link' "$image" > "$log-target" 2> "$log-target.log"
            test "$(cat "$log-target")" = long-symlink-target-with-its-own-data-block-and-a-detectable-write-payload
        fi
        status=0
        LC_ALL=C e2fsck -fn "$image" > "$log-fsck.log" 2>&1 || status=$?
        if [ "$fault" -eq 2 ]; then
            test "$status" -eq 4
            grep -F "Unattached inode $inode" "$log-fsck.log"
        else
            test "$status" -eq 0 || test "$status" -eq 4
        fi
        repaired="$log-repaired.img"
        cp "$image" "$repaired"
        status=0
        e2fsck -fy "$repaired" > "$log-repair.log" 2>&1 || status=$?
        test "$status" -eq 0 || test "$status" -eq 1
        "$work/probe" "$repaired" nojournal-clean-remount
        e2fsck -fn "$repaired" > "$log-repaired-fsck.log" 2>&1
        rm "$image" "$repaired"
        printf 'PASS symlink fault block=%s fault=%s; repaired image remounts and passes e2fsck\n' "$block" "$fault"
    done
}

if [ "${1:-}" = nojournal-symlink ]; then
    for block in 1024 4096; do nojournal_symlink_cases "$block"; done
    exit 0
fi
for block in 1024 4096; do
    nojournal_symlink_cases "$block"
    for journal in journal nonjournal; do
        image="$work/$block-$journal.img"
        features=has_journal
        [ "$journal" = journal ] || features=^has_journal
        truncate -s 32M "$image"
        mkfs.ext4 -q -F -b "$block" -g 1024 -I 256 -O "$features" "$image"
        "$work/probe" "$image" unlink-ref
        if [ "$journal" = journal ]; then
            "$work/probe" "$image" create-ref
            for fault in 0 1; do
                failed="$work/state-error-$block-$fault.img"
                cp "$image" "$failed"
                "$work/probe" "$failed" operation-state-error "$fault"
                "$work/probe" "$failed" operation-context
                e2fsck -fn "$failed" > "$work/state-error-$block-$fault-fsck.log" 2>&1
            done
            "$work/probe" "$image" operation-context
            "$work/probe" "$image" operation-lifecycle
            "$work/probe" "$image" operation-super-delta
            "$work/probe" "$image" handle-return
            "$work/probe" "$image" handle-stale
            "$work/probe" "$image" seed
            "$work/probe" "$image" owner-shared-set
        fi
        "$work/probe" "$image" allocation-locality
        log="$work/fsck-$block-$journal.log"
        e2fsck -fn "$image" > "$log" 2>&1 || { cat "$log" >&2; exit 1; }
        if LC_ALL=C grep -Eq 'wrong|Fix\? no|UNEXPECTED INCONSISTENCY|corrupt' "$log"; then
            cat "$log" >&2
            exit 1
        fi
        printf 'PASS operation ownership, super counters and allocation locality: block=%s %s; e2fsck clean\n' "$block" "$journal"
    done
done
for journal in journal nonjournal; do
    image="$work/4096-two-groups-$journal.img"
    features=has_journal
    [ "$journal" = journal ] || features=^has_journal
    truncate -s 256M "$image"
    mkfs.ext4 -q -F -b 4096 -I 256 -O "$features" "$image"
    "$work/probe" "$image" allocation-locality
    e2fsck -fn "$image" > "$work/fsck-two-groups-$journal.log" 2>&1 || {
        cat "$work/fsck-two-groups-$journal.log" >&2; exit 1;
    }
    printf 'PASS real two-group directory placement: %s; e2fsck clean\n' "$journal"
done
