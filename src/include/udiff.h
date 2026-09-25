/* SPDX-License-Identifier: GPL-2.0-only */
/* Exact line comparison and unified diff output */
#ifndef UDIFF_H
#define UDIFF_H

#include <stddef.h>
#include <stdio.h>

#include <patch-types.h>
#include <types.h>

/*
 * One line of a source image, including its trailing LF when present. Use the
 * length, since embedded NUL bytes are ordinary input here. Only the final line
 * may lack LF; its absence supplies the no-final-newline marker. Every line has
 * at least one byte, so an empty file has no lines.
 */
struct udiff_line {
	const char *ptr; /* line bytes; may span embedded NUL */
	size_t len; /* incl. trailing LF when present */
};

/*
 * An operand image: the materialized line table of one file. An empty file is
 * nlines == 0. The engine borrows the line bytes for the duration of the call
 * and never frees them. When bytes is set, it holds the concatenated line bytes
 * in one object; otherwise the engine materializes the line sequence.
 */
struct udiff_image {
	const struct udiff_line *lines;
	size_t nlines;
	const char *bytes;
};

/* Owned unified hunks without file headers; equal inputs produce none */
struct udiff_result {
	char *out_buf;
	size_t out_len;
	unsigned long hunks; /* emitted hunk count */
	bool equal; /* no differences found */
};

/*
 * Find a definition at or before the hunk's zero-based first old row (or
 * insertion gap). Return a borrowed pointer and length, valid until the next
 * callback, or NULL for no label. Labels have no CR or LF and fit the payload
 * ceiling.
 */
typedef const char *(*udiff_name_fn)(void *ctx, unsigned long old_lineno,
				     size_t *len_out);

/* Compute one complete diff; context width affects only its presentation */
void udiff_run(const struct udiff_image *a, const struct udiff_image *b,
	       unsigned int context, udiff_name_fn name_of, void *name_ctx,
	       struct udiff_result *out);

void udiff_result_free(struct udiff_result *out);

/* side[leg][row] is the opposite view's row, or SIZE_MAX if unmatched */
struct udiff_matches {
	size_t *side[2];
};

void udiff_match(const struct udiff_image *old, const struct udiff_image *new,
		 struct udiff_matches *out);

/* Patience uses unique anchors but may also match repeated lines around them */
void udiff_match_unique(const struct udiff_image *old,
			const struct udiff_image *new,
			struct udiff_matches *out);
void udiff_matches_free(struct udiff_matches *matches);

#endif /* UDIFF_H */
