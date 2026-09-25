/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 Ctrl IQ, Inc.
 *
 * Parsed names, file records, and input bounds shared by the reader and engine.
 */
#ifndef PATCH_TYPES_H
#define PATCH_TYPES_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <urcu/list.h>

#include <types.h>

/* The prefix convention established by complete record names */
enum patch_prefix {
	/* No prefix evidence; retain the literal spelling */
	PATCH_PREFIX_UNKNOWN,

	/* A bare creation/deletion needs evidence from the other records */
	PATCH_PREFIX_INCOMPLETE,

	/* Conflicting evidence prevents this record from pairing */
	PATCH_PREFIX_AMBIGUOUS,

	/* Complete names establish literal paths or Git's a/ and b/ prefixes */
	PATCH_PREFIX_LITERAL,
	PATCH_PREFIX_GIT
};

/*
 * A parsed path keeps the spelling status beside its bytes, since a decoded
 * path can itself begin with a quote. An undecodable quoted label keeps its
 * original spelling, while every real path can be quoted again for output.
 * Complete record names establish whether a/ or b/ belongs to the display
 * convention, so identity comparisons can leave literal directories intact.
 */
struct patch_name {
	char *text;
	size_t len;
	bool verbatim;
	enum patch_prefix prefix;
};

/*
 * File records retain input order. file owns the selected header name; pos is a
 * byte offset into that operand's buffer. pair links counterparts both ways, so
 * each record can belong to at most one pair. mode_only records get their name
 * from a Git opener rather than ---/+++ headers and cannot establish the strip
 * depth used for header names.
 */
struct file_list {
	struct patch_name *file;
	struct file_list *pair;
	long pos;
	bool mode_only;
	struct cds_list_head node;
};

/* read_atatline_n results other than success */
#define ATAT_NOT_HEADER 1 /* not an @@ hunk header at all */
#define ATAT_MALFORMED 2 /* opens as one, then breaks the grammar */

/*
 * Leave room for coordinate sums in signed long arithmetic. The smaller cap
 * also keeps shifts and sums representable when long has only 32 bits.
 */
#if ULONG_MAX > 0xffffffffUL
#define ATAT_MAX_COORD (1UL << 40)
#else
#define ATAT_MAX_COORD (1UL << 23)
#endif

/* The native matcher bounds its allocated row tables by this limit */
#define LINE_CEILING ((size_t)2 << 20)

/*
 * Bound each source image consistently, whether it comes from patch rows or a
 * Git blob, before the matcher allocates its work tables.
 */
#define PAYLOAD_CEILING ((size_t)64 << 20) /* bytes per image */

/*
 * Mode strings retain up to seven octal digits plus NUL; an empty field means
 * that stage was unspecified. Creation/deletion flags preserve the explicit
 * operation instead of inferring it from missing fields. Scans accumulate
 * fields and flags, so initialize this structure to zero before the first one.
 */
struct file_mode {
	char old_mode[8];
	char new_mode[8];
	bool created;
	bool deleted;
};

#endif /* PATCH_TYPES_H */
