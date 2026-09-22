#define _POSIX_C_SOURCE 200809L

#include <ext4.h>
#include <ext4_blockdev.h>
#include <ext4_misc.h>
#include <ext4_fs.h>

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
	bool fail_next_initialization_io;
	uint64_t fail_write_offset;
	unsigned int fail_writes;
	unsigned int failed_writes;
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
	if (image->fail_next_initialization_io && count == 1) {
		image->fail_next_initialization_io = false;
		return EIO;
	}
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
	struct host_image *image;
	const uint8_t *cursor = buffer;
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
	if (image->fail_writes && image->fail_write_offset >= offset &&
	    image->fail_write_offset < offset + length) {
		image->fail_writes--;
		image->failed_writes++;
		return EIO;
	}

	while (done < length) {
		size_t remaining = length - done;
		size_t chunk = remaining > (size_t)SSIZE_MAX ?
			(size_t)SSIZE_MAX : remaining;
		ssize_t bytes = pwrite(image->fd, cursor + done, chunk,
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

static bool all_zero(const uint8_t *buffer, size_t size)
{
	for (size_t i = 0; i < size; ++i) {
		if (buffer[i] != 0)
			return false;
	}
	return true;
}

static int check_fixture(const char *path, const char *expected)
{
	ext4_file file = {0};
	size_t expected_length = strlen(expected);
	uint8_t *actual = malloc(expected_length + 1);
	size_t read_count = 0;
	int failed = 0;
	int rc;

	if (actual == NULL)
		return report_error("allocate read buffer", ENOMEM);
	rc = ext4_fopen(&file, path, "rb");
	if (rc != EOK) {
		failed = report_error("open fixture", rc);
		goto cleanup;
	}
	rc = ext4_fread(&file, actual, expected_length, &read_count);
	if (rc != EOK) {
		failed = report_error("read fixture", rc);
		goto close;
	}
	if (read_count != expected_length ||
	    memcmp(actual, expected, expected_length) != 0) {
		fprintf(stderr, "fixture contents differ: got %zu of %zu bytes\n",
			read_count, expected_length);
		failed = 1;
		goto close;
	}
	read_count = 1;
	rc = ext4_fread(&file, actual, 1, &read_count);
	if (rc != EOK) {
		failed = report_error("read fixture EOF", rc);
		goto close;
	}
	if (read_count != 0) {
		fprintf(stderr, "fixture has trailing data\n");
		failed = 1;
	}

close:
	rc = ext4_fclose(&file);
	if (rc != EOK) {
		report_error("close fixture", rc);
		failed = 1;
	}
cleanup:
	free(actual);
	return failed;
}

static int check_aligned_hole(void)
{
	uint8_t actual[1024];
	ext4_file file = {0};
	size_t read_count = 0;
	int failed = 0;
	int rc;

	memset(actual, 0xa5, sizeof(actual));
	rc = ext4_fopen(&file, "/aligned-hole", "rb");
	if (rc != EOK)
		return report_error("open aligned hole", rc);
	rc = ext4_fread(&file, actual, sizeof(actual), &read_count);
	if (rc != EOK) {
		failed = report_error("read aligned hole", rc);
	} else if (read_count != sizeof(actual) ||
		   !all_zero(actual, sizeof(actual))) {
		fprintf(stderr,
			"aligned unmapped block was not returned as zero\n");
		failed = 1;
	}
	rc = ext4_fclose(&file);
	if (rc != EOK) {
		report_error("close aligned hole", rc);
		failed = 1;
	}
	return failed;
}

static int check_sparse_address_limit(void)
{
	const uint64_t aliased_offset = UINT64_C(1) << 42;
	const uint64_t max_offset = aliased_offset - 1024;
	const uint64_t oversized = aliased_offset + 1024;
	uint8_t byte = 0;
	ext4_file file = {0};
	size_t count = 0;
	int truncate_rc;
	int seek_rc;
	int write_rc;
	int seek_cur_rc;
	int failed = 0;
	int rc;

	rc = ext4_fopen2(&file, "/sparse-limit", O_CREAT | O_RDWR | O_TRUNC);
	if (rc != EOK)
		return report_error("create sparse limit file", rc);
	rc = ext4_fwrite(&file, "L", 1, &count);
	if (rc != EOK || count != 1) {
		failed = report_error("write sparse limit marker",
				      rc != EOK ? rc : EIO);
		goto close;
	}

	truncate_rc = ext4_ftruncate(&file, oversized);
	seek_rc = ext4_fseek(&file, (int64_t)aliased_offset, SEEK_SET);
	if (seek_rc == EOK) {
		count = 0;
		rc = ext4_fread(&file, &byte, 1, &count);
		if (rc == EOK && count == 1 && byte == 'L')
			fprintf(stderr,
				"oversized sparse offset aliased logical block zero\n");
	}
	count = 0;
	if (ext4_fseek(&file, (int64_t)max_offset, SEEK_SET) != EOK) {
		write_rc = EIO;
		seek_cur_rc = EIO;
	} else {
		write_rc = ext4_fwrite(&file, "X", 1, &count);
		seek_cur_rc = ext4_fseek(&file, 1, SEEK_CUR);
	}
	if (truncate_rc != EFBIG || seek_rc != EINVAL ||
	    write_rc != EFBIG || count != 0 || seek_cur_rc != EINVAL ||
	    ext4_fsize(&file) != 1) {
		fprintf(stderr,
			"sparse address limit mismatch: truncate=%d seek=%d write=%d count=%zu seek_cur=%d size=%" PRIu64 "\n",
			truncate_rc, seek_rc, write_rc, count, seek_cur_rc,
			ext4_fsize(&file));
		failed = 1;
	}

close:
	/* Restore a small position/size even when the regression is present so
	 * the RED run can still unmount and check the image. */
	(void)ext4_fseek(&file, 0, SEEK_SET);
	(void)ext4_ftruncate(&file, 1);
	rc = ext4_fclose(&file);
	if (rc != EOK) {
		report_error("close sparse limit file", rc);
		failed = 1;
	}
	return failed;
}

static int check_legacy_address_limit(void)
{
	static const uint8_t marker[] = "LEGACY";
	const uint64_t legacy_max = UINT64_C(17247252480);
	const uint64_t beyond = legacy_max + 1;
	uint8_t byte = 0xa5;
	ext4_file file = {0};
	size_t count = 0;
	int failed = 0;
	int rc;

	rc = ext4_fopen2(&file, "/legacy-limit", O_RDWR);
	if (rc != EOK)
		return report_error("open legacy limit file", rc);
	rc = ext4_fwrite(&file, marker, sizeof(marker) - 1, &count);
	if (rc != EOK || count != sizeof(marker) - 1) {
		failed = report_error("write legacy limit marker",
				      rc != EOK ? rc : EIO);
		goto close;
	}

	rc = ext4_ftruncate(&file, legacy_max);
	if (rc != EOK || ext4_fsize(&file) != legacy_max ||
	    ext4_ftell(&file) != sizeof(marker) - 1) {
		fprintf(stderr,
			"legacy exact-limit truncate mismatch: rc=%d size=%" PRIu64 " pos=%" PRIu64 "\n",
			rc, ext4_fsize(&file), ext4_ftell(&file));
		failed = 1;
		goto close;
	}
	if (ext4_fseek(&file, (int64_t)(legacy_max - 1), SEEK_SET) != EOK) {
		failed = report_error("seek legacy last byte", EIO);
		goto close;
	}
	count = 0;
	rc = ext4_fread(&file, &byte, 1, &count);
	if (rc != EOK || count != 1 || byte != 0 ||
	    ext4_ftell(&file) != legacy_max) {
		fprintf(stderr,
			"legacy exact-limit read mismatch: rc=%d count=%zu byte=%#x pos=%" PRIu64 "\n",
			rc, count, byte, ext4_ftell(&file));
		failed = 1;
	}

	rc = ext4_ftruncate(&file, beyond);
	if (rc != EFBIG || ext4_fsize(&file) != legacy_max ||
	    ext4_ftell(&file) != legacy_max) {
		fprintf(stderr,
			"legacy beyond-limit truncate mismatch: rc=%d size=%" PRIu64 " pos=%" PRIu64 "\n",
			rc, ext4_fsize(&file), ext4_ftell(&file));
		failed = 1;
	}
	rc = ext4_fseek(&file, (int64_t)beyond, SEEK_SET);
	if (rc != EINVAL || ext4_ftell(&file) != legacy_max) {
		fprintf(stderr,
			"legacy beyond-limit seek mismatch: rc=%d pos=%" PRIu64 "\n",
			rc, ext4_ftell(&file));
		failed = 1;
	}
	(void)ext4_fseek(&file, (int64_t)legacy_max, SEEK_SET);
	count = 0;
	rc = ext4_fwrite(&file, "X", 1, &count);
	if (rc != EFBIG || count != 0 || ext4_fsize(&file) != legacy_max ||
	    ext4_ftell(&file) != legacy_max) {
		fprintf(stderr,
			"legacy beyond-limit write mismatch: rc=%d count=%zu size=%" PRIu64 " pos=%" PRIu64 "\n",
			rc, count, ext4_fsize(&file), ext4_ftell(&file));
		failed = 1;
	}

close:
	rc = ext4_fclose(&file);
	if (rc != EOK) {
		report_error("close legacy limit file", rc);
		failed = 1;
	}
	return failed;
}

static int check_wide_legacy_address_limit(void)
{
	static const uint8_t marker[] = "WIDE";
	const uint64_t legacy_max = UINT64_C(35184372080640);
	const uint64_t beyond = legacy_max + 1;
	const uint64_t aliased_offset = UINT64_C(35184372088832);
	const uint64_t aliased_size = aliased_offset + 1;
	uint8_t byte = 0xa5;
	uint8_t actual[sizeof(marker) - 1];
	ext4_file file = {0};
	size_t count = 0;
	int alias_seek_rc;
	int failed = 0;
	int rc;

	rc = ext4_fopen2(&file, "/wide-legacy-limit", O_RDWR);
	if (rc != EOK)
		return report_error("open wide legacy limit file", rc);
	rc = ext4_fwrite(&file, marker, sizeof(marker) - 1, &count);
	if (rc != EOK || count != sizeof(marker) - 1) {
		failed = report_error("write wide legacy limit marker",
				      rc != EOK ? rc : EIO);
		goto close;
	}

	rc = ext4_ftruncate(&file, legacy_max);
	if (rc != EOK || ext4_fsize(&file) != legacy_max ||
	    ext4_ftell(&file) != sizeof(marker) - 1) {
		fprintf(stderr,
			"wide legacy exact-limit truncate mismatch: rc=%d size=%" PRIu64 " pos=%" PRIu64 "\n",
			rc, ext4_fsize(&file), ext4_ftell(&file));
		failed = 1;
		goto close;
	}
	if (ext4_fseek(&file, (int64_t)(legacy_max - 1), SEEK_SET) != EOK) {
		failed = report_error("seek wide legacy last byte", EIO);
		goto close;
	}
	count = 0;
	rc = ext4_fread(&file, &byte, 1, &count);
	if (rc != EOK || count != 1 || byte != 0 ||
	    ext4_ftell(&file) != legacy_max) {
		fprintf(stderr,
			"wide legacy exact-limit read mismatch: rc=%d count=%zu byte=%#x pos=%" PRIu64 "\n",
			rc, count, byte, ext4_ftell(&file));
		failed = 1;
	}

	rc = ext4_ftruncate(&file, beyond);
	if (rc != EFBIG || ext4_fsize(&file) != legacy_max ||
	    ext4_ftell(&file) != legacy_max) {
		fprintf(stderr,
			"wide legacy first-disallowed truncate mismatch: rc=%d size=%" PRIu64 " pos=%" PRIu64 "\n",
			rc, ext4_fsize(&file), ext4_ftell(&file));
		failed = 1;
	}
	rc = ext4_fseek(&file, (int64_t)beyond, SEEK_SET);
	if (rc != EINVAL || ext4_ftell(&file) != legacy_max) {
		fprintf(stderr,
			"wide legacy first-disallowed seek mismatch: rc=%d pos=%" PRIu64 "\n",
			rc, ext4_ftell(&file));
		failed = 1;
	}
	if (ext4_fseek(&file, (int64_t)legacy_max, SEEK_SET) != EOK) {
		failed = report_error("seek wide legacy exact EOF", EIO);
		goto verify_marker;
	}
	count = 0;
	byte = 0xa5;
	rc = ext4_fread(&file, &byte, 1, &count);
	if (rc != EOK || count != 0 || ext4_ftell(&file) != legacy_max) {
		fprintf(stderr,
			"wide legacy exact-EOF read mismatch: rc=%d count=%zu byte=%#x pos=%" PRIu64 "\n",
			rc, count, byte, ext4_ftell(&file));
		failed = 1;
	}
	(void)ext4_fseek(&file, (int64_t)legacy_max, SEEK_SET);
	count = 0;
	rc = ext4_fwrite(&file, "X", 1, &count);
	if (rc != EFBIG || count != 0 || ext4_fsize(&file) != legacy_max ||
	    ext4_ftell(&file) != legacy_max) {
		fprintf(stderr,
			"wide legacy first-disallowed write mismatch: rc=%d count=%zu size=%" PRIu64 " pos=%" PRIu64 "\n",
			rc, count, ext4_fsize(&file), ext4_ftell(&file));
		failed = 1;
	}

	(void)ext4_fseek(&file, (int64_t)legacy_max, SEEK_SET);
	rc = ext4_ftruncate(&file, aliased_size);
	if (rc != EFBIG || ext4_fsize(&file) != legacy_max ||
	    ext4_ftell(&file) != legacy_max) {
		fprintf(stderr,
			"wide legacy aliased-size truncate mismatch: rc=%d size=%" PRIu64 " pos=%" PRIu64 "\n",
			rc, ext4_fsize(&file), ext4_ftell(&file));
		failed = 1;
	}
	alias_seek_rc = ext4_fseek(&file, (int64_t)aliased_offset, SEEK_SET);
	if (alias_seek_rc == EOK) {
		count = 0;
		byte = 0xa5;
		rc = ext4_fread(&file, &byte, 1, &count);
		if (rc == EOK && count == 1 && byte == marker[0])
			fprintf(stderr,
				"wide legacy offset 2^45 aliased logical block zero\n");
		(void)ext4_fseek(&file, (int64_t)aliased_offset, SEEK_SET);
		count = 0;
		(void)ext4_fwrite(&file, "X", 1, &count);
	}
	if (alias_seek_rc != EINVAL || ext4_ftell(&file) != legacy_max ||
	    ext4_fsize(&file) != legacy_max) {
		fprintf(stderr,
			"wide legacy aliased offset was accepted: seek=%d size=%" PRIu64 " pos=%" PRIu64 "\n",
			alias_seek_rc, ext4_fsize(&file), ext4_ftell(&file));
		failed = 1;
	}

verify_marker:
	if (ext4_fseek(&file, 0, SEEK_SET) != EOK) {
		failed = report_error("rewind wide legacy marker", EIO);
		goto close;
	}
	memset(actual, 0, sizeof(actual));
	count = 0;
	rc = ext4_fread(&file, actual, sizeof(actual), &count);
	if (rc != EOK || count != sizeof(actual) ||
	    memcmp(actual, marker, sizeof(actual)) != 0) {
		fprintf(stderr,
			"wide legacy marker changed: rc=%d count=%zu first=%#x size=%" PRIu64 "\n",
			rc, count, actual[0], ext4_fsize(&file));
		failed = 1;
	}

close:
	rc = ext4_fclose(&file);
	if (rc != EOK) {
		report_error("close wide legacy limit file", rc);
		failed = 1;
	}
	return failed;
}

static int check_failed_block_initialization(struct host_image *image)
{
	struct ext4_inode inode;
	ext4_file file = {0};
	size_t count = 0;
	int failed = 0;
	int rc;

	rc = ext4_fopen2(&file, "/failed-initialization",
			 O_CREAT | O_RDWR | O_TRUNC);
	if (rc != EOK)
		return report_error("create failed-initialization file", rc);
	if (ext4_fseek(&file, 73, SEEK_SET) != EOK) {
		failed = report_error("seek failed-initialization file", EIO);
		goto close;
	}

	image->fail_next_initialization_io = true;
	rc = ext4_fwrite(&file, "X", 1, &count);
	if (rc != EIO || count != 0 || ext4_fsize(&file) != 0) {
		fprintf(stderr,
			"initialization failure result mismatch: rc=%d count=%zu size=%" PRIu64 "\n",
			rc, count, ext4_fsize(&file));
		failed = 1;
	}
	rc = ext4_fraw_inode_fill(&file, &inode);
	if (rc != EOK || to_le32(inode.blocks_count_lo) != 0) {
		fprintf(stderr,
			"initialization failure left an allocated mapping: rc=%d blocks=%" PRIu32 "\n",
			rc, to_le32(inode.blocks_count_lo));
		failed = 1;
	}

close:
	image->fail_next_initialization_io = false;
	rc = ext4_fclose(&file);
	if (rc != EOK) {
		report_error("close failed-initialization file", rc);
		failed = 1;
	}
	return failed;
}

static int check_preexisting_unwritten_extent(void)
{
	static const uint8_t marker[] = "LIVE";
	uint8_t block[1024];
	ext4_file file = {0};
	size_t count = 0;
	int failed = 0;
	int rc;

	rc = ext4_fopen2(&file, "/unwritten-partial", O_RDWR);
	if (rc != EOK)
		return report_error("open unwritten extent", rc);
	if (ext4_fsize(&file) != sizeof(block) ||
	    ext4_fseek(&file, 17, SEEK_SET) != EOK) {
		failed = report_error("prepare unwritten extent", EIO);
		goto close;
	}
	rc = ext4_fwrite(&file, marker, sizeof(marker) - 1, &count);
	if (rc != EOK || count != sizeof(marker) - 1 ||
	    ext4_fseek(&file, 0, SEEK_SET) != EOK) {
		failed = report_error("write unwritten extent marker",
				      rc != EOK ? rc : EIO);
		goto close;
	}
	memset(block, 0xa5, sizeof(block));
	rc = ext4_fread(&file, block, sizeof(block), &count);
	if (rc != EOK || count != sizeof(block) ||
	    !all_zero(block, 17) ||
	    memcmp(block + 17, marker, sizeof(marker) - 1) != 0 ||
	    !all_zero(block + 17 + sizeof(marker) - 1,
		      sizeof(block) - 17 - (sizeof(marker) - 1))) {
		fprintf(stderr,
			"partial write into unwritten extent did not persist\n");
		failed = 1;
	}

close:
	rc = ext4_fclose(&file);
	if (rc != EOK) {
		report_error("close unwritten extent", rc);
		failed = 1;
	}
	return failed;
}

static int check_sparse_behavior(struct host_image *image)
{
	static const uint8_t marker[] = "BOAR";
	const uint64_t sparse_offset = 8192 + 17;
	uint8_t buffer[8192 + 50];
	struct ext4_inode inode;
	ext4_file file = {0};
	size_t count = 0;
	int failed = 0;
	int rc;

	failed = check_sparse_address_limit();
	if (check_failed_block_initialization(image) != 0)
		failed = 1;
	if (check_preexisting_unwritten_extent() != 0)
		failed = 1;

	rc = ext4_fopen2(&file, "/sparse-write", O_CREAT | O_RDWR | O_TRUNC);
	if (rc != EOK)
		return report_error("create sparse write", rc);
	rc = ext4_fseek(&file, (int64_t)sparse_offset, SEEK_SET);
	if (rc != EOK) {
		failed = report_error("seek sparse write", rc);
		goto close;
	}
	rc = ext4_fwrite(&file, marker, sizeof(marker) - 1, &count);
	if (rc != EOK || count != sizeof(marker) - 1) {
		failed = report_error("write sparse marker", rc != EOK ? rc : EIO);
		goto close;
	}
	if (ext4_fsize(&file) != sparse_offset + sizeof(marker) - 1) {
		fprintf(stderr, "sparse write size differs: got %" PRIu64 "\n",
			ext4_fsize(&file));
		failed = 1;
		goto close;
	}
	rc = ext4_fseek(&file, 0, SEEK_SET);
	if (rc != EOK) {
		failed = report_error("rewind sparse write", rc);
		goto close;
	}
	memset(buffer, 0xa5, (size_t)sparse_offset + sizeof(marker) - 1);
	rc = ext4_fread(&file, buffer,
			(size_t)sparse_offset + sizeof(marker) - 1, &count);
	if (rc != EOK || count != sparse_offset + sizeof(marker) - 1 ||
	    !all_zero(buffer, (size_t)sparse_offset) ||
	    memcmp(buffer + sparse_offset, marker, sizeof(marker) - 1) != 0) {
		failed = report_error("verify sparse write", rc != EOK ? rc : EIO);
		goto close;
	}
	rc = ext4_fclose(&file);
	if (rc != EOK)
		return report_error("close sparse write", rc);

	memset(&file, 0, sizeof(file));
	rc = ext4_fopen2(&file, "/same-block-sparse",
			 O_CREAT | O_RDWR | O_TRUNC);
	if (rc != EOK)
		return report_error("create same-block sparse file", rc);
	count = 0;
	rc = ext4_fwrite(&file, "PRE", 3, &count);
	if (rc != EOK || count != 3 ||
	    ext4_fseek(&file, 73, SEEK_SET) != EOK) {
		failed = report_error("prepare same-block sparse write",
				      rc != EOK ? rc : EIO);
		goto close;
	}
	count = 0;
	rc = ext4_fwrite(&file, "END", 3, &count);
	if (rc != EOK || count != 3 || ext4_fseek(&file, 0, SEEK_SET) != EOK) {
		failed = report_error("write same-block sparse marker",
				      rc != EOK ? rc : EIO);
		goto close;
	}
	memset(buffer, 0xa5, 76);
	rc = ext4_fread(&file, buffer, 76, &count);
	if (rc != EOK || count != 76 || memcmp(buffer, "PRE", 3) != 0 ||
	    !all_zero(buffer + 3, 70) || memcmp(buffer + 73, "END", 3) != 0) {
		failed = report_error("verify same-block sparse write",
				      rc != EOK ? rc : EIO);
		goto close;
	}
	rc = ext4_fclose(&file);
	if (rc != EOK)
		return report_error("close same-block sparse file", rc);

	memset(&file, 0, sizeof(file));
	rc = ext4_fopen2(&file, "/sparse-truncate",
			 O_CREAT | O_RDWR | O_TRUNC);
	if (rc != EOK)
		return report_error("create sparse truncate file", rc);
	count = 0;
	rc = ext4_fwrite(&file, "12345", 5, &count);
	if (rc != EOK || count != 5 || ext4_ftruncate(&file, 8192 + 50) != EOK ||
	    ext4_ftell(&file) != 5 || ext4_fsize(&file) != 8192 + 50) {
		failed = report_error("grow sparse truncate file",
				      rc != EOK ? rc : EIO);
		goto close;
	}
	memset(buffer, 0xa5, 8192 + 45);
	rc = ext4_fread(&file, buffer, 8192 + 45, &count);
	if (rc != EOK || count != 8192 + 45 ||
	    !all_zero(buffer, 8192 + 45)) {
		failed = report_error("verify sparse truncate zeros",
				      rc != EOK ? rc : EIO);
		goto close;
	}
	rc = ext4_fraw_inode_fill(&file, &inode);
	if (rc != EOK ||
	    (uint64_t)to_le32(inode.blocks_count_lo) * 512 >= ext4_fsize(&file)) {
		failed = report_error("verify sparse truncate allocation",
				      rc != EOK ? rc : EIO);
	}

close:
	rc = ext4_fclose(&file);
	if (rc != EOK) {
		report_error("close sparse test file", rc);
		failed = 1;
	}
	return failed;
}

#include "lwext4_timestamps.h"
#include "lwext4_writeback.h"

int main(int argc, char **argv)
{
	uint8_t physical_buffer[PHYSICAL_SECTOR_SIZE];
	struct ext4_blockdev_iface interface = {0};
	struct ext4_blockdev device = {0};
	struct host_image image = {.fd = -1, .size = 0};
	struct stat status;
	const char *expected = NULL;
	bool sparse_mode;
	bool timestamp_mode;
	bool timestamp_readonly;
	bool aligned_hole_mode;
	bool legacy_limit_mode;
	bool wide_legacy_limit_mode;
	bool registered = false;
	bool mounted = false;
	int failed = 0;
	int rc;

    timestamp_mode = argc == 3 && strcmp(argv[2], "--timestamps") == 0;
    timestamp_readonly = argc == 3 && strcmp(argv[2], "--timestamps-readonly") == 0;
	sparse_mode = argc == 3 && strcmp(argv[2], "--sparse") == 0;
	aligned_hole_mode = argc == 3 &&
		strcmp(argv[2], "--aligned-hole") == 0;
	legacy_limit_mode = argc == 3 &&
		strcmp(argv[2], "--legacy-limit") == 0;
	wide_legacy_limit_mode = argc == 3 &&
		strcmp(argv[2], "--wide-legacy-limit") == 0;
	if (argc != 4 && !sparse_mode && !aligned_hole_mode &&
	    !legacy_limit_mode && !wide_legacy_limit_mode &&
        !timestamp_mode && !timestamp_readonly) {
		fprintf(stderr,
			"usage: %s IMAGE PATH EXPECTED | IMAGE --aligned-hole | IMAGE --sparse | IMAGE --legacy-limit | IMAGE --wide-legacy-limit\n",
			argv[0]);
		return 2;
	}

	if (argc == 4)
		expected = argv[3];

	image.fd = open(argv[1],
			(sparse_mode || legacy_limit_mode || wide_legacy_limit_mode || timestamp_mode ?
			 O_RDWR : O_RDONLY) |
			O_CLOEXEC);
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

	rc = ext4_mount(DEVICE_NAME, MOUNT_POINT,
			!sparse_mode && !legacy_limit_mode &&
			!wide_legacy_limit_mode && !timestamp_mode);
	if (rc != EOK) {
		failed = report_error("mount image", rc);
		goto cleanup;
	}
	mounted = true;

	if (timestamp_mode || timestamp_readonly)
		failed = check_timestamp_behavior(timestamp_readonly);
	else if (sparse_mode)
		failed = check_writeback_errors(&image, &device) ||
		         check_sparse_behavior(&image);
	else if (legacy_limit_mode)
		failed = check_legacy_address_limit();
	else if (wide_legacy_limit_mode)
		failed = check_wide_legacy_address_limit();
	else if (aligned_hole_mode)
		failed = check_aligned_hole();
	else
		failed = check_fixture(argv[2], expected);

cleanup:
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
	if (failed == 0)
		puts("lwext4 host check passed");
	return failed;
}
