// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ctrl IQ, Inc.
 *
 * Compare the two parents and the two results. Equal source is a common edit
 * only when both patches give it the same sign. Nothing is applied, inverted,
 * or relocated to manufacture a hypothetical result.
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include <cli.h>
#include <display.h>
#include <funcname.h>
#include <gittree.h>
#include <patch.h>
#include <review.h>
#include <util.h>

#define REPLACEMENT_MAX_BYTES 1024
#define REPLACEMENT_MAX_PAIRS 1024

/* Leg 0/1 selects the operand; stage 0/1 selects its parent/result */
struct comparison {
	/* A cross-file move limits ownership to its signed edit sequence */
	const struct patch_move *focus[2];
	struct patch_source source[2];

	/* Parent/result stages, each indexed by that stage's view positions */
	struct udiff_matches matches[2];

	/* NULL alignment maps fall back to exact matches for display */
	struct udiff_matches alignment[2];

	/* Per-leg source rows; an edit's mate has the same sign, or SIZE_MAX */
	size_t *edits[2];
	bool *selected[2];
	bool *shared[2];
};

static void comparison_free(struct comparison *review);
static void compare_sources(struct comparison *review);

DEFINE_FREE(comparison, struct comparison, comparison_free(&_T))

static size_t source_mate(const struct comparison *review,
			  const struct udiff_matches *matches, int leg,
			  int stage, size_t row)
{
	const struct source_view *view = &review->source[leg].view[stage];
	size_t at, mate;

	/* Matches use view indices; edit ownership uses original source rows */
	at = view->index[row];
	if (at == SIZE_MAX)
		return SIZE_MAX;

	mate = matches->side[leg][at];
	return mate == SIZE_MAX ? SIZE_MAX :
				  review->source[!leg].view[stage].rows[mate];
}

static size_t review_mate(const struct comparison *review, int leg, int stage,
			  size_t row)
{
	return source_mate(review, &review->matches[stage], leg, stage, row);
}

static const struct udiff_matches *
display_matches(const struct comparison *review, int stage)
{
	return review->alignment[stage].side[0] ? &review->alignment[stage] :
						  &review->matches[stage];
}

static size_t layout_mate(const struct comparison *review, int leg, int stage,
			  size_t row)
{
	return source_mate(review, display_matches(review, stage), leg, stage,
			   row);
}

struct line_occurrence {
	struct udiff_line text;
	size_t count[2];
};

static void init_unmatched_lines(const struct udiff_image *images,
				 struct udiff_matches *matches)
{
	for (int leg = 0; leg < 2; leg++) {
		matches->side[leg] = xmalloc_array(images[leg].nlines,
						   sizeof(*matches->side[leg]));
		for (size_t i = 0; i < images[leg].nlines; i++)
			matches->side[leg][i] = SIZE_MAX;
	}
}

/* A split-file edit can have its shared counterpart in another file pair */
static bool edit_elsewhere(const struct comparison *review, int leg, size_t row)
{
	const struct patch_file *partner =
		patch_row_partner(&review->source[leg], row);

	return partner && partner != review->source[!leg].file;
}

struct reindentation {
	struct udiff_matches matches;
	struct udiff_line *lines[2];
	size_t count[2];
	struct line_occurrence *keys;
	size_t nkeys;
};

static void match_changes(struct comparison *review)
{
	for (int leg = 0; leg < 2; leg++) {
		const struct patch_source *source = &review->source[leg];

		review->edits[leg] = xmalloc_array(source->nrows,
						   sizeof(*review->edits[leg]));
		for (size_t i = 0; i < source->nrows; i++) {
			const struct patch_row *row = &source->rows[i];
			size_t mate = SIZE_MAX;

			if ((row->sign == '-' || row->sign == '+') &&
			    !edit_elsewhere(review, leg, i)) {
				mate = review_mate(review, leg,
						   row->sign == '+', i);
				if (mate != SIZE_MAX &&
				    (review->source[!leg].rows[mate].sign !=
					     row->sign ||
				     edit_elsewhere(review, !leg, mate)))
					mate = SIZE_MAX;
			}
			review->edits[leg][i] = mate;
		}
	}
}

struct scope_link {
	size_t owner[2];
};

struct source_scopes {
	struct c_scope *regions;
	struct udiff_line *names;

	/* Image rows have owners; each owner's mate is in the other image */
	size_t *owner;
	size_t *mate;

