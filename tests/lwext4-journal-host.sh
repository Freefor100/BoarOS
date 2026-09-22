#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d)
trap 'result=$?; if [ "$result" -eq 0 ]; then rm -rf "$work"; else echo "test artifacts retained: $work" >&2; fi' EXIT
trap 'exit 1' HUP INT TERM
cc=${HOST_CC:-cc}
"$cc" -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-but-set-variable \
    -Wno-stringop-truncation -DCONFIG_USE_DEFAULT_CFG=1 ${JOURNAL_TEST_CFLAGS:-} \
    -I"$root/third_party/lwext4/include" -idirafter "$root/include" \
    "$root"/third_party/lwext4/src/*.c "$root/tests/host/lwext4_journal.c" \
    "$root/tests/host/block_fault.c" "$root/kernel/block.c" -o "$work/probe"
truncate -s 32M "$work/base.img"
mkfs.ext4 -q -F -b 1024 "$work/base.img"
printf 'Old data' > "$work/data"
debugfs -w -R "write $work/data /probe" "$work/base.img" >/dev/null 2>&1
cp "$work/base.img" "$work/commit.img"
cp "$work/base.img" "$work/abort-fresh.img"
timeout 30 "$work/probe" "$work/abort-fresh.img" abort-fresh
timeout 30 "$work/probe" "$work/abort-fresh.img" recover-crash 79
e2fsck -fn "$work/abort-fresh.img" >/dev/null
timeout 30 "$work/probe" "$work/commit.img" commit
timeout 30 "$work/probe" "$work/commit.img" recover-crash 78
e2fsck -fn "$work/commit.img" >/dev/null
printf '%s\n' 'PASS: durable journal commit survives loss of home writes'

for stage in 1 2; do
    cp "$work/base.img" "$work/error.img"
    timeout 30 "$work/probe" "$work/error.img" error "$stage"
    timeout 30 "$work/probe" "$work/error.img" recover-crash 79
    e2fsck -fn "$work/error.img" >/dev/null
done
for action in checkpoint checkpoint-error; do
    cp "$work/base.img" "$work/checkpoint.img"
    timeout 30 "$work/probe" "$work/checkpoint.img" "$action" 1
    timeout 30 "$work/probe" "$work/checkpoint.img" recover-crash 78
    e2fsck -fn "$work/checkpoint.img" >/dev/null
done
for action in recover-write-error recover-flush-error; do
    cp "$work/base.img" "$work/recover.img"
    timeout 30 "$work/probe" "$work/recover.img" commit
    timeout 30 "$work/probe" "$work/recover.img" "$action"
    timeout 30 "$work/probe" "$work/recover.img" recover-crash 78
    e2fsck -fn "$work/recover.img" >/dev/null
done
printf '%s\n' 'PASS: precommit/postcommit errors, checkpoint ownership, replay errors'

for action in write-error checkpoint-error checkpoint-flush-error; do
    stages='1 2'
    [ "$action" != write-error ] || stages='1 2 3'
    for stage in $stages; do
        cp "$work/base.img" "$work/error.img"
        timeout 30 "$work/probe" "$work/error.img" "$action" "$stage"
        expected=78
        [ "$action" != write-error ] || expected=79
        timeout 30 "$work/probe" "$work/error.img" recover-crash "$expected"
        e2fsck -fn "$work/error.img" >/dev/null
    done
done
cp "$work/base.img" "$work/wrap.img"
timeout 30 "$work/probe" "$work/wrap.img" wrap
timeout 30 "$work/probe" "$work/wrap.img" recover-crash 78
e2fsck -fn "$work/wrap.img" >/dev/null
cp "$work/base.img" "$work/no-flush.img"
timeout 30 "$work/probe" "$work/no-flush.img" no-flush
printf '%s\n' 'PASS: journal write/tail errors, bounded journal wrap, missing barrier rejection'

for action in oversize checksum2 checksum3 wide checksum2-wide checksum3-wide; do
    cp "$work/base.img" "$work/feature.img"
    timeout 30 "$work/probe" "$work/feature.img" "$action"
    expected=78
    [ "$action" != oversize ] || expected=79
    timeout 30 "$work/probe" "$work/feature.img" recover-crash "$expected"
    e2fsck -fn "$work/feature.img" >/dev/null
done

cp "$work/base.img" "$work/metadata.img"
timeout 30 "$work/probe" "$work/metadata.img" metadata
timeout 30 "$work/probe" "$work/metadata.img" recover-metadata 78
e2fsck -fn "$work/metadata.img" >/dev/null
for feature in commit checksum2 checksum3 checksum3-revoke checksum3-revoke64 \
               wide checksum2-wide checksum3-wide; do
    cp "$work/base.img" "$work/independent.img"
    timeout 30 "$work/probe" "$work/independent.img" "$feature"
    e2fsck -fy "$work/independent.img" > "$work/fsck.log" 2>&1 || {
        status=$?
        [ "$status" -le 1 ] || { cat "$work/fsck.log"; exit "$status"; }
    }
    timeout 30 "$work/probe" "$work/independent.img" recover 78
done

cp "$work/base.img" "$work/unsupported.img"
timeout 30 "$work/probe" "$work/unsupported.img" unsupported
printf '%s\n' 'PASS: checksummed journals, metadata cache replay, independent e2fsck replay'

cp "$work/base.img" "$work/abort.img"
timeout 30 "$work/probe" "$work/abort.img" abort-read-error
timeout 30 "$work/probe" "$work/abort.img" recover-crash 78
e2fsck -fn "$work/abort.img" >/dev/null
printf '%s\n' 'PASS: failed rollback read retains prior committed journal owner'

for format in checksum3-revoke checksum3-revoke64; do
for damage in descriptor-csum data-csum commit-csum revoke-csum \
              revoke-count-low revoke-count-high revoke-count-unaligned \
              descriptor-no-last descriptor-target; do
    cp "$work/base.img" "$work/corrupt.img"
    timeout 30 "$work/probe" "$work/corrupt.img" "$format"
    timeout 30 "$work/probe" "$work/corrupt.img" mutate "$damage"
    errno=5
    case "$damage" in revoke-count-*|descriptor-no-last|descriptor-target) errno=117 ;; esac
    timeout 30 "$work/probe" "$work/corrupt.img" recover-corrupt "$errno"
done
done
# A zero commit is a normal interrupted tail, even if its descriptor tore.
cp "$work/base.img" "$work/torn.img"
timeout 30 "$work/probe" "$work/torn.img" checksum3
timeout 30 "$work/probe" "$work/torn.img" mutate descriptor-csum
timeout 30 "$work/probe" "$work/torn.img" mutate commit-zero
timeout 30 "$work/probe" "$work/torn.img" recover-crash 79
cp "$work/base.img" "$work/partial-commit.img"
timeout 30 "$work/probe" "$work/partial-commit.img" checksum3
timeout 30 "$work/probe" "$work/partial-commit.img" mutate commit-padding
timeout 30 "$work/probe" "$work/partial-commit.img" recover-crash 78
printf '%s\n' 'PASS: committed corruption is retained, format bounds, interrupted tails'

# Linux's timestamp check recognizes an older stale suffix after a good commit.
cp "$work/base.img" "$work/stale.img"
timeout 30 "$work/probe" "$work/stale.img" checksum3-two
timeout 30 "$work/probe" "$work/stale.img" mutate commit-time-high
timeout 30 "$work/probe" "$work/stale.img" mutate second-commit-time-low
timeout 30 "$work/probe" "$work/stale.img" mutate second-descriptor-csum
timeout 30 "$work/probe" "$work/stale.img" recover-crash 78
# Also replay the valid prefix when the second transaction lacks a commit.
cp "$work/base.img" "$work/prefix.img"
timeout 30 "$work/probe" "$work/prefix.img" checksum3-two
timeout 30 "$work/probe" "$work/prefix.img" mutate second-descriptor-csum
timeout 30 "$work/probe" "$work/prefix.img" mutate second-commit-zero
timeout 30 "$work/probe" "$work/prefix.img" recover-crash 78
printf '%s\n' 'PASS: stale timestamps and committed prefix before interrupted suffix'

# A torn primary superblock needs an intact superblock in the committed log.
for record in empty commit super-record bad-super; do
    cp "$work/base.img" "$work/super.img"
    if [ "$record" != empty ]; then
        timeout 30 "$work/probe" "$work/super.img" "$record"
    fi
    python3 - "$work/super.img" <<'PYTHON'
import sys
with open(sys.argv[1], 'r+b') as image:
    image.seek(1024 + 1020)
    checksum = image.read(4)
    image.seek(1024 + 1020)
    image.write(bytes([checksum[0] ^ 1]) + checksum[1:])
PYTHON
    case "$record" in
        super-record) timeout 30 "$work/probe" "$work/super.img" recover-crash 78 ;;
        bad-super) timeout 30 "$work/probe" "$work/super.img" recover-corrupt 117 ;;
        *) timeout 30 "$work/probe" "$work/super.img" recover-no-super ;;
    esac
done
printf '%s\n' 'PASS: torn primary requires intact logged superblock, corrupt/missing records retained'

cp "$work/base.img" "$work/readonly-recovery.img"
timeout 30 "$work/probe" "$work/readonly-recovery.img" commit
timeout 30 "$work/probe" "$work/readonly-recovery.img" recover-readonly
printf '%s\n' 'PASS: read-only recovery refuses writes and retains log'
