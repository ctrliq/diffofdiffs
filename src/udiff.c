// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ctrl IQ, Inc.
 *
 * Exact line comparison and patch derivation share libgit2's buffer differ.
 * Original rows supply the emitted bytes and coordinates, including the final
 * newline distinction that a source match must preserve.
 */
#include <git2.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include <iomem.h>
#include <udiff.h>
#include <util.h>

#define MINIMAL_MAX_LINES 65536

struct comparison {
	const struct udiff_image *image[2];
	struct udiff_matches *matches;
	struct udiff_result *result;
	struct iomem_writer writer;
	udiff_name_fn name_of;
	void *name_ctx;
	size_t seen[2];
	bool unique;
};

static size_t image_size(const struct udiff_image *image, const char *side)
{
	size_t size = 0;

	if (image->nlines > LINE_CEILING)
		die("the %s operand has %zu lines, over the diff engine's %zu-line per-image ceiling",
		    side, image->nlines, LINE_CEILING);

	for (size_t i = 0; i < image->nlines; i++) {
		if (image->lines[i].len > PAYLOAD_CEILING - size)
			die("the %s operand exceeds the diff engine's %zu MiB per-image payload ceiling",
			    side, PAYLOAD_CEILING >> 20);

		size += image->lines[i].len;
	}
	return size;
}

static bool line_equal(const struct udiff_line *a, const struct udiff_line *b)
{
	return a->len == b->len && !memcmp(a->ptr, b->ptr, a->len);
}

static bool images_equal(const struct udiff_image *a,
			 const struct udiff_image *b)
{
	if (a->nlines != b->nlines)
		return false;

	for (size_t i = 0; i < a->nlines; i++) {
		if (!line_equal(&a->lines[i], &b->lines[i]))
			return false;
	}
	return true;
}

/* Views can skip original patch rows, so their bytes need not be adjacent */
static char *image_bytes(const struct udiff_image *image, size_t size)
{
	char *bytes = xmalloc(size + 1);
	size_t at = 0;

	for (size_t i = 0; i < image->nlines; i++) {
		const struct udiff_line *line = &image->lines[i];

		memcpy(bytes + at, line->ptr, line->len);
		at += line->len;
	}
	bytes[at] = '\0';
	return bytes;
}

static void emit_range(FILE *out, int start, int count)
{
	fprintf(out, "%d", start);
	if (count != 1)
		fprintf(out, ",%d", count);
}

static int emit_hunk(const git_diff_delta *delta __unused,
		     const git_diff_hunk *hunk, void *data)
{
	struct comparison *comparison = data;
	FILE *out = comparison->writer.fp;

	fputs("@@ -", out);
	emit_range(out, hunk->old_start, hunk->old_lines);
	fputs(" +", out);
	emit_range(out, hunk->new_start, hunk->new_lines);
	fputs(" @@", out);
	if (comparison->name_of) {
		/*
		 * Empty ranges name an insertion gap, not a one-based source
		 * row.
		 */
		size_t first = hunk->old_start - !!hunk->old_lines;
		size_t length = 0;
		const char *name;

		name = comparison->name_of(comparison->name_ctx, first,
					   &length);
		if (name) {
			if (!length || length > PAYLOAD_CEILING ||
			    memchr(name, '\n', length))
				die("the name provider returned an empty, oversized, or newline-bearing tail");

			fputc(' ', out);
			fwrite(name, 1, length, out);
		}
	}
	fputc('\n', out);
	comparison->result->hunks++;
	return 0;
}

static void emit_line(FILE *out, char sign, const struct udiff_line *line)
{
	fputc(sign, out);
	fwrite(line->ptr, 1, line->len, out);
	if (line->ptr[line->len - 1] != '\n')
		fputs("\n\\ No newline at end of file\n", out);
}

static int accept_line(const git_diff_delta *delta __unused,
		       const git_diff_hunk *hunk __unused,
		       const git_diff_line *line, void *data)
{
	const struct udiff_line *old = NULL, *new = NULL;
	struct comparison *comparison = data;
	FILE *out = comparison->writer.fp;

	if (line->origin != GIT_DIFF_LINE_CONTEXT &&
	    line->origin != GIT_DIFF_LINE_DELETION &&
	    line->origin != GIT_DIFF_LINE_ADDITION)
		return 0;

	if (line->old_lineno > 0)
		old = &comparison->image[0]->lines[line->old_lineno - 1];
	if (line->new_lineno > 0)
		new = &comparison->image[1]->lines[line->new_lineno - 1];
	if (comparison->matches) {
		comparison->seen[0] += old != NULL;
		comparison->seen[1] += new != NULL;

		/* Library matches must preserve our exact row bytes */
		if (old && new && line_equal(old, new)) {
			comparison->matches->side[0][line->old_lineno - 1] =
				line->new_lineno - 1;
			comparison->matches->side[1][line->new_lineno - 1] =
				line->old_lineno - 1;
		}
		return 0;
	}

	/* Original rows own their final newline, including its absence */
	if (old && new && line_equal(old, new)) {
		emit_line(out, ' ', old);
	} else {
		if (old)
			emit_line(out, '-', old);
		if (new)
			emit_line(out, '+', new);
	}
	return 0;
}

