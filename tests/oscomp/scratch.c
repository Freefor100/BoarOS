/* PID 1 checks the device it will authorize LTP to overwrite. */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

static int device(const char *path, struct stat *st, uint64_t *bytes)
{
    int fd = open(path, O_RDWR | O_CLOEXEC);
    int readonly = -1, sector = 0;
    const char *stage = "open";
    if (fd < 0) goto error;
    stage = "fstat";
    if (fstat(fd, st)) goto error;
    if (!S_ISBLK(st->st_mode)) {
        fprintf(stderr, "BOAROS-SCRATCH ERROR path=%s mode=%#o is not block\n", path, st->st_mode);
        close(fd);
        return -1;
    }
    stage = "BLKGETSIZE64";
    if (ioctl(fd, BLKGETSIZE64, bytes)) goto error;
    stage = "BLKSSZGET";
    if (ioctl(fd, BLKSSZGET, &sector)) goto error;
    stage = "BLKROGET";
    if (ioctl(fd, BLKROGET, &readonly)) goto error;
    if (readonly || sector != 512 || !*bytes) {
        fprintf(stderr, "BOAROS-SCRATCH ERROR path=%s bytes=%" PRIu64 " sector=%d readonly=%d\n",
                path, *bytes, sector, readonly);
        close(fd);
        return -1;
    }
    if (!close(fd)) return 0;
    stage = "close";
    fd = -1;
error:
    fprintf(stderr, "BOAROS-SCRATCH ERROR path=%s stage=%s errno=%d\n", path, stage, errno);
    if (fd >= 0) close(fd);
    return -1;
}

int main(int argc, char **argv)
{
    struct stat root, rootdev, scratch;
    uint64_t root_bytes = 0, scratch_bytes = 0;
    if (argc != 4) return 125;
    char *end;
    uint64_t expected = strtoull(argv[3], &end, 10);
    if (!expected || *end) return 125;
    if (stat("/", &root)) {
        fprintf(stderr, "BOAROS-SCRATCH ERROR path=/ stage=stat errno=%d\n", errno);
        return 125;
    }
    if (device(argv[1], &rootdev, &root_bytes) ||
        device(argv[2], &scratch, &scratch_bytes)) return 125;
    /* A node name alone cannot prove that the disposable disk differs from /. */
    if (root.st_dev != rootdev.st_rdev || rootdev.st_rdev == scratch.st_rdev ||
        scratch_bytes != expected || major(rootdev.st_rdev) != 252 ||
        minor(rootdev.st_rdev) != 0 || major(scratch.st_rdev) != 252 ||
        minor(scratch.st_rdev) != 16) {
        fprintf(stderr, "BOAROS-SCRATCH ERROR root_fs=%u:%u root_block=%u:%u scratch=%u:%u"
                " scratch_bytes=%" PRIu64 " expected_bytes=%" PRIu64 "\n",
                major(root.st_dev), minor(root.st_dev), major(rootdev.st_rdev), minor(rootdev.st_rdev),
                major(scratch.st_rdev), minor(scratch.st_rdev), scratch_bytes, expected);
        return 125;
    }
    printf("BOAROS-SCRATCH READY root=%u:%u root_bytes=%" PRIu64
           " scratch=%u:%u scratch_bytes=%" PRIu64 "\n",
           major(rootdev.st_rdev), minor(rootdev.st_rdev), root_bytes,
           major(scratch.st_rdev), minor(scratch.st_rdev), scratch_bytes);
    return 0;
}
