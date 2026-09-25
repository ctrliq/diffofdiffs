/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 Ctrl IQ, Inc.
 *
 * Unified-diff validation, file indexing, and filename correspondence.
 */
#ifndef READER_H
#define READER_H

#include <stddef.h>

#include <iomem.h>
#include <patch-types.h>
#include <util.h>

/*
 * DOC: two passes over one grammar
 *
 * validate_patch() checks hunk counts and normalizes CRLF sections in place.
 * index_patch() then builds the file list using the same line parsers. Run
 * validation first: compaction changes offsets in the input buffer.
 *
 * Raw patch lines are bounded slices and may contain embedded NUL bytes. Parsed
 * names have owned, terminated storage; their recorded length still preserves
 * any embedded NUL for validation.
 */

/* How the grammar reads one line of a hunk body */
enum body_class {
	BODY_JUNK, /* breaks the grammar where a body belongs */
	BODY_CONTEXT, /* spends one line from each side */
	BODY_REMOVED, /* spends one old-side line */
	BODY_ADDED, /* spends one new-side line */
	BODY_NO_NEWLINE, /* the marker behind an unterminated line */
};

/*
 * Normalize and validate one operand. Empty input and recognized file
 * operations without text hunks warn; malformed input terminates. name is the
 * command-line operand, and which labels hunk diagnostics as patch1 or patch2.
 */
void validate_patch(struct iomem_buf *buf, const char *name, const char *which);

/* Appends one record per file the operand names to the list */
void index_patch(const struct iomem_buf *buf, struct cds_list_head *list);

/* Resolve incomplete bare labels against the other operand's concrete paths */
void resolve_name_prefixes(struct cds_list_head *list1,
			   struct cds_list_head *list2);

struct file_list *add_to_list(struct cds_list_head *list,
			      const struct patch_name *file, long pos);
void pair_file_lists(struct cds_list_head *list1, struct cds_list_head *list2,
		     int depth);

void file_list_free(struct cds_list_head *list);

/* Return one of the input names, preferring a non-/dev/null path */
const struct patch_name *best_patch_name(const struct patch_name *oldname,
					 const struct patch_name *newname);

struct patch_name *patch_name_dup(const struct patch_name *name);
struct patch_name *patch_name_strip(const struct patch_name *name, int depth);
void patch_name_free(struct patch_name *name);

DEFINE_FREE(patch_name, struct patch_name *, patch_name_free(_T))

/* Return a borrowed suffix, stopping at the basename if stripping too far */
const char *stripped(const char *name, int num_components);

/*
 * Parse bytes after a ---/+++ marker, excluding any timestamp. Return an owned
 * decoded name, preserving its byte length even when it contains NULs.
 */
struct patch_name *filename_from_header(const char *base, size_t len);

/*
 * Return an owned name with valid Git quoting decoded and other spellings
 * copied unchanged.
 */
struct patch_name *unquoted_name(const char *base, size_t len);

/* Return an owned Git-quoted spelling, or NULL for a NULL name */
char *git_quote_name(const struct patch_name *name);

/*
 * Return an owned new-side name from a Git opener and its metadata, or NULL if
 * the two paths cannot be separated unambiguously. Retain the written prefix
 * and record whether matching should treat it as a Git display prefix.
 */
struct patch_name *git_block_name(const struct iomem_buf *buf,
				  const struct iomem_line *line);

bool git_block_names(const struct iomem_line *line,
		     const struct patch_name *oldname,
		     const struct patch_name *newname);

/* The opener above a record's metadata or header, or -1 for a bare section */
long block_opener_above(const struct iomem_buf *buf, size_t at);

/* The prefix convention established by a record's complete names */
enum patch_prefix patch_name_prefix(const struct iomem_buf *buf, long pos,
				    const char *operand,
				    const struct patch_name *key);

/* The length of a header's bytes with any line ending chopped off */
size_t chomp_header(const char *base, size_t len);

/*
 * Return the length of the function label after the closing @@ and borrow its
 * bytes through tail. A missing closing delimiter or empty label returns zero.
 */
size_t atat_tail(const char *base, size_t len, const char **tail);

