/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 Ctrl IQ, Inc.
 *
 * Read commits, trees, and blobs without checking out revisions. Repository
 * lookup uses libgit2; these interfaces expose owned bytes and object IDs
 * without exposing library handles to callers.
 */
#ifndef GITREAD_H
#define GITREAD_H

#include <iomem.h>
#include <types.h>

/*
 * A raw SHA-1 object id. The reader refuses repositories using any other object
 * format at open, so the width is a constant of the interface rather than a
 * property of the handle.
 */
#define GITREAD_OID_RAWSZ 20
#define GITREAD_OID_HEXSZ 40

struct gitread_oid {
	u8 raw[GITREAD_OID_RAWSZ];
};

struct gitread;

/*
 * Open dir as a working or bare repository, without searching its ancestors.
 * Reject unsupported formats and history features before lookup. Read failures
 * terminate the program; path lookups return false for missing entries.
 */
void gitread_open(struct gitread **out, const char *dir);
void gitread_close(struct gitread **gr);

/*
 * Resolve a unique 4-to-40-digit hex object ID or a full/shorthand reference
 * name (HEAD, refs/heads/x, or x). Hex names try the object store first, then
 * references if no object matches. Follow symbolic references and annotated
 * tags to a commit; ambiguous prefixes and non-commit objects are errors.
 */
void gitread_resolve_commit(struct gitread *gr, const char *name,
			    struct gitread_oid *out);

/* Returns an owned copy of the commit's first paragraph, folded to one line */
char *gitread_commit_subject(struct gitread *gr,
			     const struct gitread_oid *commit);

/*
 * Return the parent count and optionally copy the first parent's ID. A root
 * commit returns 0 and leaves first_parent untouched.
 */
int gitread_commit_parents(struct gitread *gr, const struct gitread_oid *commit,
			   struct gitread_oid *first_parent);

/*
 * Test whether candidate is a parent of tip. BASE..TIP uses this to select a
 * merge parent; arbitrary ranges aren't accepted.
 */
bool gitread_commit_parent_of(struct gitread *gr, const struct gitread_oid *tip,
			      const struct gitread_oid *candidate);

/*
 * Looks up a file entry without loading its object, which may belong to another
 * repository for a gitlink. Returns its ID and raw mode, or false if absent or
 * replaced by a directory. Directory contents have their own file entries.
 */
bool gitread_entry_by_path(struct gitread *gr, const struct gitread_oid *commit,
			   const char *path, struct gitread_oid *id, u32 *mode);

/*
 * Allocate the blob's bytes with iomem's trailing NUL and copy its raw mode.
 * Return false for a missing path. Directories, gitlinks, and oversized blobs
 * are errors. Symlinks return their target-path bytes and aren't followed.
 */
bool gitread_blob_by_path(struct gitread *gr, const struct gitread_oid *commit,
			  const char *path, struct iomem_buf *out, u32 *mode);

/*
 * Allocate the named blob's bytes with iomem's trailing NUL. The caller already
 * has its tree entry, so path is used only in diagnostics. A missing object,
 * non-blob object, or oversized blob is an error.
 */
void gitread_blob_by_oid(struct gitread *gr, const struct gitread_oid *id,
			 const char *path, struct iomem_buf *out);

/*
 * A non-directory entry with its full path, raw mode, and object ID. Symlinks
 * and gitlinks retain their own modes; the tree differ decides how to compare
 * them.
 */
struct gitread_tree_entry {
	char *path;
	struct gitread_oid oid;
	u32 mode;
};

struct gitread_tree {
	struct gitread_tree_entry *entries;
	size_t n;
};

/*
 * Read commit's entries differing in id or raw mode from other_commit, or the
 * whole tree if it is NULL. Entries found only in other_commit are omitted.
 * Equal subtrees need no traversal. Sort by path bytes and keep historical
 * permission spellings. Every visited tree must be readable. The caller
 * supplies an empty output.
 */
void gitread_tree_read(struct gitread *gr, const struct gitread_oid *commit,
		       const struct gitread_oid *other_commit,
		       struct gitread_tree *out);
void gitread_tree_free(struct gitread_tree *tree);

/* Formats oid as 40 lowercase hex characters plus the terminating NUL */
void gitread_oid_hex(const struct gitread_oid *oid,
		     char hex[GITREAD_OID_HEXSZ + 1]);

#endif /* GITREAD_H */
