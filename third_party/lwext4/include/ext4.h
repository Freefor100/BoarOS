/*
 * Copyright (c) 2013 Grzegorz Kostka (kostka.grzegorz@gmail.com)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * - The name of the author may not be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/** @addtogroup lwext4
 * @{
 */
/**
 * @file  ext4.h
 * @brief Ext4 high level operations (files, directories, mount points...).
 *        Client has to include only this file.
 */

#ifndef EXT4_H_
#define EXT4_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

#include <ext4_config.h>
#include <ext4_types.h>
#include <ext4_errno.h>
#include <ext4_oflags.h>
#include <ext4_debug.h>

#include <ext4_blockdev.h>
#include <ext4_operation.h>

/********************************OS LOCK INFERFACE***************************/

/**@brief   OS dependent lock interface.*/
struct ext4_lock {

	/* Enter the backend operation. Legacy ports serialize the mount here;
     * parallel ports retain call qualification while object/resource locks
     * protect the actual directory, inode and allocation mutations. */
	void (*lock)(void *context);

	/**@brief   Unlock access to mount point.*/
	void (*unlock)(void *context);
	void (*read_lock)(void *context);
	/* True only while this caller holds the actual sleeping mount writer.
	 * It stabilizes running/index owners through private preparation/accept. */
	bool (*serial_writer)(void *context);
	void *context;
	void (*state_lock)(void *context);
	/* Read-only snapshots borrow accepted records until state_unlock. A port
	 * without shared qualification keeps using its exclusive state_lock. */
	void (*state_read_lock)(void *context);
	void (*state_unlock)(void *context);
	int (*resource_acquire)(void *context, struct ext4_operation *operation,
	                        enum ext4_resource_kind kind, uint32_t key);
	int (*resource_claim_inode)(void *context, struct ext4_operation *operation,
	                            uint32_t inode);
	void (*resource_release)(void *context);
	bool (*resource_retry)(void *context);
};

/* now_ns supplies a bounded clock snapshot. request() carries actual COMMIT /
 * CHECKPOINT consumer bits: first running/deadline or due work requests COMMIT,
 * actionable reclaim requests CHECKPOINT. Ordinary merges do not reset timers.
 * request() and wait() run outside shared journal-state protection. wait()
 * releases any legacy mount qualification before sleeping and restores it
 * before returning. */
enum ext4_journal_wait {
	EXT4_JOURNAL_WAIT_SEALED,
	EXT4_JOURNAL_WAIT_DURABLE,
	EXT4_JOURNAL_WAIT_CHECKPOINT,
};
enum ext4_journal_work {
	EXT4_JOURNAL_WORK_COMMIT = 1,
	EXT4_JOURNAL_WORK_CHECKPOINT = 2,
};
struct ext4_journal_runtime {
	void *context;
	uint64_t (*now_ns)(void *context);
	void (*request)(void *context, unsigned work);
	int (*wait)(void *context, uint64_t sequence, enum ext4_journal_wait kind);
};
int ext4_journal_group_enable(const char *mount_point,
	const struct ext4_journal_runtime *runtime, size_t memory_limit);
/* A runtime supplies one consumer for each stage. Selection, freezing and
 * retirement publish shared state under state_lock. Consumers retain exact
 * immutable image references across device I/O outside that protection.
 * service() drives either stage synchronously when no runtime is supplied. */
int ext4_journal_group_service(const char *mount_point, bool force);
int ext4_journal_group_commit(const char *mount_point, bool force);
int ext4_journal_group_checkpoint(const char *mount_point);
int ext4_journal_group_drain(const char *mount_point);
/* Quota pressure reuses closed groups; memory pressure may end a running
 * group. Explicit drain still waits for all work accepted at its entry. */
int ext4_journal_group_reclaim(const char *mount_point, bool memory_pressure);
struct ext4_journal_progress {
	uint64_t accepted, sealed, durable, checkpoint, deadline_ns;
	size_t memory_used, memory_peak;
	bool ready, commit_ready, checkpoint_ready, checkpoint_pending;
	int error;
};
int ext4_journal_group_progress(const char *mount_point, struct ext4_journal_progress *progress);

/********************************FILE DESCRIPTOR*****************************/

