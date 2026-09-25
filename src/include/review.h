/* SPDX-License-Identifier: GPL-2.0-only */
/* Selected original source, ready for layout without further matching */
#ifndef REVIEW_H
#define REVIEW_H

#include <iomem.h>
#include <types.h>

struct review_row {
	/* NULL leaves this side of the displayed row empty */
	char *text;
	size_t len;

	/*
	 * Zero-based parent/result positions; SIZE_MAX suppresses the number.
	 * Context rows carry only the result position.
	 */
	size_t pos[2];

	/*
	 * Native edit sign in delta; left '-' or right '+' for unequal context.
	 */
	char sign;

	/* Shared delta edits retain their native sign and source coordinates */
	bool shared;
};

struct review_pair {
	struct review_row side[2];
};

struct review_block {
	/* Optional function labels for this interval of section rows */
	char *name[2];
	size_t first;
	size_t count;
};

struct review_section {
	struct review_pair *rows;
	struct review_block *blocks;
	size_t nrows;
	size_t nblocks;
};

struct review_file {
	/* Index 0 is the delta section; index 1 is source context */
	struct review_section section[2];
	char *path[2];
	char *metadata[2];
	char *note[2];
	bool metadata_diff;
	bool ambiguous;
};

struct review_report {
	struct review_file *files;
	size_t count;
};

/* Build an owned report; its copied rows outlive the borrowed input buffers */
void review_report_build(struct review_report *report,
			 const struct iomem_buf *a, const struct iomem_buf *b);
void review_report_free(struct review_report *report);

DEFINE_FREE(review_report, struct review_report, review_report_free(&_T))

#endif /* REVIEW_H */
