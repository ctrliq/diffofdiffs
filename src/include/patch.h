/* SPDX-License-Identifier: GPL-2.0-only */
/* Original patch rows and the source views they describe */
#ifndef PATCH_H
#define PATCH_H

#include <reader.h>
#include <udiff.h>

/* Text is borrowed and retains its LF unless the patch marks source EOF */
struct patch_row {
	struct udiff_line text;
	const struct patch_move *move;

	/*
	 * Zero-based parent/result coordinates. On an absent stage, the
	 * coordinate names the insertion point before the next present line.
	 */
	size_t pos[2];

	/* SIZE_MAX for source rows outside a hunk, including unknown gaps */
	size_t hunk;

	/* Original patch sign, or '?' for an unquoted interval of any length */
	char sign;

	/*
	 * Part of an adjacent, byte-identical removal/addition with no effect.
	 */
	bool neutral;
};

struct patch_hunk {
	struct iomem_slice name;
	size_t first;
	size_t count;
	size_t pos[2];
	size_t lines[2];
};

/* A matched row interval in another file, linked back by partner_move */
struct patch_move {
	const struct patch_file *partner_file;
	size_t first;
	size_t count;
	size_t partner_move;
};

/*
 * Own the names and arrays; record and source slices borrow the input index and
 * patch buffer. Two-element stage arrays use parent at 0 and result at 1.
 */
struct patch_file {
	struct file_list *record;
	struct patch_name *name[2];
	struct patch_name *operation_name[2];
	struct block_story story;
	struct iomem_slice binary;
	struct patch_row *rows;
	struct patch_hunk *hunks;
	struct patch_move *moves;
	char *nlterm;
	size_t nrows;
	size_t nhunks;
	size_t nmoves;
	size_t row_cap;
	size_t hunk_cap;
	bool empty[2];
	bool copy;
};

struct patch_document {
	struct patch_file *files;
	size_t nfiles;
};

/*
 * A source has one ordered forward sequence. Views omit additions or deletions
 * without changing row identity. Unknown intervals have one explicit gap row;
 * their coordinates never cause allocation of invented source lines.
 */
struct source_view {
	struct udiff_line *lines;

	/* rows maps view indices to source rows; index maps back */
	size_t *rows;
	size_t *index;
	size_t count;
};

struct patch_source {
	const struct patch_file *file;
	struct patch_row *rows;
	struct source_view view[2];
	struct iomem_buf blobs[2];
	size_t nrows;
	size_t capacity;

	/* True when every unquoted interval contains verified Git bytes */
	bool complete;

	/*
	 * Overlapping hunks, source after EOF, or empty EOF-marked rows make
	 * quotations inconsistent. Matching then requires identical records.
	 */
	bool ambiguous;
};

void patch_document_read(struct patch_document *doc,
			 const struct iomem_buf *buf,
			 struct cds_list_head *index, const char *label);
void patch_document_free(struct patch_document *doc);

/* Assign move ownership before patch_source_read copies the document's rows */
void patch_match_moved(struct patch_document docs[2]);

/*
 * Complete reads own verified tree bytes; quoted reads borrow document text and
 * leave gaps even in tree mode. leg selects the left or right operand. Keep the
 * document and its input alive until the source has been freed.
 */
void patch_source_read(struct patch_source *source,
		       const struct patch_file *file,
		       const struct patch_file *other, int leg, bool complete);
void patch_source_free(struct patch_source *source);
bool patch_row_equal(const struct udiff_line *a, const struct udiff_line *b);
const struct patch_file *patch_row_partner(const struct patch_source *source,
					   size_t row);
const char *patch_file_path(const struct patch_file *file);
char *patch_related_change(const struct patch_file *file,
			   const struct patch_document *other, int depth);

DEFINE_FREE(patch_document, struct patch_document, patch_document_free(&_T))
DEFINE_FREE(patch_source, struct patch_source, patch_source_free(&_T))

#endif /* PATCH_H */