struct ext4_delalloc_chunk;
struct ext4_delalloc_change;
RB_HEAD(ext4_delalloc_tree, ext4_delalloc_chunk);
struct ext4_delalloc {
    struct ext4_delalloc_tree tree;
    struct ext4_delalloc_chunk *retired;
    uint64_t blocks;
};

/**@brief   File descriptor. */
typedef struct ext4_file {

	/**@brief   Mount point handle.*/
	struct ext4_mountpoint *mp;

	/**@brief   File inode id.*/
	uint32_t inode;

	/**@brief   Open flags.*/
	uint32_t flags;

	/**@brief   File size.*/
	uint64_t fsize;

	/**@brief   Maximum size addressable by this inode's block mapping.*/
	uint64_t fmax;

	/**@brief   Actual file position.*/
	uint64_t fpos;

	void *time_owner; /* Adapter-owned live inode; valid for this handle lifetime. */
    struct ext4_delalloc *delalloc; /* Shared by positioned handle copies. */
} ext4_file;

/* BoarOS adapter: optional realtime source and live-inode timestamp updates.
 * The callback returns false until wall-clock initialization is complete.
 * Seconds are signed; nanoseconds must be in [0, 1000000000). */
struct ext4_timestamp {
    int64_t seconds;
    uint32_t nanoseconds;
};
typedef bool (*ext4_clock_read)(struct ext4_timestamp *now);
/* Adapter-owned live inode times. Stores are tentative until the outer
 * transaction completes. Handle stores borrow time_owner; namespace stores
 * pass NULL. The store callback attaches its records to the current operation;
 * finish receives only that operation's records, after journal acceptance. */
struct ext4_time_observer {
    void *context;
    void (*store)(void *context, void *owner, uint32_t inode, unsigned field,
                  struct ext4_timestamp value);
    void (*finish)(void *context, void *staged_times, bool accepted);
    unsigned (*snapshot)(void *owner, struct ext4_timestamp times[3]);
    void (*written)(void *owner, unsigned fields,
                    const struct ext4_timestamp times[3], int result);
};
int ext4_mount_setup_time_observer(const char *mount_point,
                                  const struct ext4_time_observer *observer);

#define EXT4_TIME_ATIME 1U
#define EXT4_TIME_MTIME 2U
#define EXT4_TIME_CTIME 4U
#define EXT4_TIME_RELATIME 8U
int ext4_mount_setup_clock(const char *mount_point, ext4_clock_read clock);
/* Read the mount clock without inode lookup or a metadata transaction. */
bool ext4_file_read_clock(const ext4_file *file, struct ext4_timestamp *now);
int ext4_file_touch(ext4_file *file, unsigned int fields);
/* Pure relatime query under a shared mount gate. A true result grants no
 * mutation right: reacquire exclusive ownership and recheck before updating. */
int ext4_file_relatime_needed(ext4_file *file, bool *needed);
/* Set selected atime/mtime/ctime values by live inode identity. Array order is
 * atime, mtime, ctime; unselected values are ignored. Only EXT4_TIME_{A,M,C}TIME
 * bits are valid. A zero mask performs no I/O and permits times == NULL. */
int ext4_file_set_times(ext4_file *file, unsigned fields,
                       const struct ext4_timestamp times[3]);
/* Encode an already-published runtime snapshot without publishing it again.
 * A background caller uses wait=false; EAGAIN leaves all fields dirty. */
int ext4_file_writeback_times(ext4_file *file, bool wait);
/* Preserve inode identity after unlink; update permission bits and ctime in one
 * transaction, with the same file error owner as other metadata mutations. */
int ext4_file_set_mode(ext4_file *file, uint32_t mode);
int ext4_file_capability(ext4_file *,void *,size_t,size_t *);
int ext4_file_remove_privileges(ext4_file *,uint32_t mode);
/* Live inode owner update, set-ID/capability removal and ctime share one
 * operation transaction. UINT32_MAX preserves the corresponding ID. */
