/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 Ctrl IQ, Inc.
 *
 * Keep one repository and both resolved revisions available for comparison.
 */
#ifndef GITTREE_H
#define GITTREE_H

#include <iomem.h>

/* Which operand's map a lookup reads */
enum gittree_leg {
	GITTREE_PATCH1,
	GITTREE_PATCH2
};

/*
 * Opens the repository at dir, resolves both operands, derives each side's
 * patch with the specified number of context lines, and installs the run as the
 * active one gittree_active() reports. An operand is a commit-ish derived
 * against its first parent (a parentless commit derives against the empty
 * tree), or BASE..TIP naming which parent of TIP to derive against instead; a
 * BASE that isn't a parent of TIP is rejected. Every resolution or derivation
 * failure dies here, before anything reaches stdout.
 */
void gittree_begin(const char *dir, const char *rev1, const char *rev2,
		   unsigned int context, struct iomem_buf *doc1,
		   struct iomem_buf *doc2);

/* True between gittree_begin() and gittree_end() */
bool gittree_active(void);

/* The caller owns the subject of the already resolved result commit */
char *gittree_commit_subject(enum gittree_leg leg);

/* Read this operand's source even when its derived patch omits the file */
bool gittree_source_bytes(enum gittree_leg leg, const char *path, bool result,
			  struct iomem_buf *out);

/* Tears the active run down; harmless when no run is active */
void gittree_end(void);

#endif /* GITTREE_H */