	/* Left-side paired candidates become predecessors during ordering */
	size_t *guess;
	size_t count;
};

struct scope_pairing {
	struct source_scopes side[2];

	/* Body links can associate several functions after a split */
	struct scope_link *links;
	size_t nlinks;
};

/*
 * Matching ties must not depend on which operand the command lists first. Order
 * by known source bytes and original row roles; gap labels are local
 * placeholders and have no part in this ordering.
 */
static int source_order(const struct patch_source *a,
			const struct patch_source *b)
{
	for (size_t i = 0; i < MIN(a->nrows, b->nrows); i++) {
		const struct patch_row *right = &b->rows[i];
		const struct patch_row *left = &a->rows[i];
		int cmp;

		if (left->sign != right->sign)
			return left->sign < right->sign ? -1 : 1;

		if (left->sign != '?') {
			cmp = memcmp(left->text.ptr, right->text.ptr,
				     MIN(left->text.len, right->text.len));
			if (cmp)
				return cmp;

			if (left->text.len < right->text.len)
				return -1;

			if (left->text.len > right->text.len)
				return 1;
		}
		for (int stage = 0; stage < 2; stage++) {
			if (left->pos[stage] < right->pos[stage])
				return -1;

			if (left->pos[stage] > right->pos[stage])
				return 1;
		}
	}
	return (a->nrows > b->nrows) - (a->nrows < b->nrows);
}

/* Conflicting quotations need identical patch records */
static void match_ambiguous_sources(struct comparison *review,
				    const struct udiff_image images[2][2])
{
	bool identical = !source_order(&review->source[0], &review->source[1]);

	for (int stage = 0; stage < 2; stage++) {
		init_unmatched_lines(images[stage], &review->matches[stage]);
		if (!identical)
			continue;

		/*
		 * Equal gap placeholders still cannot identify unknown source.
		 */
		for (int leg = 0; leg < 2; leg++) {
			const struct patch_source *source =
				&review->source[leg];
			const struct source_view *view = &source->view[stage];

			for (size_t i = 0; i < view->count; i++) {
				if (source->rows[view->rows[i]].sign != '?')
					review->matches[stage].side[leg][i] = i;
			}
		}
	}
}

static void compare_ordered(struct comparison *review)
{
	struct udiff_image images[2][2];

	for (int stage = 0; stage < 2; stage++) {
		for (int leg = 0; leg < 2; leg++) {
			const struct source_view *view =
				&review->source[leg].view[stage];

			images[stage][leg] =
				(typeof(images[0][0])){ .lines = view->lines,
							.nlines = view->count };
		}
	}
	if (review->source[0].ambiguous || review->source[1].ambiguous) {
		match_ambiguous_sources(review, images);
		match_changes(review);
		return;
	}

	udiff_match(&images[0][0], &images[0][1], &review->matches[0]);
	udiff_match(&images[1][0], &images[1][1], &review->matches[1]);
	match_changes(review);
}

static void compare_sources(struct comparison *review)
{
	if (source_order(&review->source[0], &review->source[1]) <= 0) {
		compare_ordered(review);
	} else {
		struct comparison ordered = {
			.source = { review->source[1], review->source[0] },
			.focus = { review->focus[1], review->focus[0] }
		};

		compare_ordered(&ordered);

		/* Only the match tables change owners; sources are borrowed */
		for (int stage = 0; stage < 2; stage++) {
			for (int leg = 0; leg < 2; leg++) {
				review->matches[stage].side[leg] =
					ordered.matches[stage].side[!leg];
				review->alignment[stage].side[leg] =
					ordered.alignment[stage].side[!leg];
			}
		}
		review->edits[0] = ordered.edits[1];
		review->edits[1] = ordered.edits[0];
	}
}

static void select_delta(struct comparison *review)
{
	for (int leg = 0; leg < 2; leg++) {
		const struct patch_source *source = &review->source[leg];

		review->selected[leg] = xzalloc_array(
			source->nrows, sizeof(*review->selected[leg]));
		review->shared[leg] = xzalloc_array(
			source->nrows, sizeof(*review->shared[leg]));
	}

	/* Shared edits stay out of delta, including cross-file moves */
	for (int leg = 0; leg < 2; leg++) {
		const struct patch_source *source = &review->source[leg];

		for (size_t i = 0; i < source->nrows; i++) {
			const struct patch_row *row = &source->rows[i];

			if (row->sign != '-' && row->sign != '+')
				continue;

			if (patch_row_partner(source, i)) {
				review->shared[leg][i] = true;
				continue;
			}

			if (review->edits[leg][i] != SIZE_MAX) {
				review->shared[leg][i] = true;
				continue;
			}

			review->selected[leg][i] = true;
		}
	}

	/* Include retained counterparts before growing context margins */
}