int ext4_file_set_owner(ext4_file *file, uint32_t uid, uint32_t gid, uint32_t privilege_mode);
/* One operation for up to EXT4_BLOCK_BATCH_MAX ordered, disjoint file ranges. Sources are
 * borrowed on entry; a retained owner is released after its last full block's
 * ordered commit or abort. Failed mounts retain accepted versions for readers.
 * source_buffer/source_size optionally cover complete filesystem blocks around
 * buffer. They contain current bytes, including zeros beyond logical EOF; only
 * offset/size advance file size and completion. An ordinary buffer (empty source)
 * is copied into a private version. */
int ext4_fpwrite_batch(ext4_file *file, struct file_write_span *spans, unsigned count);
int ext4_fpwrite(ext4_file *file, uint64_t offset, const void *buffer, size_t size, size_t *written);
int ext4_file_sync_metadata_mode(ext4_file *file, bool data_only);
/* Flush non-grouped metadata synchronously, or capture and request one fixed
 * grouped-journal durable target without waiting for it. Zero means complete. */
int ext4_file_sync_metadata_prepare(ext4_file *file, bool data_only, uint64_t *target);
/* Snapshot and persist all accepted groups of this mount. */
int ext4_filesystem_sync_metadata(const char *mount_point);

/*****************************DIRECTORY DESCRIPTOR***************************/

/**@brief   Directory entry descriptor. */
typedef struct ext4_direntry {
	uint32_t inode;
	uint16_t entry_length;
	uint8_t name_length;
	uint8_t inode_type;
	uint8_t name[255];
} ext4_direntry;

/**@brief   Directory descriptor. */
typedef struct ext4_dir {
	/**@brief   File descriptor.*/
	ext4_file f;
	/**@brief   Current directory entry.*/
	ext4_direntry de;
	/**@brief   Next entry offset.*/
	uint64_t next_off;
} ext4_dir;

/********************************MOUNT OPERATIONS****************************/

/**@brief   Register block device.
 *
 * @param   bd Block device.
 * @param   dev_name Block device name.
 *
 * @return  Standard error code.*/
int ext4_device_register(struct ext4_blockdev *bd,
			 const char *dev_name);

/**@brief   Un-register block device.
 *
 * @param   dev_name Block device name.
 *
 * @return  Standard error code.*/
int ext4_device_unregister(const char *dev_name);

/**@brief   Un-register all block devices.
 *
 * @return  Standard error code.*/
int ext4_device_unregister_all(void);

/**@brief   Mount a block device with EXT4 partition to the mount point.
 *
 * @param   dev_name Block device name (@ref ext4_device_register).
 * @param   mount_point Mount point, for example:
 *          -   /
 *          -   /my_partition/
 *          -   /my_second_partition/
 * @param   read_only mount as read-only mode.
 *
 * @return Standard error code */
int ext4_mount(const char *dev_name,
	       const char *mount_point,
	       bool read_only);

/**@brief   Umount operation.
 *
 * @param   mount_point Mount point.
 *
 * @return  Standard error code */
int ext4_umount(const char *mount_point);

/**@brief   Starts journaling. Journaling start/stop functions are transparent
 *          and might be used on filesystems without journaling support.
 * @warning Usage:
 *              ext4_mount("sda1", "/");
 *              ext4_journal_start("/");
 *
 *              //File operations here...
 *
 *              ext4_journal_stop("/");
 *              ext4_umount("/");
 * @param   mount_point Mount point.
 *
 * @return  Standard error code. */
int ext4_journal_start(const char *mount_point);

/**@brief   Stops journaling. Journaling start/stop functions are transparent
 *          and might be used on filesystems without journaling support.
 *
 * @param   mount_point Mount point name.
 *
 * @return  Standard error code. */
int ext4_journal_stop(const char *mount_point);

/* Group modifications into one journal transaction. Calls nest; a failed
 * inner transaction poisons the outer transaction, which restores its in-memory
 * state when its last level ends. The caller holds the actual directory,
 * inode and resource qualifications for its operation; independent tasks own
 * independent contexts. These explicit grouping APIs require an active journal. After an outer
 * abort, reopen file handles modified within that group before reusing their
 * cached size/position. Passing zero to abort reports ECANCELED. */
