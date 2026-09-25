/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 Ctrl IQ, Inc.
 *
 * Convert changed tree entries into the unified diff format consumed by the
 * patch reader.
 */
#ifndef ASSEMBLE_H
#define ASSEMBLE_H

#include <gitread.h>
#include <iomem.h>
#include <treediff.h>

/*
 * Allocate out and write a patch in map order, with the specified number of
 * context lines per hunk. An empty map produces an empty patch. Reject binary
 * content that needs text hunks, and paths the patch reader cannot represent.
 */
void assemble_patch(struct gitread *gr, const struct treediff_map *map,
		    unsigned int context, struct iomem_buf *out);

/*
 * Allocate out and read one present side's contents. A symlink's blob contains
 * its target path; never follow it. A gitlink becomes "Subproject commit <id>"
 * followed by LF, without looking up that commit in the submodule repository.
 * Patch assembly and later source lookup share this spelling.
 */
void assemble_side_bytes(struct gitread *gr, const struct treediff_side *side,
			 const char *path, struct iomem_buf *out);

#endif /* ASSEMBLE_H */
