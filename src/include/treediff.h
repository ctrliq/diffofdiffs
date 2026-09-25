/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 Ctrl IQ, Inc.
 *
 * The tree-entry differ behind --git-tree: compare two commits' flattened trees
 * and build the path map the patch assembler consumes. The map records changed
 * paths alone, since unchanged entries produce no patch.
 */
#ifndef TREEDIFF_H
#define TREEDIFF_H

#include <gitread.h>
#include <types.h>

enum treediff_op {
	TREEDIFF_CREATE,
	TREEDIFF_DELETE,
	TREEDIFF_MODIFY
};

/*
 * The entry kinds a record may carry. Directories never appear, since the
 * differ walks through them and records what they hold; a kind change at one
 * path is a delete record followed by a create record so assembly preserves the
 * full content and mode of both kinds.
 */
enum treediff_kind {
	TREEDIFF_REG,
	TREEDIFF_SYMLINK,
	TREEDIFF_GITLINK
};

/* Classifies file, symlink, and gitlink modes; every other type is refused */
enum treediff_kind treediff_mode_kind(const char *path, u32 mode);

/*
 * One side's state at a record's path. The absent side of a create or delete
 * record stays zeroed; op names which side that is.
 */
struct treediff_side {
	struct gitread_oid oid;
	u32 mode;
	enum treediff_kind kind;
};

struct treediff_rec {
	char *path;
	enum treediff_op op;
	struct treediff_side old;
	struct treediff_side new;
};

struct treediff_map {
	struct treediff_rec *recs;
	size_t n;
};

/*
 * Diffs old_commit's tree against new_commit's tree into out, one record per
 * changed path in path-byte order, with a kind change at one path yielding its
 * delete record before its create record. A null old_commit means the empty
 * tree (the parentless-commit form), so every entry records as a create.
 */
void treediff_build(struct gitread *gr, const struct gitread_oid *old_commit,
		    const struct gitread_oid *new_commit,
		    struct treediff_map *out);
void treediff_map_free(struct treediff_map *map);

DEFINE_FREE(treediff_map, struct treediff_map, treediff_map_free(&_T))

#endif /* TREEDIFF_H */