int ext4_transaction_begin(const char *mount_point);
int ext4_transaction_end(const char *mount_point);
int ext4_transaction_abort(const char *mount_point, int error);
/* Execute a complete atomic metadata operation. The callback must reset its
 * temporary handles and outputs on every invocation. Only a hard journal-budget
 * denial after full outer rollback can wait for older checkpoint owners and
 * replay the callback; nesting preserves the enclosing owner's retry duty.
 * Nonjournal callbacks keep their own existing operation boundaries and partial
 * error semantics; they have no journal rollback or budget replay. */
int ext4_metadata_execute(const char *mount_point, int (*apply)(void *), void *context);

/**@brief   Journal recovery.
 * @warning Must be called after @ref ext4_mount.
 *
 * @param   mount_point Mount point.
 *
 * @return Standard error code. */
int ext4_recover(const char *mount_point);

/**@brief   Some of the filesystem stats. */
struct ext4_mount_stats {
	uint32_t inodes_count;
	uint32_t free_inodes_count;
	uint64_t blocks_count; /* Raw filesystem blocks, including overhead. */
	uint64_t free_blocks_count;
	uint64_t overhead_blocks; /* Validated static metadata and internal journal. */
	uint64_t reserved_blocks_count;
	uint8_t uuid[16];

	uint32_t block_size;
	uint32_t block_group_count;
	uint32_t blocks_per_group;
	uint32_t inodes_per_group;

	char volume_name[16];
};

/**@brief   Get file mount point stats.
 * Static overhead is calculated on the first successful query and cached for
 * this mount (online resize is not supported). Allocation counts are current
 * in-memory superblock values. On error the caller's output is unchanged.
 *
 * @param   mount_point Mount point.
 * @param   stats Filesystem stats.
 *
 * @return Standard error code. */
int ext4_mount_point_stats(const char *mount_point,
			   struct ext4_mount_stats *stats);

/**@brief   Setup OS lock routines.
 *
 * @param   mount_point Mount point.
 * @param   locks  Lock and unlock functions
 *
 * @return Standard error code. */
int ext4_mount_setup_locks(const char *mount_point,
			   const struct ext4_lock *locks);

/**@brief   Acquire the filesystem superblock pointer of a mp.
 *
 * @param   mount_point Mount point.
 * @param   sb Superblock handle
 *
 * @return Standard error code. */
int ext4_get_sblock(const char *mount_point, struct ext4_sblock **sb);

/**@brief   Enable/disable write back cache mode.
 * @warning Default model of cache is write trough. It means that when You do:
 *
 *          ext4_fopen(...);
 *          ext4_fwrite(...);
 *                           < --- data is flushed to physical drive
 *
 *          When you do:
 *          ext4_cache_write_back(..., 1);
 *          ext4_fopen(...);
 *          ext4_fwrite(...);
 *                           < --- data is NOT flushed to physical drive
 *          ext4_cache_write_back(..., 0);
 *                           < --- when write back mode is disabled all
 *                                 cache data will be flushed
 * To enable write back mode permanently just call this function
 * once after ext4_mount (and disable before ext4_umount).
 *
 * Some of the function use write back cache mode internally.
 * If you enable write back mode twice you have to disable it twice
 * to flush all data:
 *
 *      ext4_cache_write_back(..., 1);
 *      ext4_cache_write_back(..., 1);
 *
 *      ext4_cache_write_back(..., 0);
 *      ext4_cache_write_back(..., 0);
 *
 * Write back mode is useful when you want to create a lot of empty
 * files/directories.
 *
 * @param   path Mount point.
 * @param   on Enable/disable cache writeback mode.
 *
 * @return Standard error code. */
int ext4_cache_write_back(const char *path, bool on);


/**@brief   Force cache flush.
 *
 * @param   path Mount point.
 *
 * @return  Standard error code. */
int ext4_cache_flush(const char *path);

/********************************FILE OPERATIONS*****************************/

/**@brief   Remove file by path.
 *
 * @param   path Path to file.
 *
 * @return  Standard error code. */
int ext4_fremove(const char *path);

struct ext4_rename_result {
	uint32_t replaced_inode;
	bool replaced_last_link;
	bool changed;
};

/* Rename one directory entry without resolving an absolute pathname. Flags
 * are 0 or 1 (NOREPLACE). Requires an active journal and caller-held
 * namespace qualifications for the affected directories and inodes. A replaced inode keeps its contents; its last link is transferred
 * to the persistent orphan owner until the caller releases live references.
 * Results are published only on success. Within an outer transaction they
 * remain provisional until that transaction commits. */