/*
 * At EOF, tolerate at most three missing context rows, equally short on both
 * stages. Unequal or larger shortfalls are fatal; no source bytes are invented.
 */
void check_hunk_chop(unsigned long old_left, unsigned long new_left);

/*
 * Parses an @@ line of at most len bytes, storing the numbers through whichever
 * pointers aren't NULL. Returns 0, ATAT_NOT_HEADER for a line that isn't a hunk
 * header at all, or ATAT_MALFORMED for one that opens as a header and then
 * breaks the grammar.
 */
int read_atatline_n(const char *base, size_t len, unsigned long *orig_offset,
		    unsigned long *orig_count, unsigned long *new_offset,
		    unsigned long *new_count);

/*
 * Classify a hunk body line. Optional content receives the source-text offset:
 * one after an explicit sign, or zero for context missing its leading space.
 */
enum body_class classify_body_line(const char *base, size_t len,
				   size_t *content);

/* Refuses a body line that breaks the grammar, naming the line */
void malformed_patch_line(const char *base, size_t len);

/* The smallest strip depth at which any patch1 name matches a patch2 name */
int determine_ignore_components(struct cds_list_head *list1,
				struct cds_list_head *list2);

/*
 * Reads a mode line into *m, which it touches only on a match. The four forms
 * are "old mode", "new mode", "new file mode", and "deleted file mode", each
 * carrying an octal token of at most seven digits; a longer or non-octal token
 * makes the line something other than a mode line.
 */
bool scan_mode_line(const char *base, size_t len, struct file_mode *m);

/*
 * Recognize the mode keyword and separator without validating its value.
 * Metadata boundaries use this weaker test even when the octal field is bad.
 */
bool line_is_mode_word(const char *base, size_t len);

/*
 * Recognize "GIT binary patch" or "Binary files " without reading a payload.
 */
bool line_is_binary_word(const char *base, size_t len);

/*
 * Consecutive from/to lines must name the same operation to form a pair.
 */
enum block_op {
	BLOCK_OP_NONE,
	BLOCK_OP_RENAME_FROM,
	BLOCK_OP_RENAME_TO,
	BLOCK_OP_COPY_FROM,
	BLOCK_OP_COPY_TO
};

/*
 * Validation and indexing share the decision to retain a metadata-only block.
 * named requires a decoded opener path; armed records an operation indication.
 * complete requires a mode pair, adjacent rename/copy pair, valid similarity,
 * or valid index line in the uninterrupted metadata after the opener. Both
 * armed and complete must be set to retain a file record.
 */
struct block_latch {
	bool named;
	bool armed;
	bool complete;
	bool run_open;
	bool saw_old_mode;
	bool saw_new_mode;
	enum block_op prev_op;
};

/* Resets the latch at an opener; named says the opener names its new side */
void block_latch_opener(struct block_latch *bl, bool named);

/*
 * Process metadata before ---/+++ headers. Return true at the first operation
 * indication so the index can save its offset as the record's start.
 */
bool block_latch_line(struct block_latch *bl, const char *base, size_t len);

/*
 * Parse two 4-64 digit lowercase object IDs around "..", an optional six-digit
 * octal mode, and an optional CR. Set borrowed pre/post slices only on success.
 */
bool scan_index_line(const char *base, size_t len, struct iomem_slice *pre,
		     struct iomem_slice *post);

/*
 * Metadata from the uninterrupted run after a Git opener. String slices borrow
 * the patch buffer; later valid fields replace earlier ones. has_binary records
 * a binary marker, not proof that a payload follows it. Similarity lines keep
 * the run open without storing a field.
 */
struct block_story {
	struct file_mode mode;
	struct iomem_slice rename_from;
	struct iomem_slice rename_to;
	struct iomem_slice copy_from;
	struct iomem_slice copy_to;
	struct iomem_slice index_pre;
	struct iomem_slice index_post;
	bool has_index;
	bool has_binary;
};

/*
 * Reset bs and read metadata immediately after the opener offset, stopping at
 * the first unrecognized line. operand labels seek errors.
 */
void block_story_read(struct block_story *bs, const struct iomem_buf *buf,
		      size_t opener, const char *operand);

#endif /* READER_H */