struct row_pair {
	/*
	 * Context starts in result-view positions; pair_rows converts to rows.
	 */
	size_t side[2];
	bool selected[2];

	/* An empty difference seeds a context margin without selecting a row */
	bool gap_seed[2];
	bool equal;
	bool known;
};

struct paired_rows {
	struct row_pair *rows;
	size_t count;
};

static void paired_rows_free(struct paired_rows *pairs)
{
	free(pairs->rows);
}

DEFINE_FREE(paired_rows, struct paired_rows, paired_rows_free(&_T))

static void append_pair(struct paired_rows *pairs, size_t a, size_t b,
			bool equal)
{
	pairs->rows[pairs->count++] =
		(typeof(pairs->rows[0])){ .side = { a, b }, .equal = equal };
}

/*
 * Context alignment uses view indices and separates rejected equal-text pairs.
 * Delta alignment uses source rows (views == NULL), so deletions participate. A
 * positional pair there does not assert shared ownership: only edits[] does.
 */
static void align_until(struct paired_rows *pairs, size_t *at,
			const size_t *end, const struct source_view *views[2])
{
	while (at[0] < end[0] || at[1] < end[1]) {
		if (views && at[0] < end[0] && at[1] < end[1] &&
		    patch_row_equal(&views[0]->lines[at[0]],
				    &views[1]->lines[at[1]])) {
			/* Rejected equal-text matches must stay separate */
			append_pair(pairs, at[0]++, SIZE_MAX, false);
			append_pair(pairs, SIZE_MAX, at[1]++, false);
		} else {
			append_pair(pairs, at[0] < end[0] ? at[0]++ : SIZE_MAX,
				    at[1] < end[1] ? at[1]++ : SIZE_MAX, false);
		}
	}
}

struct replacement_match {
	size_t mate;
	unsigned int score;
};

/* Parent rows can pair deletions inside each interval between result rows */
static void align_parents(const struct comparison *review,
			  struct paired_rows *pairs, size_t *at,
			  const size_t *end)
{
	for (size_t i = at[0]; i < end[0]; i++) {
		size_t next[2], mate;

		mate = layout_mate(review, 0, 0, i);
		if (mate == SIZE_MAX || mate < at[1] || mate >= end[1])
			continue;

		next[0] = i;
		next[1] = mate;
		align_until(pairs, at, next, NULL);
		append_pair(pairs, at[0]++, at[1]++,
			    review_mate(review, 0, 0, i) == mate);
	}
	align_until(pairs, at, end, NULL);
}

/*
 * Both sections use one ordered pair sequence. Result correspondence supplies
 * the main sequence; delta also keeps original deletions in their own gaps.
 */
static void pair_rows(const struct comparison *review,
		      struct paired_rows *pairs, bool context)
{
	const struct udiff_matches *matches = display_matches(review, 1);
	const struct source_view *views[2] = { &review->source[0].view[1],
					       &review->source[1].view[1] };
	size_t at[2] = {}, end[2];

	/* Every appended pair consumes at least one original source row */
	pairs->rows =
		xmalloc_array(review->source[0].nrows + review->source[1].nrows,
			      sizeof(*pairs->rows));

	/* Monotonic result anchors bound gaps, including delta's deletions */
	for (size_t i = 0; i < views[0]->count; i++) {
		size_t mate;

		mate = matches->side[0][i];
		if (mate == SIZE_MAX)
			continue;

		end[0] = context ? i : views[0]->rows[i];
		end[1] = context ? mate : views[1]->rows[mate];
		if (context)
			align_until(pairs, at, end, views);
		else
			align_parents(review, pairs, at, end);
		append_pair(pairs, at[0]++, at[1]++,
			    review->matches[1].side[0][i] == mate);
	}
	for (int leg = 0; leg < 2; leg++) {
		end[leg] = context ? views[leg]->count :
				     review->source[leg].nrows;
	}
	if (context)
		align_until(pairs, at, end, views);
	else
		align_parents(review, pairs, at, end);
	if (context) {
		/* Saved report rows always use original source indices */
		for (size_t i = 0; i < pairs->count; i++) {
			for (int leg = 0; leg < 2; leg++) {
				size_t *at = &pairs->rows[i].side[leg];

				if (*at != SIZE_MAX)
					*at = views[leg]->rows[*at];
			}
		}
	}
}

