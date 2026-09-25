// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ctrl IQ, Inc.
 *
 * The tree-entry differ behind --git-tree. Changed tree entries arrive in path-
 * byte order from the object reader, so the diff is one merge walk: a path on
 * one side alone records as a create or delete, a path on both sides records
 * only when its mode or object differs, and a path whose entry kind changes
 * records as a delete and a create at the same path. Moves also remain separate
 * deletions and creations, preserving their complete source content.
 */
#include <stdlib.h>
#include <string.h>

#include <gitread.h>
#include <treediff.h>
#include <util.h>

/* The type half of a git tree-entry mode; the object format defines these */
#define MODE_TYPE_MASK 0170000
#define MODE_TYPE_REG 0100000
#define MODE_TYPE_SYMLINK 0120000
#define MODE_TYPE_GITLINK 0160000

/*
 * Only type bits determine the entry kind. Historical permissions such as
 * 100664 still classify as regular files while records retain their raw mode.
 * The reader traverses directories, so they cannot reach this point.
 */
enum treediff_kind treediff_mode_kind(const char *path, u32 mode)
{
	switch (mode & MODE_TYPE_MASK) {
	case MODE_TYPE_REG:
		return TREEDIFF_REG;

	case MODE_TYPE_SYMLINK:
		return TREEDIFF_SYMLINK;

	case MODE_TYPE_GITLINK:
		return TREEDIFF_GITLINK;
	}

	die("the tree entry at %s carries mode %o, which this differ does not implement",
	    path, mode);
}

static void side_set(struct treediff_side *side,
		     const struct gitread_tree_entry *entry)
{
	side->oid = entry->oid;
	side->mode = entry->mode;
	side->kind = treediff_mode_kind(entry->path, entry->mode);
}

/* Grows the record array geometrically; the walk cannot know its count */
static struct treediff_rec *rec_push(struct treediff_map *map, size_t *cap,
				     const char *path, enum treediff_op op)
{
	struct treediff_rec *rec;

	if (map->n == *cap) {
		*cap = *cap ? *cap * 2 : 32;
		map->recs = xrealloc_array(map->recs, *cap, sizeof(*rec));
	}

	rec = &map->recs[map->n++];
	*rec = (typeof(*rec)){ .path = xstrdup(path), .op = op };
	return rec;
}

void treediff_build(struct gitread *gr, const struct gitread_oid *old_commit,
		    const struct gitread_oid *new_commit,
		    struct treediff_map *out)
{
	struct gitread_tree old_tree = {};
	struct gitread_tree new_tree = {};
	struct treediff_rec *rec;
	size_t cap = 0;

	*out = (typeof(*out)){};

	if (old_commit)
		gitread_tree_read(gr, old_commit, new_commit, &old_tree);

	gitread_tree_read(gr, new_commit, old_commit, &new_tree);

	for (size_t i = 0, j = 0; i < old_tree.n || j < new_tree.n;) {
		const struct gitread_tree_entry *o =
			i < old_tree.n ? &old_tree.entries[i] : NULL;
		const struct gitread_tree_entry *n =
			j < new_tree.n ? &new_tree.entries[j] : NULL;
		int cmp;

		/*
		 * Exhausting one list leaves every entry in the other
		 * unmatched.
		 */
		if (!o)
			cmp = 1;
		else if (!n)
			cmp = -1;
		else
			cmp = strcmp(o->path, n->path);

		if (cmp < 0) {
			rec = rec_push(out, &cap, o->path, TREEDIFF_DELETE);
			side_set(&rec->old, o);
			i++;
		} else if (cmp > 0) {
			rec = rec_push(out, &cap, n->path, TREEDIFF_CREATE);
			side_set(&rec->new, n);
			j++;
		} else if (treediff_mode_kind(o->path, o->mode) !=
			   treediff_mode_kind(n->path, n->mode)) {
			rec = rec_push(out, &cap, o->path, TREEDIFF_DELETE);
			side_set(&rec->old, o);
			rec = rec_push(out, &cap, n->path, TREEDIFF_CREATE);
			side_set(&rec->new, n);
			i++;
			j++;
		} else {
			rec = rec_push(out, &cap, o->path, TREEDIFF_MODIFY);
			side_set(&rec->old, o);
			side_set(&rec->new, n);

			i++;
			j++;
		}
	}

	gitread_tree_free(&old_tree);
	gitread_tree_free(&new_tree);
}

void treediff_map_free(struct treediff_map *map)
{
	for (size_t i = 0; i < map->n; i++)
		free(map->recs[i].path);

	free(map->recs);
	map->recs = NULL;
	map->n = 0;
}
