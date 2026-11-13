#!/busybox sh
set -e
BB=/busybox
work=/tmp/busybox-inode-flags
$BB mkdir "$work" "$work/memory"
check_tree() {
    base=$1
    label=$2
    $BB mkdir "$base/case"
    file="$base/case/file"
    $BB printf 'before' > "$file"
    $BB chattr +d "$file"
    listing=$($BB lsattr -d "$file")
    set -- $listing
    case "$1" in *d*) ;; *) exit 1;; esac
    $BB printf 'after' > "$file"
    test "$($BB cat "$file")" = after
    $BB chattr +a "$file"
    $BB printf '+append' >> "$file"
    test "$($BB cat "$file")" = after+append
    if $BB sh -c 'printf overwrite > "$1"' unused "$file" 2>/dev/null; then exit 1; fi
    if $BB rm -f "$file" 2>/dev/null; then exit 1; fi
    $BB chattr -a "$file"
    $BB chattr +i "$file"
    listing=$($BB lsattr -d "$file")
    set -- $listing
    case "$1" in *i*) ;; *) exit 1;; esac
    if $BB sh -c 'printf overwrite > "$1"' unused "$file" 2>/dev/null; then exit 1; fi
    if $BB rm -f "$file" 2>/dev/null; then exit 1; fi
    $BB chattr -i "$file"
    $BB chattr -d "$file"
    $BB rm "$file"
    $BB rmdir "$base/case"
    echo "BUSYBOX ATTR $label PASS"
}
check_tree "$work" ext4
$BB mount -t tmpfs none "$work/memory"
check_tree "$work/memory" tmpfs
$BB umount "$work/memory"
$BB rmdir "$work/memory" "$work"
echo 'BUSYBOX ATTR PASS'