int ext4_rename_child(const char *mount_point,
	uint32_t old_parent, const char *old_name, uint32_t old_len,
	uint32_t new_parent, const char *new_name, uint32_t new_len,
	unsigned flags, struct ext4_rename_result *result);

/**@brief   Remove directory entry by path and decrement inode links count,
 *          without truncating blocks or freeing inode bitmap.
 *
 * @param   path Path to file.
 * @param   out_inode Pointer to receive unlinked inode number (optional, can be NULL).
 * @param   out_is_orphan Pointer to receive bool indicating if links_cnt became 0 (optional, can be NULL).
 *
 * @return  Standard error code. */
int ext4_funlink_dentry(const char *path, uint32_t *out_inode, bool *out_is_orphan);

/**@brief   Free an orphan inode whose link count is zero (truncate blocks and free inode).
 *
 * @param   path Mount point or file path to identify mount.
 * @param   inode Inode number to truncate and free.
 *
 * @return  Standard error code. */
int ext4_orphan_free(const char *path, uint32_t inode);
/* Replay must finish first; recovery completes before opening the mount. */
int ext4_orphan_recover(const char *mount_point);

/**@brief   Create a hardlink for a file.
 *
 * @param   path Path to file.
 * @param   hardlink_path Path of hardlink.
 *
 * @return  Standard error code. */
/* Link an existing inode without resolving a potentially stale source path. */
int ext4_link_child(const char *mount_point, uint32_t source_inode,
                   uint32_t parent_inode, const char *name, uint32_t length);

int ext4_flink(const char *path, const char *hardlink_path);

/**@brief Rename file.
 * @param path Source.
 * @param new_path Destination.
 * @return  Standard error code. */
int ext4_frename(const char *path, const char *new_path);

/**@brief   File open function.
 *
 * @param   file  File handle.
 * @param   path  File path, has to start from mount point:/my_partition/file.
 * @param   flags File open flags.
 *  |---------------------------------------------------------------|
 *  |   r or rb                 O_RDONLY                            |
 *  |---------------------------------------------------------------|
 *  |   w or wb                 O_WRONLY|O_CREAT|O_TRUNC            |
 *  |---------------------------------------------------------------|
 *  |   a or ab                 O_WRONLY|O_CREAT|O_APPEND           |
 *  |---------------------------------------------------------------|
 *  |   r+ or rb+ or r+b        O_RDWR                              |
 *  |---------------------------------------------------------------|
 *  |   w+ or wb+ or w+b        O_RDWR|O_CREAT|O_TRUNC              |
 *  |---------------------------------------------------------------|
 *  |   a+ or ab+ or a+b        O_RDWR|O_CREAT|O_APPEND             |
 *  |---------------------------------------------------------------|
 *
 * @return  Standard error code.*/
int ext4_fopen(ext4_file *file, const char *path, const char *flags);

/**@brief   Alternate file open function.
 *
 * @param   file  File handle.
 * @param   path  File path, has to start from mount point:/my_partition/file.
 * @param   flags File open flags.
 *
 * @return  Standard error code.*/
int ext4_fopen2(ext4_file *file, const char *path, int flags);

/**@brief   File close function.
 *
 * @param   file File handle.
 *
 * @return  Standard error code.*/
int ext4_fclose(ext4_file *file);


/**@brief   File truncate function.
 *
 * @param   file File handle.
 * @param   size New file size.
 *
 * @return  Standard error code.*/
int ext4_ftruncate(ext4_file *file, uint64_t size, uint32_t privilege_mode);
#define EXT4_FALLOC_KEEP_SIZE 1U
#define EXT4_FALLOC_PUNCH_HOLE 2U
/* changed reports committed mapping/data changes even on a partial failure. */
int ext4_fallocate(ext4_file *file, unsigned mode, uint64_t offset,
                   uint64_t length, bool *changed, uint32_t privilege_mode);
/* Prepare storage without publishing EOF or file bytes. A committed prefix
 * remains usable when a later range cannot be allocated. If complete is set,
 * write back accepted bytes before calling complete_write to trim unused
 * initialized legacy mappings; retain this duty when either operation fails. */
