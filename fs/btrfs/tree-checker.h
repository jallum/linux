/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) Qu Wenruo 2017.  All rights reserved.
 */

#ifndef BTRFS_TREE_CHECKER_H
#define BTRFS_TREE_CHECKER_H

#include "ctree.h"
#include "extent_io.h"

/*
 * Comprehensive leaf checker.
 * Will check not only the item pointers, but also every possible member
 * in item data.
 */
int btrfs_check_leaf_full(struct btrfs_fs_info *fs_info,
			  struct extent_buffer *leaf);

/*
 * Less strict leaf checker.
 * Will only check item pointers, not reading item data.
 */
int btrfs_check_leaf_relaxed(struct btrfs_fs_info *fs_info,
			     struct extent_buffer *leaf);

/*
 * Write time specific leaf checker.
 * Don't check if the empty leaf belongs to a tree root. Mostly for balance
 * and new tree created in current transaction.
 */
int btrfs_check_leaf_write(struct btrfs_fs_info *fs_info,
			   struct extent_buffer *leaf);
int btrfs_check_node(struct btrfs_fs_info *fs_info, struct extent_buffer *node);

#endif
