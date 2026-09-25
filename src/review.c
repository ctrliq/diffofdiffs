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

DEFINE_FREE(udiff_matches, struct udiff_matches, udiff_matches_free(&_T))

static void match_interval(const struct udiff_image *images,
			   struct udiff_matches *matches, const size_t *first,
			   const size_t *end)
{
	struct udiff_matches part __free(udiff_matches) = {};
	struct udiff_image window[2];

	if (first[0] == end[0] || first[1] == end[1])
		return;

	for (int leg = 0; leg < 2; leg++) {
		window[leg] =
			(typeof(window[0])){ .lines = images[leg].lines +
						      first[leg],
					     .nlines = end[leg] - first[leg] };
	}
	udiff_match(&window[0], &window[1], &part);
	for (int leg = 0; leg < 2; leg++) {
		for (size_t i = 0; i < window[leg].nlines; i++) {
			if (part.side[leg][i] != SIZE_MAX)
				matches->side[leg][first[leg] + i] =
					first[!leg] + part.side[leg][i];
		}
	}
}

struct line_occurrence {
	struct udiff_line text;
	size_t count[2];
};

static int line_occurrence_order(const void *a, const void *b)
{
	const struct line_occurrence *left = a, *right = b;

	if (left->text.len != right->text.len)
		return left->text.len < right->text.len ? -1 : 1;

	return memcmp(left->text.ptr, right->text.ptr, left->text.len);
}

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

static struct line_occurrence *
collect_line_occurrences(const struct udiff_image *images,
			 const struct udiff_image *candidates,
			 const size_t *map, size_t *count)
{
	struct line_occurrence *keys =
		xmalloc_array(candidates->nlines, sizeof(*keys));
	size_t n = 0;

	*count = 0;
	for (size_t i = 0; i < candidates->nlines; i++) {
		if (!map || map[i] != SIZE_MAX)
			keys[n++] = (typeof(keys[0])){
				.text = candidates->lines[i]
			};
	}
	qsort(keys, n, sizeof(*keys), line_occurrence_order);

	/* Repeated candidate lines must share one occurrence counter */
	for (size_t i = 0; i < n; i++) {
		if (!*count ||
		    line_occurrence_order(&keys[*count - 1], &keys[i]))
			keys[(*count)++] = keys[i];
	}
	for (int leg = 0; leg < 2; leg++) {
		for (size_t i = 0; i < images[leg].nlines; i++) {
			struct line_occurrence key = {
				.text = images[leg].lines[i]
			};
			struct line_occurrence *found;

			found = bsearch(&key, keys, *count, sizeof(*keys),
					line_occurrence_order);

			/* Two occurrences suffice to disprove uniqueness */
			if (found && found->count[leg] < 2)
				found->count[leg]++;
		}
	}
	return keys;
}

static bool line_has_content(const struct udiff_line *line)
{
	for (size_t i = 0; i < line->len; i++) {
		if (!isspace(line->ptr[i]))
			return true;
	}
	return false;
}

static bool line_is_unique_in_both(const struct line_occurrence *keys,
				   size_t count, const struct udiff_line *line)
{
	struct line_occurrence key = { .text = *line };
	const struct line_occurrence *found;

	if (!count || !line_has_content(line))
		return false;

	found = bsearch(&key, keys, count, sizeof(*keys),
			line_occurrence_order);
	return found && found->count[0] == 1 && found->count[1] == 1;
}

static bool line_has_word(const struct udiff_line *line)
{
	for (size_t i = 0; i < line->len; i++) {
		if (isalnum(line->ptr[i]) || line->ptr[i] == '_')
			return true;
	}
	return false;
}

static bool same_blank_context(const struct udiff_image *images,
			       const struct udiff_matches *matches,
			       const size_t *first, const size_t *end,
			       const size_t *at)
{
	for (int direction = -1; direction <= 1; direction += 2) {
		size_t neighbor[2];

		for (int leg = 0; leg < 2; leg++) {
			/* Punctuation cannot identify a neighboring site */
			for (neighbor[leg] = at[leg] + direction;;
			     neighbor[leg] += direction) {
				if (neighbor[leg] < first[leg] ||
				    neighbor[leg] >= end[leg]) {
					neighbor[leg] = SIZE_MAX;
					break;
				}
				if (line_has_word(
					    &images[leg].lines[neighbor[leg]]))
					break;
			}
		}
		if (neighbor[0] == SIZE_MAX || neighbor[1] == SIZE_MAX) {
			if (neighbor[0] == neighbor[1])
				return true;
		} else if (matches->side[0][neighbor[0]] == neighbor[1]) {
			return true;
		}
	}
	return false;
}

/*
 * A line repeated elsewhere in the file can still identify a site inside one
 * function. Recover missing unique lines before matching the remaining source.
 * Keep the original match when no such anchor was lost.
 */