int ext4_file_prepare_write(ext4_file *file, uint64_t offset, size_t length,
                            size_t *ready, bool *complete);
int ext4_file_complete_write(ext4_file *file);
/* Adapter owns the live inode I/O writer; global accounting still uses STATE. */
int ext4_file_reserve_write(ext4_file *, uint64_t, size_t, size_t *, void **ticket);
void ext4_file_accept_reservation(ext4_file *, void *ticket, uint64_t accepted_end);
void ext4_file_finish_reservation(ext4_file *, void *ticket, uint64_t accepted_end);
void ext4_file_release_reservations(ext4_file *, uint64_t size);
void ext4_file_drain_reservations(ext4_file *);


/**@brief   Read data from file.
 *
 * @param   file File handle.
 * @param   buf  Output buffer.
 * @param   size Bytes to read.
 * @param   rcnt Bytes read (NULL allowed).
 *
 * @return  Standard error code.*/
int ext4_fread(ext4_file *file, void *buf, size_t size, size_t *rcnt);
/* Positioned data read. The caller stabilizes the inode mapping; returned
 * bytes borrow exact payload versions while accepted cache versions advance. */
int ext4_fpread(const ext4_file *file, uint64_t offset, void *buf, size_t size, size_t *rcnt);
/* At most FILE_READ_BATCH_MAX disjoint outputs; every span reports a contiguous prefix.
 * Reads consult cache versions, including dirty and journal-pending data.
 * Multi-output callers lend a workspace until return; scalar callers may pass
 * NULL. size_limit caps visible EOF without changing the caller's span sizes. */
int ext4_fpread_batch(const ext4_file *file, struct file_read_span *spans, unsigned count,
    struct file_read_workspace *workspace, uint64_t size_limit, const struct file_read_completion *completion);


/**@brief   Write data to file.
 *
 * @param   file File handle.
 * @param   buf  Data to write
 * @param   size Write length..
 * @param   wcnt Bytes written (NULL allowed).
 *
 * @return  Standard error code.*/
int ext4_fwrite(ext4_file *file, const void *buf, size_t size, size_t *wcnt);

/**@brief   File seek operation.
 *
 * @param   file File handle.
 * @param   offset Offset to seek. SEEK_SET accepts positions beyond EOF
 *                 without changing the file size.
 * @param   origin Seek type:
 *              @ref SEEK_SET
 *              @ref SEEK_CUR
 *              @ref SEEK_END
 *
 * @return  Standard error code.*/
int ext4_fseek(ext4_file *file, int64_t offset, uint32_t origin);

/**@brief   Get file position.
 *
 * @param   file File handle.
 *
 * @return  Actual file position */
uint64_t ext4_ftell(ext4_file *file);

/**@brief   Get file size.
 *
 * @param   file File handle.
 *
 * @return  File size. */
uint64_t ext4_fsize(ext4_file *file);


/**@brief Get inode of file/directory/link.
 *
 * @param path    Parh to file/dir/link.
 * @param ret_ino Inode number.
 * @param inode   Inode internals.
 *
 * @return  Standard error code.*/
int ext4_raw_inode_fill(const char *path, uint32_t *ret_ino,
			struct ext4_inode *inode);

/**@brief Copy the raw inode addressed by an open file handle.
 *
 * Unlike the path form, this continues to work after the final directory
 * entry has been unlinked while the handle still owns the inode.
 */
int ext4_fraw_inode_fill(const ext4_file *file, struct ext4_inode *inode);

/* Submit the held inode's metadata buffer, without draining unrelated files.
 * Caller must have submitted data first and issue the device barrier after. */
int ext4_file_sync_metadata(ext4_file *file);

/**@brief Check if inode exists.
 *
 * @param path    Parh to file/dir/link.
 * @param type    Inode type.
 *                @ref EXT4_DIRENTRY_UNKNOWN
 *                @ref EXT4_DE_REG_FILE
 *                @ref EXT4_DE_DIR
 *                @ref EXT4_DE_CHRDEV
 *                @ref EXT4_DE_BLKDEV
 *                @ref EXT4_DE_FIFO
 *                @ref EXT4_DE_SOCK
 *                @ref EXT4_DE_SYMLINK
 *
 * @return  Standard error code.*/
