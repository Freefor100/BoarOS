#define _POSIX_C_SOURCE 200809L

#include <ext4.h>
#include <ext4_blockdev.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define PHYSICAL_SECTOR_SIZE 512U
#define DEVICE_NAME "host-image"
#define MOUNT_POINT "/"

struct host_image {
	int fd;
	uint64_t size;
};

static int host_open(struct ext4_blockdev *bdev)
{
	struct host_image *image;

	if (bdev == NULL || bdev->bdif == NULL)
		return EINVAL;

	image = bdev->bdif->p_user;
	if (image == NULL || image->fd < 0)
		return EINVAL;

	return EOK;
}

static int host_range(struct ext4_blockdev *bdev, uint64_t block,
		      uint32_t count, uint64_t *offset, size_t *length)
{
	struct host_image *image;
	uint64_t byte_count;
	uint64_t byte_offset;

	if (bdev == NULL || bdev->bdif == NULL || offset == NULL ||
	    length == NULL)
		return EINVAL;

	image = bdev->bdif->p_user;
	if (image == NULL || block > UINT64_MAX / PHYSICAL_SECTOR_SIZE)
		return EINVAL;

	byte_offset = block * PHYSICAL_SECTOR_SIZE;
	byte_count = (uint64_t)count * PHYSICAL_SECTOR_SIZE;
	*length = (size_t)byte_count;
	if ((uint64_t)*length != byte_count)
		return EINVAL;
	if (byte_offset > image->size || byte_count > image->size - byte_offset ||
	    byte_offset > (uint64_t)INT64_MAX)
		return EIO;

	*offset = byte_offset;
	return EOK;
}

static int host_read(struct ext4_blockdev *bdev, void *buffer,
		     uint64_t block, uint32_t count)
{
	struct host_image *image;
	uint8_t *cursor = buffer;
	uint64_t offset;
	size_t length;
	size_t done = 0;
	int rc;

	if (buffer == NULL && count != 0)
		return EINVAL;

	rc = host_range(bdev, block, count, &offset, &length);
	if (rc != EOK)
		return rc;

	image = bdev->bdif->p_user;
	while (done < length) {
		size_t remaining = length - done;
		size_t chunk = remaining > (size_t)SSIZE_MAX ?
			(size_t)SSIZE_MAX : remaining;
		ssize_t bytes = pread(image->fd, cursor + done, chunk,
				      (off_t)(offset + done));

		if (bytes < 0) {
			if (errno == EINTR)
				continue;
			return EIO;
		}
		if (bytes == 0)
			return EIO;
		done += (size_t)bytes;
	}

	return EOK;
}

static int host_write(struct ext4_blockdev *bdev, const void *buffer,
		      uint64_t block, uint32_t count)
{
	uint64_t offset;
	size_t length;
	int rc;

	if (buffer == NULL && count != 0)
		return EINVAL;

	rc = host_range(bdev, block, count, &offset, &length);
	if (rc != EOK)
		return rc;

	(void)offset;
	(void)length;
	return EROFS;
}

static int host_close(struct ext4_blockdev *bdev)
{
	if (bdev == NULL || bdev->bdif == NULL || bdev->bdif->p_user == NULL)
		return EINVAL;

	return EOK;
}

static int report_error(const char *operation, int rc)
{
	fprintf(stderr, "%s failed: %d (%s)\n", operation, rc, strerror(rc));
	return 1;
}

int main(int argc, char **argv)
{
	uint8_t physical_buffer[PHYSICAL_SECTOR_SIZE];
	struct ext4_blockdev_iface interface = {0};
	struct ext4_blockdev device = {0};
	struct host_image image = {.fd = -1, .size = 0};
	struct stat status;
	ext4_file file = {0};
	const char *expected;
	uint8_t *actual = NULL;
	size_t expected_length;
	size_t read_count = 0;
	bool registered = false;
	bool mounted = false;
	bool opened = false;
	int failed = 0;
	int rc;

	if (argc != 4) {
		fprintf(stderr, "usage: %s IMAGE PATH EXPECTED\n", argv[0]);
		return 2;
	}

	expected = argv[3];
	expected_length = strlen(expected);
	actual = malloc(expected_length + 1);
	if (actual == NULL)
		return report_error("allocate read buffer", ENOMEM);

	image.fd = open(argv[1], O_RDONLY | O_CLOEXEC);
	if (image.fd < 0) {
		failed = report_error("open image", errno);
		goto cleanup;
	}
	if (fstat(image.fd, &status) != 0) {
		failed = report_error("stat image", errno);
		goto cleanup;
	}
	if (status.st_size <= 0 ||
	    (uintmax_t)status.st_size > UINT64_MAX ||
	    (uint64_t)status.st_size % PHYSICAL_SECTOR_SIZE != 0) {
		failed = report_error("validate image size", EINVAL);
		goto cleanup;
	}
	image.size = (uint64_t)status.st_size;

	interface.open = host_open;
	interface.bread = host_read;
	interface.bwrite = host_write;
	interface.close = host_close;
	interface.ph_bsize = PHYSICAL_SECTOR_SIZE;
	interface.ph_bcnt = image.size / PHYSICAL_SECTOR_SIZE;
	interface.ph_bbuf = physical_buffer;
	interface.p_user = &image;
	device.bdif = &interface;
	device.part_size = image.size;

	rc = ext4_device_register(&device, DEVICE_NAME);
	if (rc != EOK) {
		failed = report_error("register device", rc);
		goto cleanup;
	}
	registered = true;

	rc = ext4_mount(DEVICE_NAME, MOUNT_POINT, true);
	if (rc != EOK) {
		failed = report_error("mount image", rc);
		goto cleanup;
	}
	mounted = true;

	rc = ext4_fopen(&file, argv[2], "rb");
	if (rc != EOK) {
		failed = report_error("open fixture", rc);
		goto cleanup;
	}
	opened = true;

	rc = ext4_fread(&file, actual, expected_length, &read_count);
	if (rc != EOK) {
		failed = report_error("read fixture", rc);
		goto cleanup;
	}
	if (read_count != expected_length ||
	    memcmp(actual, expected, expected_length) != 0) {
		fprintf(stderr, "fixture contents differ: got %zu of %zu bytes\n",
			read_count, expected_length);
		failed = 1;
		goto cleanup;
	}

	read_count = 1;
	rc = ext4_fread(&file, actual, 1, &read_count);
	if (rc != EOK) {
		failed = report_error("read fixture EOF", rc);
		goto cleanup;
	}
	if (read_count != 0) {
		fprintf(stderr, "fixture has trailing data\n");
		failed = 1;
	}

cleanup:
	if (opened) {
		rc = ext4_fclose(&file);
		if (rc != EOK) {
			report_error("close fixture", rc);
			failed = 1;
		}
	}
	if (mounted) {
		rc = ext4_umount(MOUNT_POINT);
		if (rc != EOK) {
			report_error("unmount image", rc);
			failed = 1;
		}
	}
	if (registered) {
		rc = ext4_device_unregister(DEVICE_NAME);
		if (rc != EOK) {
			report_error("unregister device", rc);
			failed = 1;
		}
	}
	if (image.fd >= 0 && close(image.fd) != 0) {
		report_error("close image", errno);
		failed = 1;
	}
	free(actual);

	if (failed == 0)
		puts("lwext4 host read passed");
	return failed;
}