static void select_pairs(const struct comparison *review,
			 struct paired_rows *pairs)
{
	for (size_t i = 0; i < pairs->count; i++) {
		struct row_pair *pair = &pairs->rows[i];

		for (int leg = 0; leg < 2; leg++) {
			size_t at = pair->side[leg];

			pair->selected[leg] = at != SIZE_MAX &&
					      review->selected[leg][at];
		}
		for (int leg = 0; pair->equal && leg < 2; leg++) {
			if (pair->selected[!leg] &&
			    review->source[leg].rows[pair->side[leg]].hunk !=
				    SIZE_MAX)
				pair->selected[leg] = true;
		}
	}
}

static void save_review_row(const struct comparison *review,
			    const struct row_pair *pair, int leg, bool context,
			    struct review_row *saved)
{
	const struct patch_row *row =
		&review->source[leg].rows[pair->side[leg]];

	/* Saved text must outlive views and backing blobs */
	*saved = (typeof(*saved)){
		.text = memdup(row->text.ptr, row->text.len),
		.len = row->text.len,
		.pos = { row->sign != '+' ? row->pos[0] : SIZE_MAX,
			 row->sign != '-' ? row->pos[1] : SIZE_MAX },
		.sign = row->sign,
		.shared = !context && review->shared[leg][pair->side[leg]]
	};

	/*
	 * Delta keeps native signs and both coordinate systems. Context reports
	 * presence in the results, so its signs identify which result owns a
	 * row.
	 */
	if (context) {
		saved->pos[0] = SIZE_MAX;
		saved->sign = ' ';
		if (!pair->equal)
			saved->sign = leg ? '+' : '-';
	}
}

static void save_section(const struct comparison *review,
			 const struct paired_rows *pairs,
			 struct review_section *section, bool context)
{
	size_t previous[2] = { SIZE_MAX, SIZE_MAX }, block_capacity = 0;
	bool separated = true;
	size_t count = 0;

	for (size_t i = 0; i < pairs->count; i++) {
		count += pairs->rows[i].selected[0] ||
			 pairs->rows[i].selected[1];
	}
	section->rows = xzalloc_array(count, sizeof(*section->rows));
	for (size_t i = 0; i < pairs->count; i++) {
		const struct row_pair *pair = &pairs->rows[i];
		size_t indexes[2] = { SIZE_MAX, SIZE_MAX };
		struct review_block *block;
		struct review_pair *saved;

		if (!pair->selected[0] && !pair->selected[1])
			continue;

		/*
		 * A jump on either side starts a new excerpt. Delta follows
		 * interleaved source rows; context follows result rows, where
		 * omitted deletions do not create a gap in the excerpt.
		 */
		for (int leg = 0; leg < 2; leg++) {
			size_t at = pair->side[leg];

			if (!pair->selected[leg])
				continue;

			indexes[leg] =
				context ?
					review->source[leg].view[1].index[at] :
					at;
			if (previous[leg] != SIZE_MAX &&
			    indexes[leg] != previous[leg] + 1)
				separated = true;
		}
		if (separated) {
			if (section->nblocks == block_capacity) {
				block_capacity =
					block_capacity ? block_capacity * 2 : 8;
				section->blocks = xrealloc_array(
					section->blocks, block_capacity,
					sizeof(*section->blocks));
			}
			section->blocks[section->nblocks++] =
				(typeof(section->blocks[0])){
					.first = section->nrows
				};
			previous[0] = previous[1] = SIZE_MAX;
			separated = false;
		}
		saved = &section->rows[section->nrows++];
		block = &section->blocks[section->nblocks - 1];
		block->count++;
		for (int leg = 0; leg < 2; leg++) {
			if (!pair->selected[leg])
				continue;

			previous[leg] = indexes[leg];
			save_review_row(review, pair, leg, context,
					&saved->side[leg]);
		}
	}
}

static void comparison_free(struct comparison *review)
{
	for (int stage = 0; stage < 2; stage++) {
		udiff_matches_free(&review->matches[stage]);
		udiff_matches_free(&review->alignment[stage]);
	}

	for (int leg = 0; leg < 2; leg++) {
		patch_source_free(&review->source[leg]);
		free(review->edits[leg]);
		free(review->selected[leg]);
		free(review->shared[leg]);
	}
}