int ext4_inode_exist(const char *path, int type);

/**@brief Change file/directory/link mode bits.
 *
 * @param path Path to file/dir/link.
 * @param mode New mode bits (for example 0777).
 *
 * @return  Standard error code.*/
int ext4_mode_set(const char *path, uint32_t mode);
/* Initial directory permissions preserve the inherited setgid bit. */
int ext4_mode_set_initial(const char *path, uint32_t mode);


/**@brief Get file/directory/link mode bits.
 *
 * @param path Path to file/dir/link.
 * @param mode New mode bits (for example 0777).
 *
 * @return  Standard error code.*/
int ext4_mode_get(const char *path, uint32_t *mode);

/**@brief Change file owner and group.
 *
 * @param path Path to file/dir/link.
 * @param uid  User id.
 * @param gid  Group id.
 *
 * @return  Standard error code.*/
int ext4_owner_set(const char *path, uint32_t uid, uint32_t gid);

/**@brief Get file/directory/link owner and group.
 *
 * @param path Path to file/dir/link.
 * @param uid  User id.
 * @param gid  Group id.
 *
 * @return  Standard error code.*/
int ext4_owner_get(const char *path, uint32_t *uid, uint32_t *gid);

/**@brief Set file/directory/link access time.
 *
 * @param path  Path to file/dir/link.
 * @param atime Access timestamp.
 *
 * @return  Standard error code.*/
int ext4_atime_set(const char *path, uint32_t atime);

/**@brief Set file/directory/link modify time.
 *
 * @param path  Path to file/dir/link.
 * @param mtime Modify timestamp.
 *
 * @return  Standard error code.*/
int ext4_mtime_set(const char *path, uint32_t mtime);

/**@brief Set file/directory/link change time.
 *
 * @param path  Path to file/dir/link.
 * @param ctime Change timestamp.
 *
 * @return  Standard error code.*/
int ext4_ctime_set(const char *path, uint32_t ctime);

/**@brief Get file/directory/link access time.
 *
 * @param path  Path to file/dir/link.
 * @param atime Access timestamp.
 *
 * @return  Standard error code.*/
int ext4_atime_get(const char *path, uint32_t *atime);

/**@brief Get file/directory/link modify time.
 *
 * @param path  Path to file/dir/link.
 * @param mtime Modify timestamp.
 *
 * @return  Standard error code.*/
int ext4_mtime_get(const char *path, uint32_t *mtime);

/**@brief Get file/directory/link change time.
 *
 * @param path  Pathto file/dir/link.
 * @param ctime Change timestamp.
 *
 * @return  standard error code*/
int ext4_ctime_get(const char *path, uint32_t *ctime);

/**@brief Create symbolic link.
 *
 * @param target Destination entry path.
 * @param path   Source entry path.
 *
 * @return  Standard error code.*/
int ext4_fsymlink(const char *target, const char *path);

/**@brief Create special file.
 * @param path     Path to new special file.
 * @param filetype Filetype of the new special file.
 * 	           (that must not be regular file, directory, or unknown type)
 * @param dev      If filetype is char device or block device,
 * 	           the device number will become the payload in the inode.
 * @return  Standard error code.*/
int ext4_mknod(const char *path, int filetype, uint32_t dev);

/**@brief Read symbolic link payload.
 *
 * @param path    Path to symlink.
 * @param buf     Output buffer.
 * @param bufsize Output buffer max size.
 * @param rcnt    Bytes read.
 *
 * @return  Standard error code.*/
int ext4_readlink(const char *path, char *buf, size_t bufsize, size_t *rcnt);

/**@brief Set extended attribute.
 *
 * @param path      Path to file/directory
 * @param name      Name of the entry to add.
 * @param name_len  Length of @name in bytes.
 * @param data      Data of the entry to add.
 * @param data_size Size of data to add.
 *
 * @return  Standard error code.*/
int ext4_setxattr(const char *path, const char *name, size_t name_len,
		  const void *data, size_t data_size);