static void compare_images(struct comparison *comparison, unsigned int context)
{
	const struct udiff_image *a = comparison->image[0];
	const struct udiff_image *b = comparison->image[1];
	char *right __free(free) = NULL;
	char *left __free(free) = NULL;
	git_diff_options options;
	size_t sizes[2];
	int status;

	sizes[0] = image_size(a, "old");
	sizes[1] = image_size(b, "new");
	if (comparison->matches) {
		for (int leg = 0; leg < 2; leg++) {
			size_t count = comparison->image[leg]->nlines;
			size_t *map = xmalloc_array(count, sizeof(*map));

			comparison->matches->side[leg] = map;

			/* Missing callbacks must not invent matches */
			for (size_t i = 0; i < count; i++)
				map[i] = SIZE_MAX;
		}
	}
	if (images_equal(a, b)) {
		if (comparison->matches) {
			for (size_t i = 0; i < a->nlines; i++) {
				comparison->matches->side[0][i] =
					comparison->matches->side[1][i] = i;
			}
		} else {
			comparison->result->equal = true;
		}
		return;
	}

	if (comparison->matches && (!a->nlines || !b->nlines))
		return;

	if (!a->bytes)
		left = image_bytes(a, sizes[0]);
	if (!b->bytes)
		right = image_bytes(b, sizes[1]);
	status = git_libgit2_init();
	if (status < 0)
		die("cannot initialize line comparison");

	git_diff_options_init(&options, GIT_DIFF_OPTIONS_VERSION);
	options.flags = GIT_DIFF_FORCE_TEXT | GIT_DIFF_INDENT_HEURISTIC;

	/*
	 * Patience favors unique anchors; use minimal matching only while its
	 * cost remains bounded.
	 */
	if (comparison->unique)
		options.flags |= GIT_DIFF_PATIENCE;
	else if (a->nlines + b->nlines <= MINIMAL_MAX_LINES)
		options.flags |= GIT_DIFF_MINIMAL;
	options.context_lines = MIN((size_t)context, LINE_CEILING);
	if (comparison->result)
		iomem_writer_open(&comparison->writer);
	status = git_diff_buffers(a->bytes ? a->bytes : left, sizes[0], NULL,
				  b->bytes ? b->bytes : right, sizes[1], NULL,
				  &options, NULL, NULL,
				  comparison->result ? emit_hunk : NULL,
				  accept_line, comparison);

	/* An allocation failure can return zero without callbacks */
	if (status || (comparison->result && !comparison->result->hunks) ||
	    (comparison->matches && (comparison->seen[0] != a->nlines ||
				     comparison->seen[1] != b->nlines))) {
		const git_error *error = git_error_last();

		pr_dbg("line comparison: %s\n",
		       error ? error->message : "(no library detail)");
		die("cannot compare the source images");
	}
	git_libgit2_shutdown();
	if (comparison->result) {
		struct iomem_buf bytes = {};

		iomem_writer_publish(&comparison->writer, &bytes);
		comparison->result->out_buf = bytes.base;
		comparison->result->out_len = bytes.len;
	}
}

void udiff_run(const struct udiff_image *a, const struct udiff_image *b,
	       unsigned int context, udiff_name_fn name_of, void *name_ctx,
	       struct udiff_result *out)
{
	struct comparison comparison = { .image = { a, b },
					 .result = out,
					 .name_of = name_of,
					 .name_ctx = name_ctx };

	*out = (typeof(*out)){};
	compare_images(&comparison, context);
}

void udiff_match(const struct udiff_image *old, const struct udiff_image *new,
		 struct udiff_matches *out)
{
	struct comparison comparison = { .image = { old, new },
					 .matches = out };

	/* Full context makes the callback visit unchanged rows as well */
	compare_images(&comparison, MAX(old->nlines, new->nlines));
}

void udiff_match_unique(const struct udiff_image *old,
			const struct udiff_image *new,
			struct udiff_matches *out)
{
	struct comparison comparison = { .image = { old, new },
					 .matches = out,
					 .unique = true };

	compare_images(&comparison, MAX(old->nlines, new->nlines));
}

void udiff_matches_free(struct udiff_matches *matches)
{
	free(matches->side[0]);
	free(matches->side[1]);
}

void udiff_result_free(struct udiff_result *result)
{
	free(result->out_buf);
}