static char *file_label(const struct patch_source *source,
			const struct patch_source *other)
{
	const struct patch_file *file = source->file ?: other->file;
	const struct patch_name *name = file->record->file;
	struct patch_name *stripped_name __free(patch_name) =
		patch_name_strip(name, name->prefix == PATCH_PREFIX_GIT);

	return git_quote_name(stripped_name);
}

static void review_file_build(struct review_file *file,
			      const struct patch_file *a,
			      const struct patch_file *b)
{
	struct comparison review __free(comparison) = {};

	patch_source_read(&review.source[0], a, b, 0, gittree_active());
	patch_source_read(&review.source[1], b, a, 1, gittree_active());
	compare_sources(&review);

	select_delta(&review);
	for (int section = 0; section < 1; section++) {
		struct paired_rows pairs __free(paired_rows) = {};

		pair_rows(&review, &pairs, section);
		select_pairs(&review, &pairs);
		save_section(&review, &pairs, &file->section[section], section);
	}
	for (int leg = 0; leg < 2; leg++) {
		const struct patch_source *source = &review.source[leg];

		file->path[leg] = file_label(source, &review.source[!leg]);
		file->ambiguous |= source->ambiguous;
	}
}

static void review_section_free(struct review_section *section)
{
	for (size_t i = 0; i < section->nrows; i++) {
		for (int leg = 0; leg < 2; leg++)
			free(section->rows[i].side[leg].text);
	}
	for (size_t i = 0; i < section->nblocks; i++) {
		for (int leg = 0; leg < 2; leg++)
			free(section->blocks[i].name[leg]);
	}
	free(section->rows);
	free(section->blocks);
}

static void review_file_free(struct review_file *file)
{
	for (int s = 0; s < 2; s++)
		review_section_free(&file->section[s]);
	for (int leg = 0; leg < 2; leg++) {
		free(file->path[leg]);
		free(file->metadata[leg]);
		free(file->note[leg]);
	}
}

void review_report_free(struct review_report *report)
{
	for (size_t f = 0; f < report->count; f++)
		review_file_free(&report->files[f]);
	free(report->files);
	*report = (typeof(*report)){};
}

void review_report_build(struct review_report *report,
			 const struct iomem_buf *a, const struct iomem_buf *b)
{
	struct patch_document docs[2] = {};
	CDS_LIST_HEAD(files_in_patch1);
	CDS_LIST_HEAD(files_in_patch2);
	int ignore_components;
	size_t capacity;

	*report = (typeof(*report)){};

	/* Pair complete file records before interpreting any quoted source */
	index_patch(a, &files_in_patch1);
	index_patch(b, &files_in_patch2);
	resolve_name_prefixes(&files_in_patch1, &files_in_patch2);
	ignore_components =
		determine_ignore_components(&files_in_patch1, &files_in_patch2);
	pair_file_lists(&files_in_patch1, &files_in_patch2, ignore_components);
	patch_document_read(&docs[0], a, &files_in_patch1, "patch #1");
	patch_document_read(&docs[1], b, &files_in_patch2, "patch #2");

	capacity = docs[0].nfiles + docs[1].nfiles;
	report->files = xzalloc_array(capacity, sizeof(*report->files));

	/* Emit pairs from the left and remaining files from the right */
	for (int leg = 0; leg < 2; leg++) {
		for (size_t i = 0; i < docs[leg].nfiles; i++) {
			const struct patch_file *file = &docs[leg].files[i],
						*pair = NULL;
			const struct file_list *paired = file->record->pair;
			struct review_file *saved;

			if (leg && paired)
				continue;

			if (paired) {
				for (size_t j = 0; j < docs[1].nfiles; j++) {
					if (docs[1].files[j].record == paired)
						pair = &docs[1].files[j];
				}
			}
			saved = &report->files[report->count++];
			review_file_build(saved, leg ? NULL : file,
					  leg ? file : pair);
			if (!pair) {
				char *note = patch_related_change(
					file, &docs[!leg], ignore_components);

				if (note) {
					free(saved->note[leg]);
					saved->note[leg] = note;
				}
			}
		}
	}

	patch_document_free(&docs[0]);
	patch_document_free(&docs[1]);
	file_list_free(&files_in_patch1);
	file_list_free(&files_in_patch2);
}