/**@brief Get extended attribute.
 *
 * @param path      Path to file/directory.
 * @param name      Name of the entry to get.
 * @param name_len  Length of @name in bytes.
 * @param buf      Data of the entry to get.
 * @param buf_size Size of data to get.
 *
 * @return  Standard error code.*/
int ext4_getxattr(const char *path, const char *name, size_t name_len,
		  void *buf, size_t buf_size, size_t *data_size);

/**@brief List extended attributes.
 *
 * @param path     Path to file/directory.
 * @param list     List to hold the name of entries.
 * @param size     Size of @list in bytes.
 * @param ret_size Used bytes of @list.
 *
 * @return  Standard error code.*/
int ext4_listxattr(const char *path, char *list, size_t size, size_t *ret_size);

/**@brief Remove extended attribute.
 *
 * @param path     Path to file/directory.
 * @param name     Name of the entry to remove.
 * @param name_len Length of @name in bytes.
 *
 * @return  Standard error code.*/
int ext4_removexattr(const char *path, const char *name, size_t name_len);


/*********************************DIRECTORY OPERATION***********************/

/**@brief   Recursive directory remove.
 *
 * @param   path Directory path to remove
 *
 * @return  Standard error code.*/
int ext4_dir_rm(const char *path);

/**@brief Rename/move directory.
 *
 * @param path     Source path.
 * @param new_path Destination path.
 *
 * @return  Standard error code. */
int ext4_dir_mv(const char *path, const char *new_path);

/**@brief   Create new directory.
 *
 * @param   path Directory name.
 *
 * @return  Standard error code.*/
int ext4_dir_mk(const char *path);

/**@brief   Directory open.
 *
 * @param   dir  Directory handle.
 * @param   path Directory path.
 *
 * @return  Standard error code.*/
int ext4_dir_open(ext4_dir *dir, const char *path);
int ext4_dir_open_file(ext4_file *file, const char *path);
int ext4_chrdev_open_file(ext4_file *file, const char *path);
int ext4_fopen_inode(ext4_file *file, const char *mount_point,
                     uint32_t inode_number, struct ext4_inode *initial);
/* The file output is published only on success. Optional initial attributes
 * are caller scratch filled before releasing the new inode reference, not a
 * disk/checksum image. Discard them on failure; a successful result remains
 * provisional until an outer transaction accepts it. */
int ext4_create_child(const char *mount, uint32_t parent, const char *name,
    uint32_t mode, uint32_t device, const char *target, ext4_file *output,
    struct ext4_inode *initial,uint32_t uid,uint32_t gid);
int ext4_unlink_child(const char *mount, uint32_t parent, const char *name,
    bool directory, uint32_t *inode, bool *orphan);
int ext4_lookup_child(const char *mount_point, uint32_t parent_inode,
                      const char *name, uint32_t name_length,
                      uint32_t *child_inode, uint32_t *child_mode);
int ext4_readlink_inode(const char *mount_point, uint32_t inode_number,
                        char *buffer, size_t capacity, size_t *bytes_read);
int ext4_fdir_unlink_dentry(const char *path, uint32_t *out_inode);

/**@brief   Directory close.
 *
 * @param   dir directory handle.
 *
 * @return  Standard error code.*/
int ext4_dir_close(ext4_dir *dir);

/**@brief   Return next directory entry.
 *
 * @param   dir Directory handle.
 *
 * @return  Directory entry id (NULL if no entry)*/
const ext4_direntry *ext4_dir_entry_next(ext4_dir *dir);

/**@brief Return one directory entry and preserve iterator errors.
 *
 * @param dir Directory handle. `next_off` is the byte position to inspect.
 * @param entry Destination entry descriptor.
 * @param entry_offset Receives the entry's byte position, or UINT64_MAX at
 *        end of directory.
 * @return EOK for an entry or end of directory, otherwise an ext4 error.
 */
int ext4_dir_entry_next_status(ext4_dir *dir,
                               ext4_direntry *entry,
                               uint64_t *entry_offset);

/**@brief   Rewine directory entry offset.
 *
 * @param   dir Directory handle.*/
void ext4_dir_entry_rewind(ext4_dir *dir);


#ifdef __cplusplus
}
#endif

#endif /* EXT4_H_ */

/**
 * @}
 */