static void match_function_interval(const struct udiff_image *images,
				    struct udiff_matches *matches,
				    const size_t *first, const size_t *end)
{
	struct udiff_matches candidates __free(udiff_matches) = {};
	size_t cursor[2] = { first[0], first[1] }, count = 0;
	struct line_occurrence *keys __free(free) = NULL;
	struct udiff_image window[2];
	bool missing = false;

	match_interval(images, matches, first, end);
	if (first[0] == end[0] || first[1] == end[1])
		return;

	for (int leg = 0; leg < 2; leg++) {
		window[leg] =
			(typeof(window[0])){ .lines = images[leg].lines +
						      first[leg],
					     .nlines = end[leg] - first[leg] };
	}
	udiff_match_unique(&window[0], &window[1], &candidates);
	keys = collect_line_occurrences(window, &window[0], candidates.side[0],
					&count);
	for (size_t i = 0; i < window[0].nlines; i++) {
		size_t mate;

		mate = candidates.side[0][i];
		if (mate == SIZE_MAX || !line_has_word(&window[0].lines[i]) ||
		    !line_is_unique_in_both(keys, count, &window[0].lines[i])) {
			candidates.side[0][i] = SIZE_MAX;
			continue;
		}
		missing |= matches->side[0][first[0] + i] != first[1] + mate;
	}
	if (!missing)
		return;

	/* Rebuild both directions around the recovered unique anchors */
	for (int leg = 0; leg < 2; leg++) {
		for (size_t i = first[leg]; i < end[leg]; i++)
			matches->side[leg][i] = SIZE_MAX;
	}
	for (size_t i = 0; i < window[0].nlines; i++) {
		size_t boundary[2], mate;

		mate = candidates.side[0][i];
		if (mate == SIZE_MAX)
			continue;

		boundary[0] = first[0] + i;
		boundary[1] = first[1] + mate;
		match_interval(images, matches, cursor, boundary);
		for (int leg = 0; leg < 2; leg++) {
			matches->side[leg][boundary[leg]] = boundary[!leg];
			cursor[leg] = boundary[leg] + 1;
		}
	}
	match_interval(images, matches, cursor, end);

	/* Equal blank bytes can still belong to different parts of the body */
	for (size_t i = first[0]; i < end[0]; i++) {
		size_t at[2] = { i, matches->side[0][i] };

		if (at[1] == SIZE_MAX ||
		    line_has_content(&images[0].lines[i]) ||
		    same_blank_context(images, matches, first, end, at))
			continue;

		matches->side[0][i] = SIZE_MAX;
		matches->side[1][at[1]] = SIZE_MAX;
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

static bool blank_neighbors_correspond(const struct comparison *review,
				       const struct udiff_matches *matches,
				       const size_t *rows, int direction)
{
	int stage = review->source[0].rows[rows[0]].sign == '+';
	size_t neighbor[2] = { SIZE_MAX, SIZE_MAX };
	bool boundary[2] = {};

	for (int leg = 0; leg < 2; leg++) {
		const struct patch_source *source = &review->source[leg];
		const struct source_view *view = &source->view[stage];
		size_t at;

		for (at = view->index[rows[leg]] + direction; at < view->count;
		     at += direction) {
			const struct patch_row *next =
				&source->rows[view->rows[at]];

			if (next->sign == '?')
				break;

			if (line_has_content(&next->text) &&
			    (next->sign == ' ' ||
			     matches->side[leg][at] != SIZE_MAX)) {
				neighbor[leg] = view->rows[at];
				break;
			}
		}
		boundary[leg] = at >= view->count;
	}
	return (boundary[0] && boundary[1]) ||
	       (neighbor[0] != SIZE_MAX && neighbor[1] != SIZE_MAX &&
		source_mate(review, matches, 0, stage, neighbor[0]) ==
			neighbor[1]);
}

/* A blank edit needs a neighboring correspondence to identify its site */
static bool same_blank_edit_site(const struct comparison *review, size_t row,
				 size_t mate)
{
	int stage = review->source[0].rows[row].sign == '+';
	const size_t rows[2] = { row, mate };

	for (int direction = -1; direction <= 1; direction += 2) {
		if (blank_neighbors_correspond(review, &review->matches[stage],
					       rows, direction))
			return true;
	}

	/* Reindented edits need support on both sides of the blank */
	return review->alignment[stage].side[0] &&
	       blank_neighbors_correspond(review, &review->alignment[stage],
					  rows, -1) &&
	       blank_neighbors_correspond(review, &review->alignment[stage],
					  rows, 1);
}

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

	/* Reject the raw match too, so layout can't reuse a bad site */
	for (size_t i = 0; i < review->source[0].nrows; i++) {
		const struct patch_row *row = &review->source[0].rows[i];
		size_t mate = review->edits[0][i];
		size_t left, right;
		int stage;

		if (mate == SIZE_MAX || line_has_content(&row->text) ||
		    same_blank_edit_site(review, i, mate))
			continue;

		stage = row->sign == '+';
		left = review->source[0].view[stage].index[i];
		right = review->source[1].view[stage].index[mate];
		review->matches[stage].side[0][left] = SIZE_MAX;
		review->matches[stage].side[1][right] = SIZE_MAX;
		if (review->alignment[stage].side[0]) {
			review->alignment[stage].side[0][left] = SIZE_MAX;
			review->alignment[stage].side[1][right] = SIZE_MAX;
		}
		review->edits[0][i] = SIZE_MAX;
		review->edits[1][mate] = SIZE_MAX;
	}
}

/* Retained parent boundaries prevent an edit from moving to another site */
static void match_result_interval(const struct udiff_image *images,
				  struct udiff_matches *matches,
				  const struct udiff_matches *anchors,
				  const size_t *first, const size_t *end,
				  bool unique)
{
	size_t cursor[2] = { first[0], first[1] };

	for (size_t i = first[0]; anchors->side[0] && i < end[0]; i++) {
		size_t boundary[2] = { i, anchors->side[0][i] };

		if (boundary[1] < cursor[1] || boundary[1] >= end[1])
			continue;

		if (unique)
			match_function_interval(images, matches, cursor,
						boundary);
		else
			match_interval(images, matches, cursor, boundary);
		matches->side[0][i] = boundary[1];
		matches->side[1][boundary[1]] = i;
		for (int leg = 0; leg < 2; leg++)
			cursor[leg] = boundary[leg] + 1;
	}
	if (unique)
		match_function_interval(images, matches, cursor, end);
	else
		match_interval(images, matches, cursor, end);
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
 * Unique retained parent rows keep their identity at the tips. Other matches
 * remain flexible around insertions and deletions. Hunk grouping contributes no
 * evidence: changing the amount of quoted context cannot move an edit.
 */
static void match_results(struct comparison *review,
			  const struct udiff_image *parents,
			  const struct udiff_image *images)
{
	const struct source_view *parent = &review->source[0].view[0];
	struct udiff_matches anchors __free(udiff_matches) = {};
	struct udiff_matches *matches = &review->matches[1];
	struct line_occurrence *keys __free(free) = NULL;
	size_t first[2] = {}, end[2], count = 0;

	keys = collect_line_occurrences(parents, &parents[0],
					review->matches[0].side[0], &count);
	init_unmatched_lines(images, matches);
	for (size_t i = 0; i < parent->count; i++) {
		size_t indexes[2], mate;
		bool survives = true;

		mate = review->matches[0].side[0][i];
		if (mate == SIZE_MAX ||
		    !line_is_unique_in_both(keys, count, &parents[0].lines[i]))
			continue;

		indexes[0] = parent->rows[i];
		indexes[1] = review->source[1].view[0].rows[mate];

		/* Only rows retained by both patches can anchor results */
		for (int leg = 0; leg < 2; leg++) {
			const struct patch_source *source =
				&review->source[leg];
			size_t at = indexes[leg];

			survives &= source->rows[at].sign == ' ';
		}
		if (!survives)
			continue;

		/* Translate surviving source rows into result-view positions */
		for (int leg = 0; leg < 2; leg++)
			end[leg] =
				review->source[leg].view[1].index[indexes[leg]];

		/* Added rows cannot cross these surviving parent identities */
		match_result_interval(images, matches, &anchors, first, end,
				      false);
		matches->side[0][end[0]] = end[1];
		matches->side[1][end[1]] = end[0];
		for (int leg = 0; leg < 2; leg++)
			first[leg] = end[leg] + 1;
	}
	end[0] = images[0].nlines;
	end[1] = images[1].nlines;
	match_result_interval(images, matches, &anchors, first, end, false);
}

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
	match_results(review, images[0], images[1]);
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

/* Quoted source or a unique match can locate retained counterparts */
static void select_counterparts(struct comparison *review, int stage)
{
	struct line_occurrence *keys __free(free) = NULL;
	struct udiff_image images[2];
	size_t count = 0;

	for (int leg = 0; leg < 2; leg++) {
		const struct source_view *view =
			&review->source[leg].view[stage];

		images[leg] = (typeof(images[0])){ .lines = view->lines,
						   .nlines = view->count };
	}
	if (review->source[0].complete)
		keys = collect_line_occurrences(images, &images[0],
						review->matches[stage].side[0],
						&count);
	for (int leg = 0; leg < 2; leg++) {
		const struct patch_source *source = &review->source[leg];
		const struct patch_source *other = &review->source[!leg];
		char sign = stage ? '+' : '-';

		for (size_t i = 0; i < source->nrows; i++) {
			const struct patch_row *row = &source->rows[i];
			size_t mate;

			if (!review->selected[leg][i] || row->sign != sign)
				continue;

			mate = review_mate(review, leg, stage, i);
			if (mate == SIZE_MAX || other->rows[mate].sign != ' ')
				continue;

			if (other->rows[mate].hunk != SIZE_MAX ||
			    line_is_unique_in_both(keys, count, &row->text))
				review->selected[!leg][mate] = true;
		}
	}
}

static void select_margin(const struct patch_source *source, bool *selected,
			  size_t at, unsigned int context, int direction)
{
	for (size_t n = 0; n < context; n++) {
		if ((!at && direction < 0) ||
		    (at + 1 == source->nrows && direction > 0))
			break;

		at = direction < 0 ? at - 1 : at + 1;

		/* An unquoted gap gives no source to use as display context */
		if (source->rows[at].sign == '?')
			break;

		selected[at] = true;
	}
}

static void select_delta(struct comparison *review, unsigned int context,
			 bool shared_deletion)
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

			if ((shared_deletion && row->sign == '-') ||
			    patch_row_partner(source, i)) {
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
	for (int stage = 0; stage < 2; stage++)
		select_counterparts(review, stage);
	for (int leg = 0; leg < 2; leg++) {
		const struct patch_source *source = &review->source[leg];
		bool *seeds __free(free) =
			xmalloc_array(source->nrows, sizeof(*seeds));

		/* Freeze the seeds so newly selected context can't grow more */
		memcpy(seeds, review->selected[leg],
		       source->nrows * sizeof(*seeds));
		for (size_t i = 0; i < source->nrows; i++) {
			if (!seeds[i])
				continue;

			for (int direction = -1; direction <= 1;
			     direction += 2) {
				select_margin(source, review->selected[leg], i,
					      context, direction);
			}
		}
	}
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

static void align_unpaired(struct paired_rows *pairs, size_t *at,
			   const size_t *end)
{
	for (; at[0] < end[0]; at[0]++)
		append_pair(pairs, at[0], SIZE_MAX, false);
	for (; at[1] < end[1]; at[1]++)
		append_pair(pairs, SIZE_MAX, at[1], false);
}

static void align_fallback(struct paired_rows *pairs, size_t *at,
			   const size_t *end,
			   const struct source_view *views[2], bool backward)
{
	if (backward) {
		/* Keep excess rows above a suffix's following anchor */
		size_t count = MIN(end[0] - at[0], end[1] - at[1]);
		size_t start[2] = { end[0] - count, end[1] - count };

		align_unpaired(pairs, at, start);
	}
	align_until(pairs, at, end, views);
}

static unsigned int replacement_similarity(const struct udiff_line *a,
					   const struct udiff_line *b)
{
	struct udiff_matches matches __free(udiff_matches) = {};
	size_t total_bytes = a->len + b->len, count = 0, common = 0;
	size_t identifiers;
	const struct udiff_line *source[2] = { a, b };
	struct udiff_line *lines __free(free) = NULL;
	struct udiff_image image[2] = {};
	char *bytes __free(free) = NULL;

	if (total_bytes > REPLACEMENT_MAX_BYTES || patch_row_equal(a, b))
		return 0;

	lines = xmalloc_array(total_bytes, sizeof(*lines));
	bytes = xmalloc_array(total_bytes, 2);

	/*
	 * Identifier bytes carry more evidence than whitespace and punctuation.
	 * Encode each as a separate line for libgit2, without locale-dependent
	 * expansion, so terminal settings cannot change the chosen rows. At
	 * least three quarters must survive to displace positional pairing.
	 */
	for (int leg = 0; leg < 2; leg++) {
		image[leg].lines = lines + count;
		image[leg].bytes = bytes + 2 * count;
		for (size_t i = 0; i < source[leg]->len; i++) {
			char c = source[leg]->ptr[i];

			if ((c < 'a' || c > 'z') && (c < 'A' || c > 'Z') &&
			    (c < '0' || c > '9') && c != '_' && c < 0x80)
				continue;

			bytes[2 * count] = c;
			bytes[2 * count + 1] = '\n';
			lines[count] =
				(typeof(lines[0])){ .ptr = bytes + 2 * count,
						    .len = 2 };
			count++;
			image[leg].nlines++;
		}
	}
	udiff_match(&image[0], &image[1], &matches);
	for (size_t i = 0; i < image[0].nlines; i++)
		common += matches.side[0][i] != SIZE_MAX;

	/* Reward common bytes and subtract the longer side's unmatched bytes */
	identifiers = MAX(image[0].nlines, image[1].nlines);
	return common * 4 >= identifiers * 3 ? common * 2 - identifiers : 0;
}

static const struct patch_row *gap_row(const struct comparison *review,
				       const struct source_view *views[2],
				       int leg, size_t at)
{
	return &review->source[leg].rows[views ? views[leg]->rows[at] : at];
}

struct replacement_match {
	size_t mate;
	unsigned int score;
};

static void consider_replacement(struct replacement_match *best, size_t mate,
				 unsigned int score)
{
	/*
	 * Keep the tied score so a later equal candidate cannot break the tie.
	 */
	if (score > best->score)
		*best = (typeof(*best)){ .mate = mate, .score = score };
	else if (score == best->score)
		best->mate = SIZE_MAX;
}

static void align_known_replacements(const struct comparison *review,
				     struct paired_rows *pairs, size_t *at,
				     const size_t *end,
				     const struct source_view *views[2],
				     bool backward)
{
	size_t count[2] = { end[0] - at[0], end[1] - at[1] };
	size_t first[2] = { at[0], at[1] }, last = SIZE_MAX;
	struct replacement_match *best __free(free) = NULL;
	struct replacement_match *right;

	/*
	 * Empty or single-row gaps need no candidate search. Large gaps also
	 * retain positional pairing to bound quadratic work.
	 */
	if (!count[0] || !count[1] || (count[0] == 1 && count[1] == 1) ||
	    count[0] > REPLACEMENT_MAX_PAIRS / count[1]) {
		align_fallback(pairs, at, end, views, backward);
		return;
	}

	best = xzalloc_array(count[0] + count[1], sizeof(*best));
	right = best + count[0];

	/*
	 * These pairs affect layout only. Exact parent/result matches still
	 * determine equality, edit ownership, and the boundaries of this gap.
	 */
	for (size_t i = 0; i < count[0]; i++) {
		const struct patch_row *a =
			gap_row(review, views, 0, first[0] + i);

		for (size_t j = 0; j < count[1]; j++) {
			const struct patch_row *b =
				gap_row(review, views, 1, first[1] + j);
			unsigned int weight = 0;

			if (views || a->sign == b->sign || a->sign == ' ' ||
			    b->sign == ' ')
				weight = replacement_similarity(&a->text,
								&b->text);
			consider_replacement(&best[i], j, weight);
			consider_replacement(&right[j], i, weight);
		}
	}

	/* Tied or crossed matches cannot choose a reliable source occurrence */
	for (size_t i = 0; i < count[0]; i++) {
		size_t mate = best[i].mate;

		if (!best[i].score || mate == SIZE_MAX ||
		    right[mate].mate != i) {
			best[i].mate = SIZE_MAX;
			continue;
		}

		if (last != SIZE_MAX && mate < last) {
			align_fallback(pairs, at, end, views, backward);
			return;
		}
		last = mate;
	}
	for (size_t i = 0; i < count[0]; i++) {
		size_t boundary[2];

		if (best[i].mate == SIZE_MAX)
			continue;

		boundary[0] = first[0] + i;
		boundary[1] = first[1] + best[i].mate;
		align_fallback(pairs, at, boundary, views, backward);
		append_pair(pairs, at[0]++, at[1]++, false);
	}
	align_fallback(pairs, at, end, views, backward);
}

static void align_replaced_additions(const struct comparison *review,
				     struct paired_rows *pairs, size_t *at,
				     const size_t *end)
{
	size_t next[2] = { at[0], at[1] };

	if (!at[0] || !at[1] ||
	    gap_row(review, NULL, 0, at[0] - 1)->sign != '-' ||
	    gap_row(review, NULL, 1, at[1] - 1)->sign != '-' ||
	    review_mate(review, 0, 0, at[0] - 1) != at[1] - 1)
		return;

	/* A shared removal locates its immediately following replacement */
	for (int leg = 0; leg < 2; leg++) {
		for (; next[leg] < end[leg]; next[leg]++) {
			if (gap_row(review, NULL, leg, next[leg])->sign != '+')
				break;
		}
	}
	if (next[0] > at[0] && next[1] > at[1])
		align_known_replacements(review, pairs, at, next, NULL, false);
}

static size_t quoted_island_position(const struct comparison *review,
				     const struct source_view *views[2],
				     int leg, size_t at, size_t end)
{
	for (; at < end; at++) {
		const struct patch_row *row = gap_row(review, views, leg, at);

		if (row->sign != '?')
			return row->pos[0];
	}
	return SIZE_MAX;
}

/*
 * Order unpaired islands by position so reversal keeps the same groups. These
 * coordinates order presentation only; they cannot establish a match.
 */
static void align_unknown_islands(const struct comparison *review,
				  struct paired_rows *pairs, size_t *at,
				  const size_t *end,
				  const struct source_view *views[2])
{
	while (at[0] < end[0] || at[1] < end[1]) {
		size_t key[2], stop[2], next;

		for (int leg = 0; leg < 2; leg++) {
			key[leg] = quoted_island_position(review, views, leg,
							  at[leg], end[leg]);
		}

		next = MIN(key[0], key[1]);

		/* Islands starting together share one group after both gaps */
		for (int leg = 0; leg < 2; leg++) {
			stop[leg] = at[leg];
			if (at[leg] == end[leg] || key[leg] != next)
				continue;
			for (; stop[leg] < end[leg]; stop[leg]++) {
				if (gap_row(review, views, leg, stop[leg])
					    ->sign != '?')
					break;
			}
		}
		align_unpaired(pairs, at, stop);
		for (int leg = 0; leg < 2; leg++) {
			if (at[leg] == end[leg] || key[leg] != next)
				continue;
			for (; stop[leg] < end[leg]; stop[leg]++) {
				if (gap_row(review, views, leg, stop[leg])
					    ->sign == '?')
					break;
			}
		}
		align_unpaired(pairs, at, stop);
	}
}

static void align_replacements(const struct comparison *review,
			       struct paired_rows *pairs, size_t *at,
			       const size_t *end,
			       const struct source_view *views[2])
{
	size_t first[2] = { end[0], end[1] }, last[2] = {};

	for (int leg = 0; leg < 2; leg++) {
		for (size_t i = at[leg]; i < end[leg]; i++) {
			if (gap_row(review, views, leg, i)->sign != '?')
				continue;

			first[leg] = MIN(first[leg], i);
			last[leg] = i + 1;
		}
	}
	if (!last[0] && !last[1]) {
		align_known_replacements(review, pairs, at, end, views, false);
		return;
	}

	/* A gapless opposite interval has no boundary at which to split it */
	if (!last[0] || !last[1]) {
		if (!views)
			align_replaced_additions(review, pairs, at, end);
		align_unknown_islands(review, pairs, at, end, views);
		return;
	}

	/*
	 * Only the quoted prefix and suffix attach to the surrounding anchors.
	 * Rows between the first and last gap have no reliable correspondence.
	 */
	align_until(pairs, at, first, views);
	align_unknown_islands(review, pairs, at, last, views);

	/*
	 * The suffix is wholly quoted. Similar rows anchor it; unmatched rows
	 * attach backward to the next anchor, leaving any excess above.
	 */
	align_known_replacements(review, pairs, at, end, views, true);
}

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
		align_replacements(review, pairs, at, next, NULL);
		append_pair(pairs, at[0]++, at[1]++,
			    review_mate(review, 0, 0, i) == mate);
	}
	align_replacements(review, pairs, at, end, NULL);
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
			align_replacements(review, pairs, at, end, views);
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
		align_replacements(review, pairs, at, end, views);
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

static bool row_in_scope(const struct comparison *review, int leg, size_t i)
{
	const struct patch_source *source = &review->source[leg];
	const struct patch_row *row = &source->rows[i];
	const struct patch_move *focus = review->focus[leg];
	const struct patch_file *file = source->file;

	if (!focus)
		return !patch_row_partner(source, i);

	if (row->move == focus)
		return true;

	if (row->sign != ' ' || row->hunk == SIZE_MAX)
		return false;

	return row->hunk >= file->rows[focus->first].hunk &&
	       row->hunk <= file->rows[focus->first + focus->count - 1].hunk;
}

/* Quoted source stays in scope even when the requested margin is zero */
static void context_scope(const struct comparison *review, int leg,
			  unsigned int context, bool *near)
{
	const struct patch_source *source = &review->source[leg];
	size_t at = 0, marked = 0, count = source->view[1].count;

	for (size_t i = 0; i < source->nrows; i++) {
		const struct patch_row *row = &source->rows[i];
		bool active = row_in_scope(review, leg, i);

		if ((row->sign == '-' || row->sign == '+') && active) {
			size_t width = (size_t)context + (row->sign == '+');
			size_t first = at > context ? at - context : 0;
			size_t end = at + MIN(width, count - at);

			for (size_t j = MAX(first, marked); j < end; j++)
				near[j] = true;
			marked = MAX(marked, end);
		}
		if (source->view[1].index[i] != SIZE_MAX) {
			if (row->hunk != SIZE_MAX && active)
				near[at] = true;
			at++;
		}
	}
}

/*
 * Grow from the original selections only. A differing row exposed by a margin
 * belongs to that excerpt; it does not seed another expansion. A patch margin
 * stops at a region whose correspondence requires unknown source.
 */
static void grow_context_margins(const struct patch_source *source,
				 const struct paired_rows *pairs,
				 bool *selected, unsigned int context, int leg)
{
	for (size_t i = 0; i < source->view[1].count; i++)
		selected[i] = false;

	/* Separate passes preserve seeds while growing both margins */
	for (int direction = 0; direction < 2; direction++) {
		size_t remaining = 0;

		for (size_t n = 0; n < pairs->count; n++) {
			size_t i = direction ? pairs->count - 1 - n : n;
			const struct row_pair *pair = &pairs->rows[i];
			size_t at = pair->side[leg];

			/* Unquoted source cannot connect two known excerpts */
			if (!pair->equal && !pair->known) {
				remaining = 0;
				continue;
			}

			/* A missing counterpart seeds a margin without a row */
			if (at == SIZE_MAX) {
				if (pair->gap_seed[leg])
					remaining = context;
				continue;
			}

			if (source->rows[at].sign == '?')
				remaining = 0;
			else if (pair->selected[leg])
				remaining = (size_t)context + 1;
			if (remaining) {
				size_t pos = source->view[1].index[at];

				selected[pos] = true;
				remaining--;
			}
		}
	}
}

static void context_margins(const struct comparison *review,
			    struct paired_rows *pairs, bool *selected[2],
			    unsigned int context)
{
	for (int leg = 0; leg < 2; leg++) {
		grow_context_margins(&review->source[leg], pairs, selected[leg],
				     context, leg);
	}

	/* Publish after both passes so a grown margin cannot seed another */
	for (size_t i = 0; i < pairs->count; i++) {
		struct row_pair *pair = &pairs->rows[i];

		for (int leg = 0; leg < 2; leg++) {
			const struct source_view *view =
				&review->source[leg].view[1];
			size_t at = pair->side[leg];

			pair->selected[leg] = at != SIZE_MAX &&
					      selected[leg][view->index[at]];
		}
	}
}

/* Unequal correspondences can bound known text beside an unquoted gap */
static void mark_known_context(const struct comparison *review,
			       struct paired_rows *pairs)
{
	bool unknown = false;
	size_t first = 0;

	for (size_t i = 0; i <= pairs->count; i++) {
		if (i < pairs->count) {
			struct row_pair *pair = &pairs->rows[i];

			for (int leg = 0; leg < 2; leg++) {
				const struct patch_source *source =
					&review->source[leg];
				size_t at = pair->side[leg];

				unknown |= at != SIZE_MAX &&
					   source->rows[at].sign == '?';
			}
			if (pair->side[0] == SIZE_MAX ||
			    pair->side[1] == SIZE_MAX ||
			    layout_mate(review, 0, 1, pair->side[0]) !=
				    pair->side[1])
				continue;

			pair->known = true;
		}
		if (!unknown) {
			for (size_t j = first; j < i; j++)
				pairs->rows[j].known = true;
		}
		first = i + 1;
		unknown = false;
	}
}

/* Full-source matches identify differences; patch regions select excerpts */
static void select_context(const struct comparison *review,
			   struct paired_rows *pairs, unsigned int context)
{
	bool *right __free(free) = NULL;
	bool *left __free(free) = NULL;
	bool *near[2];

	if (review->source[0].ambiguous || review->source[1].ambiguous)
		return;

	left = xzalloc_array(review->source[0].view[1].count, sizeof(*left));
	right = xzalloc_array(review->source[1].view[1].count, sizeof(*right));
	near[0] = left;
	near[1] = right;
	context_scope(review, 0, context, left);
	context_scope(review, 1, context, right);
	mark_known_context(review, pairs);

	/* Context needs retained source; edits alone belong to delta */
	for (size_t i = 0; i < pairs->count;) {
		bool present[2] = {};
		bool retained = false;
		size_t first = i;

		if (pairs->rows[i].equal || !pairs->rows[i].known) {
			i++;
			continue;
		}

		for (; i < pairs->count && !pairs->rows[i].equal &&
		       pairs->rows[i].known;
		     i++) {
			for (int leg = 0; leg < 2; leg++) {
				const struct patch_source *source =
					&review->source[leg];
				size_t at;

				at = pairs->rows[i].side[leg];
				if (at == SIZE_MAX)
					continue;

				present[leg] = true;
				retained |=
					source->rows[at].sign == ' ' &&
					near[leg][source->view[1].index[at]];
			}
		}
		if (!retained)
			continue;

		/* Keep only the difference near the original patch */
		for (size_t j = first; j < i; j++) {
			for (int leg = 0; leg < 2; leg++) {
				const struct source_view *view =
					&review->source[leg].view[1];
				size_t at = pairs->rows[j].side[leg];

				pairs->rows[j].selected[leg] =
					at != SIZE_MAX &&
					near[leg][view->index[at]];
			}
		}

		/* An empty side still has a source gap needing context */
		for (int leg = 0; leg < 2; leg++) {
			if (!present[leg])
				pairs->rows[first].gap_seed[leg] = true;
		}
	}

	/* Reuse the proximity maps for the final selection and its margins */
	context_margins(review, pairs, near, context);
}

static char *source_name(const struct patch_source *source,
			 const struct patch_file *other, size_t first,
			 bool result)
{
	const struct source_view *view =
		&source->view[result || source->rows[first].sign == '+'];
	size_t position = view->index[first], begin = position,
	       end = position + 1;
	size_t hunk = source->rows[first].hunk;
	struct iomem_slice name = {};
	struct udiff_image image;
	size_t definition;

	if (source->complete) {
		begin = 0;
		end = view->count;
	} else {
		/* Unquoted patch boundaries limit declaration searches */
		for (;
		     begin && source->rows[view->rows[begin - 1]].hunk == hunk;
		     begin--)
			;
		for (; end < view->count &&
		       source->rows[view->rows[end]].hunk == hunk;
		     end++)
			;
	}
	image = (typeof(image)){ .lines = view->lines + begin,
				 .nlines = end - begin };
	definition = source_definition(
		&image, position - begin,
		path_names_c_source(patch_file_path(source->file ?: other)));
	if (definition != SIZE_MAX)
		name = (typeof(name)){ .base = image.lines[definition].ptr,
				       .len = image.lines[definition].len };

	/* A patch may quote no declaration, leaving only its hunk heading */
	if (!name.len && source->file && hunk != SIZE_MAX)
		name = source->file->hunks[hunk].name;
	for (; name.len && isspace(name.base[0]); name.len--)
		name.base++;
	for (; name.len && isspace(name.base[name.len - 1]); name.len--)
		;
	return name.len ? memdup(name.base, name.len) : NULL;
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
			const struct patch_source *source =
				&review->source[leg];
			size_t at = pair->side[leg];

			if (!pair->selected[leg])
				continue;

			if (previous[leg] == SIZE_MAX)
				block->name[leg] = source_name(
					source, review->source[!leg].file, at,
					context);
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

/* Null IDs name absence; present IDs compare at their common written width */
static bool same_blob(const struct iomem_slice *a, const struct iomem_slice *b)
{
	const struct iomem_slice *ids[2] = { a, b };
	bool zero[2] = { true, true };

	for (int leg = 0; leg < 2; leg++) {
		for (size_t i = 0; i < ids[leg]->len; i++)
			zero[leg] &= ids[leg]->base[i] == '0';
	}
	if (zero[0] || zero[1])
		return zero[0] == zero[1];

	/* A full SHA-1 ID cannot abbreviate an ID from a longer hash family */
	if ((a->len == 40 && b->len > 40) || (b->len == 40 && a->len > 40))
		return false;

	return !memcmp(a->base, b->base, MIN(a->len, b->len));
}

static char *format_file_metadata(const struct patch_file *file, bool show_id)
{
	struct iomem_writer writer = {};
	const struct block_story *story;
	struct iomem_buf out = {};

	if (!file)
		return NULL;

	story = &file->story;
	iomem_writer_open(&writer);
	if (file->empty[0])
		fprintf(writer.fp, "Create file\n");
	if (file->empty[1])
		fprintf(writer.fp, "Delete file\n");
	if (story->mode.old_mode[0])
		fprintf(writer.fp, "Old mode: %s\n", story->mode.old_mode);
	if (story->mode.new_mode[0])
		fprintf(writer.fp, "New mode: %s\n", story->mode.new_mode);
	if (file->operation_name[0]) {
		char *old_text __free(free) =
			git_quote_name(file->operation_name[0]);
		char *new_text __free(free) =
			git_quote_name(file->operation_name[1]);

		fprintf(writer.fp, "%s: %s -> %s\n",
			file->copy ? "Copy" : "Rename", old_text, new_text);
	}

	/* Blob identities carry the difference when source is not quoted */
	if (story->has_binary)
		fputs("Binary file\n", writer.fp);
	if (show_id && story->has_index)
		fprintf(writer.fp, "Result blob: %.*s\n",
			(int)story->index_post.len, story->index_post.base);
	iomem_writer_publish(&writer, &out);
	if (!out.len) {
		iomem_buf_free(&out);
		return NULL;
	}

	return out.base;
}

static void review_file_build(struct review_file *file,
			      const struct patch_file *a,
			      const struct patch_file *b)
{
	struct comparison review __free(comparison) = {};
	const struct iomem_slice absent = {};
	bool shared_deletion;
	bool show_id = false;

	patch_source_read(&review.source[0], a, b, 0, gittree_active());
	patch_source_read(&review.source[1], b, a, 1, gittree_active());
	compare_sources(&review);

	/* An irreversible deletion omits text, not the whole-file operation */
	shared_deletion = a && b && a->empty[1] && b->empty[1] &&
			  (!a->nhunks || !b->nhunks) && a->story.has_index &&
			  b->story.has_index &&
			  !same_blob(&a->story.index_pre, &absent) &&
			  same_blob(&a->story.index_pre, &b->story.index_pre);
	select_delta(&review, max_context, shared_deletion);

	for (int section = 0; section < 2; section++) {
		struct paired_rows pairs __free(paired_rows) = {};

		pair_rows(&review, &pairs, section);
		if (section)
			select_context(&review, &pairs, max_context);
		else
			select_pairs(&review, &pairs);
		save_section(&review, &pairs, &file->section[section], section);
	}
	for (int leg = 0; leg < 2; leg++) {
		const struct patch_source *source = &review.source[leg];

		file->path[leg] = file_label(source, &review.source[!leg]);
		file->ambiguous |= source->ambiguous;
		if (source->file && source->file->empty[1] &&
		    !source->file->nhunks && !shared_deletion)
			file->note[leg] = xstrdup(
				"This patch deletes the whole file without quoting its source.\n");
	}
	/* Original file comparisons own operations and blob metadata */
	if (a && b) {
		show_id =
			a->story.has_index && b->story.has_index &&
			(a->story.has_binary || b->story.has_binary ||
			 !a->nhunks || !b->nhunks) &&
			!same_blob(&a->story.index_post, &b->story.index_post);
	} else {
		const struct patch_file *only = a ?: b;

		show_id = only && (only->story.has_binary || !only->nhunks);
	}
	file->metadata[0] = format_file_metadata(a, show_id);
	file->metadata[1] = format_file_metadata(b, show_id);
	file->metadata_diff =
		strcmp(file->metadata[0] ?: "", file->metadata[1] ?: "") != 0;
	if (a && b && a->story.has_binary && b->story.has_binary &&
	    (!a->story.has_index || !b->story.has_index) &&
	    (a->binary.len != b->binary.len ||
	     (a->binary.len &&
	      memcmp(a->binary.base, b->binary.base, a->binary.len)))) {
		char *previous __free(free) = file->note[0];

		file->metadata_diff = true;
		xasprintf(&file->note[0],
			  "%sThe quoted binary patch data differs.\n",
			  previous ?: "");
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
