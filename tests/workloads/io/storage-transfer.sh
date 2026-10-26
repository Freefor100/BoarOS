#!/busybox sh
set -eu
/busybox mkdir -p /slow
/busybox mknod /dev/slow b 252 16
/busybox mount -t ext4 /dev/slow /slow
for medium in fast slow; do
    if [ "$medium" = fast ]; then base=/data; else base=/slow/data; fi
    for work in cold4 cold64 append mixed; do
        echo "STORAGE TRANSFER medium=$medium case=$work"
        /data-path "/configs/$medium-$work" "$base/$work"
        /busybox cat "/configs/$medium-$work.results"
    done
done
/busybox sync
/busybox umount /slow
echo 'STORAGE TRANSFER PASS windows=8'
