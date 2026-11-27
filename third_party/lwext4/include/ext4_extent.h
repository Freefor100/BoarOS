/*
 * Copyright (c) 2013 Grzegorz Kostka (kostka.grzegorz@gmail.com)
 *
 * HelenOS:
 * Copyright (c) 2012 Martin Sucha
 * Copyright (c) 2012 Frantisek Princ
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
 * @file  ext4_extent.h
 * @brief More complex filesystem functions.
 */
#ifndef EXT4_EXTENT_H_
#define EXT4_EXTENT_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <ext4_config.h>
#include <ext4_types.h>
#include <ext4_inode.h>

/* A single insertion can allocate one block per level, including root growth. */
enum { EXT4_EXTENT_MAX_DEPTH = 5 };

void ext4_extent_tree_init(struct ext4_inode_ref *inode_ref);
int ext4_extent_remove_tail(struct ext4_inode_ref *inode_ref, uint64_t keep,
    uint32_t window, bool *expanded, bool *done);
/* The group defaults to the caller's group for an empty extent root. */
bool ext4_extent_small_root(struct ext4_inode_ref *inode, uint32_t limit, uint32_t *group);


struct ext4_inode_block_range;
int ext4_extent_get_range(struct ext4_inode_ref *inode_ref, ext4_lblk_t iblock,
    uint32_t max_blocks, bool create, struct ext4_inode_block_range *range);
int ext4_extent_write_range(struct ext4_inode_ref *inode_ref, ext4_lblk_t iblock,
    uint32_t max_blocks, uint32_t overwritten_blocks, struct ext4_inode_block_range *range);
int ext4_extent_complete_write(struct ext4_inode_ref *inode_ref, ext4_lblk_t iblock,
    ext4_fsblk_t physical, uint32_t blocks);

/* Allocate holes as unwritten extents, preserving the state of existing mappings. */
int ext4_extent_reserve_range(struct ext4_inode_ref *inode_ref, ext4_lblk_t iblock,
    uint32_t max_blocks, struct ext4_inode_block_range *range);
/* Make one block run read as zero while retaining or allocating its storage. */
int ext4_extent_zero_range(struct ext4_inode_ref *inode_ref, ext4_lblk_t iblock,
    uint32_t max_blocks, struct ext4_inode_block_range *range);

int ext4_extent_get_blocks(struct ext4_inode_ref *inode_ref, ext4_lblk_t iblock,
			   uint32_t max_blocks, ext4_fsblk_t *result, bool create,
			   uint32_t *blocks_count);


/**@brief Release all data blocks starting from specified logical block.
 * @param inode_ref   I-node to release blocks from
 * @param iblock_from First logical block to release
 * @return Error code */
int ext4_extent_remove_space(struct ext4_inode_ref *inode_ref, ext4_lblk_t from,
			     ext4_lblk_t to);


#ifdef __cplusplus
}
#endif

#endif /* EXT4_EXTENT_H_ */
/**
* @}
*/
