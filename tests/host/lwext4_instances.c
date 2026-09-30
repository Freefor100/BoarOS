/* Independent mount names, callbacks, journal owners and lifecycle. */
#include "block_fault.h"
#include <ext4.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); exit(1); } } while (0)
struct fixture {
    struct fault_block disk;
    struct ext4_blockdev_iface iface;
    struct ext4_blockdev device;
    struct ext4_lock locks;
    unsigned depth, acquisitions;
    unsigned char scratch[512];
};
static int dev_open(struct ext4_blockdev *dev) { (void)dev; return EOK; }
static int dev_read(struct ext4_blockdev *dev, void *p, uint64_t b, uint32_t n)
{ struct fixture *f = dev->bdif->p_user; return kernel_block_read_at(&f->disk.device, b*512, p, n*512) == 0 ? EOK : EIO; }
static int dev_write(struct ext4_blockdev *dev, const void *p, uint64_t b, uint32_t n)
{ struct fixture *f = dev->bdif->p_user; return kernel_block_write_at(&f->disk.device, b*512, p, n*512) == 0 ? EOK : EIO; }
static int dev_flush(struct ext4_blockdev *dev)
{ struct fixture *f = dev->bdif->p_user; return kernel_block_flush(&f->disk.device) == 0 ? EOK : EIO; }
static void lock(void *context) { struct fixture *f = context; CHECK(f); f->depth++; f->acquisitions++; }
static void unlock(void *context) { struct fixture *f = context; CHECK(f->depth); f->depth--; }
static uintptr_t owner(void *context) { struct fixture *f = context; CHECK(f->depth); return (uintptr_t)f; }
static void mount_fixture(struct fixture *f, const char *image, const char *name, const char *mount)
{
    CHECK(fault_block_open(&f->disk, image) == 0);
    f->iface = (struct ext4_blockdev_iface){ .open=dev_open, .close=dev_open,
        .bread=dev_read, .bwrite=dev_write, .flush=dev_flush, .p_user=f,
        .ph_bbuf=f->scratch, .ph_bsize=512, .ph_bcnt=f->disk.device.capacity_bytes/512 };
    f->device = (struct ext4_blockdev){ .bdif=&f->iface, .part_size=f->disk.device.capacity_bytes };
    f->locks = (struct ext4_lock){ .lock=lock, .unlock=unlock, .read_lock=lock, .owner=owner, .context=f };
    CHECK(ext4_device_register(&f->device, name) == EOK);
    CHECK(ext4_mount(name, mount, false) == EOK);
    CHECK(ext4_mount_setup_locks(mount, &f->locks) == EOK);
    CHECK(ext4_recover(mount) == EOK);
    CHECK(ext4_journal_start(mount) == EOK);
    CHECK(ext4_orphan_recover(mount) == EOK);
}
static void write_value(const char *path, const char *value)
{
    ext4_file f; size_t n;
    CHECK(ext4_fopen(&f, path, "w+") == EOK);
    CHECK(ext4_fwrite(&f, value, strlen(value), &n) == EOK && n == strlen(value));
    CHECK(ext4_fclose(&f) == EOK);
}
static void check_value(const char *path, const char *value)
{
    ext4_file f; size_t n; char data[16];
    CHECK(ext4_fopen(&f, path, "r") == EOK);
    CHECK(ext4_fpread(&f, 0, data, sizeof(data), &n) == EOK);
    CHECK(n == strlen(value) && !memcmp(data, value, n));
    CHECK(ext4_fclose(&f) == EOK);
}
int main(int argc, char **argv)
{
    CHECK(argc == 3);
    struct fixture first = {0}, second = {0};
    mount_fixture(&first, argv[1], "first", "/first/");
    mount_fixture(&second, argv[2], "second", "/second/");
    unsigned before = second.acquisitions;
    write_value("/first/shared", "FIRST");
    CHECK(second.acquisitions == before);
    before = first.acquisitions;
    write_value("/second/shared", "SECOND");
    CHECK(first.acquisitions == before);
    check_value("/first/shared", "FIRST"); check_value("/second/shared", "SECOND");
    CHECK(ext4_transaction_begin("/first/") == EOK);
    CHECK(first.depth == 1 && !second.depth);
    write_value("/first/transient", "ABORT");
    write_value("/second/persist", "KEEP");
    CHECK(first.depth == 1 && !second.depth);
    CHECK(ext4_transaction_abort("/first/", ECANCELED) == ECANCELED);
    ext4_file absent; CHECK(ext4_fopen(&absent, "/first/transient", "r") == ENOENT);
    check_value("/second/persist", "KEEP");
    CHECK(ext4_umount("/first/") == EOK); CHECK(ext4_device_unregister("first") == EOK);
    CHECK(!first.depth && !second.depth);
    write_value("/second/shared", "AFTER"); check_value("/second/shared", "AFTER");
    fault_block_close(&first.disk);
    memset(&first, 0, sizeof(first));
    mount_fixture(&first, argv[1], "replacement", "/replacement/");
    check_value("/replacement/shared", "FIRST");
    CHECK(!first.depth && !second.depth);
    CHECK(ext4_umount("/replacement/") == EOK);
    CHECK(ext4_device_unregister("replacement") == EOK);
    CHECK(ext4_umount("/second/") == EOK); CHECK(ext4_device_unregister("second") == EOK);
    fault_block_close(&first.disk); fault_block_close(&second.disk);
    return 0;
}
