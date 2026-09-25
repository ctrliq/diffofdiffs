// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ctrl IQ, Inc.
 *
 * Preserve the original forward edits, then supply the known source around
 * them. Parsing, source validation, and ownership live here; matching and
 * rendering borrow these records without rewriting them.
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include <gittree.h>
#include <patch.h>
#include <util.h>

bool patch_row_equal(const struct udiff_line *a, const struct udiff_line *b)
{
	return a->len == b->len && !memcmp(a->ptr, b->ptr, a->len);
}

const char *patch_file_path(const struct patch_file *file)
{
	const struct patch_name *name = file->record->file;

	return stripped(name->text, name->prefix == PATCH_PREFIX_GIT);
}

static bool line_has_prefix(const struct iomem_line *line, const char *word)
{
	size_t n = strlen(word);

	return line->len >= n && !memcmp(line->base, word, n);
}

static bool peek_patch_line(const struct iomem_cursor *cur,
			    struct iomem_line *line)
{
	struct iomem_cursor copy = *cur;

	return iomem_cursor_next(&copy, line);
}

static void mode_overlay(struct file_mode *to, const struct file_mode *from)
{
	if (from->old_mode[0])
		memcpy(to->old_mode, from->old_mode, sizeof(to->old_mode));
	if (from->new_mode[0])
		memcpy(to->new_mode, from->new_mode, sizeof(to->new_mode));
	to->created |= from->created;
	to->deleted |= from->deleted;
}

static void append_patch_row(struct patch_file *file,
			     const struct patch_row *row)
{
	if (file->nrows == 2 * LINE_CEILING)
		die("too many quoted lines in %s", patch_file_path(file));

	if (file->nrows == file->row_cap) {
		file->row_cap = file->row_cap ? file->row_cap * 2 : 64;
		file->rows = xrealloc_array(file->rows, file->row_cap,
					    sizeof(*file->rows));
	}
	file->rows[file->nrows] = *row;
	file->nrows++;
}

static void read_hunk(struct patch_file *file, struct iomem_cursor *cur,
		      const struct iomem_line *header, const char *label)
{
	struct patch_hunk *hunk;
	unsigned long off[2], count[2];
	struct iomem_line line;
	size_t left[2], pos[2], tail;
	const char *name;

	read_atatline_n(header->base, header->len, &off[0], &count[0], &off[1],
			&count[1]);

	/* Empty ranges name an insertion point, not a one-based source line */
	for (int s = 0; s < 2; s++) {
		pos[s] = off[s] - (off[s] && count[s]);
		left[s] = count[s];
	}
	if (file->nhunks == file->hunk_cap) {
		file->hunk_cap = file->hunk_cap ? file->hunk_cap * 2 : 8;
		file->hunks = xrealloc_array(file->hunks, file->hunk_cap,
					     sizeof(*file->hunks));
	}
	hunk = &file->hunks[file->nhunks++];
	tail = atat_tail(header->base, header->len, &name);
	*hunk = (typeof(*hunk)){ .name = { .base = name, .len = tail },
				 .first = file->nrows,
				 .pos = { pos[0], pos[1] },
				 .lines = { count[0], count[1] } };
	while (peek_patch_line(cur, &line)) {
		struct patch_row row = { .pos = { pos[0], pos[1] },
					 .hunk = file->nhunks - 1 };
		enum body_class kind;
		size_t prefix;

		kind = classify_body_line(line.base, line.len, &prefix);
		if (kind == BODY_NO_NEWLINE && file->nrows > hunk->first) {
			struct udiff_line *last =
				&file->rows[file->nrows - 1].text;

			/*
			 * The final counted row can still have a following
			 * marker.
			 */
			if (last->len && last->ptr[last->len - 1] == '\n')
				last->len--;
			iomem_cursor_next(cur, &line);
			continue;
		}

		if (!left[0] && !left[1])
			break;

		iomem_cursor_next(cur, &line);
		if (memchr(line.base, '\0', line.len))
			die("%s carries an embedded NUL byte", label);

		row.sign = kind == BODY_REMOVED ? '-' :
			   kind == BODY_ADDED	? '+' :
						  ' ';
		row.text.ptr = line.base + prefix;
		row.text.len = line.len - prefix + line.has_lf;
		if (!line.has_lf) {
			/*
			 * Only a marker asserts source EOF. Missing patch LF
			 * can occur only on its last row, so this allocation is
			 * unique.
			 */
			file->nlterm = xmalloc(row.text.len + 1);
			memcpy(file->nlterm, row.text.ptr, row.text.len);
			file->nlterm[row.text.len++] = '\n';
			row.text.ptr = file->nlterm;
		}
		append_patch_row(file, &row);
		for (int s = 0; s < 2; s++) {
			char absent_sign = s ? '-' : '+';

			if (row.sign == absent_sign)
				continue;

			pos[s]++;
			left[s]--;
		}
	}
	hunk->count = file->nrows - hunk->first;
}

/*
 * An adjacent replacement of a sequence by itself has no effect. Context
 * between removal and addition keeps them separate actions, including moves.
 */
static void mark_neutral(struct patch_file *file)
{
	for (size_t first = 0; first < file->nrows;) {
		size_t mid, end;

		for (mid = first;
		     mid < file->nrows && file->rows[mid].sign == '-' &&
		     file->rows[mid].hunk == file->rows[first].hunk;
		     mid++)
			;
		for (end = mid;
		     end < file->nrows && file->rows[end].sign == '+' &&
		     file->rows[end].hunk == file->rows[first].hunk;
		     end++)
			;
		if (mid > first && mid - first == end - mid) {
			bool equal = true;

			for (size_t i = 0; i < mid - first; i++) {
				equal &= patch_row_equal(
					&file->rows[first + i].text,
					&file->rows[mid + i].text);
			}
			if (equal) {
				for (size_t i = first; i < end; i++)
					file->rows[i].neutral = true;
			}
		}
		first = end > first ? end : first + 1;
	}
}

static void read_operation_names(struct patch_file *file)
{
	struct iomem_slice names[2] = { file->story.rename_from,
					file->story.rename_to };

	if (!names[0].len || !names[1].len) {
		names[0] = file->story.copy_from;
		names[1] = file->story.copy_to;
		if (!names[0].len || !names[1].len)
			return;

		file->copy = true;
	}
	for (int stage = 0; stage < 2; stage++) {
		file->operation_name[stage] =
			unquoted_name(names[stage].base, names[stage].len);
	}
}

static void read_file(struct patch_file *file, const struct iomem_buf *buf,
		      size_t end, const struct file_mode *head,
		      const char *label)
{
	struct iomem_cursor cur;
	struct iomem_line line;
	long opener = block_opener_above(buf, file->record->pos);
	bool in_hunks = false;

	if (opener >= 0)
		block_story_read(&file->story, buf, opener, label);

	/* Per-file metadata overrides any mode inherited before the opener */
	if (head) {
		struct file_mode mode = *head;

		mode_overlay(&mode, &file->story.mode);
		file->story.mode = mode;
	}
	iomem_cursor_init(&cur, buf);
	iomem_cursor_seek(&cur, file->record->pos, label);
	while (iomem_cursor_next(&cur, &line) && line.offset < end) {
		if (!in_hunks) {
			scan_mode_line(line.base, line.len, &file->story.mode);
			if (line_has_prefix(&line, "GIT binary patch")) {
				file->binary.base = line.base;
				file->binary.len = end - line.offset;
			}
			if (line_has_prefix(&line, "--- ")) {
				patch_name_free(file->name[0]);
				file->name[0] = filename_from_header(
					line.base + 4, line.len - 4);
			} else if (line_has_prefix(&line, "+++ ") &&
				   file->name[0]) {
				file->name[1] = filename_from_header(
					line.base + 4, line.len - 4);
				in_hunks = true;
			}
			continue;
		}

		if (!read_atatline_n(line.base, line.len, NULL, NULL, NULL,
				     NULL))
			read_hunk(file, &cur, &line, label);
	}

	/* Metadata-only files have no labels; the index supplies their name */
	for (int s = 0; s < 2; s++) {
		if (!file->name[s])
			file->name[s] = patch_name_dup(file->record->file);
		file->name[s]->prefix = file->record->file->prefix;
		file->empty[s] = !strcmp(file->name[s]->text, "/dev/null") ||
				 (s ? file->story.mode.deleted :
				      file->story.mode.created);
	}
	read_operation_names(file);
	mark_neutral(file);
}

void patch_document_read(struct patch_document *doc,
			 const struct iomem_buf *buf,
			 struct cds_list_head *index, const char *label)
{
	struct file_mode head = {};
	struct iomem_cursor cur;
	struct iomem_line line;
	struct file_list *record;
	size_t at = 0;

	iomem_cursor_init(&cur, buf);
	while (iomem_cursor_next(&cur, &line) &&
	       scan_mode_line(line.base, line.len, &head))
		;
	cds_list_for_each_entry(record, index, node)
		doc->nfiles++;
	doc->files = xzalloc_array(doc->nfiles, sizeof(*doc->files));
	cds_list_for_each_entry(record, index, node) {
		struct patch_file *file = &doc->files[at];
		size_t end = buf->len;

		if (record->node.next != index) {
			struct file_list *next = cds_list_entry(
				record->node.next, struct file_list, node);
			long opener = block_opener_above(buf, next->pos);

			/*
			 * The next opener's metadata belongs to the next file.
			 */
			end = opener > record->pos ? (size_t)opener :
						     (size_t)next->pos;
		}
		file->record = record;
		read_file(file, buf, end, at ? NULL : &head, label);
		at++;
	}
}

void patch_document_free(struct patch_document *doc)
{
	for (size_t i = 0; i < doc->nfiles; i++) {
		struct patch_file *file = &doc->files[i];

		patch_name_free(file->name[0]);
		patch_name_free(file->name[1]);
		patch_name_free(file->operation_name[0]);
		patch_name_free(file->operation_name[1]);
		free(file->nlterm);
		free(file->rows);
		free(file->hunks);
		free(file->moves);
	}
	free(doc->files);
	*doc = (typeof(*doc)){};
}

/* Compare a record label with an operation's unprefixed source path */
static bool names_operation(const struct patch_file *file,
			    const struct patch_name *name, int depth)
{
	const struct patch_name *key = file->record->file;

	if (!name || key->verbatim || name->verbatim)
		return false;

	if (key->prefix == PATCH_PREFIX_LITERAL)
		return !strcmp(stripped(key->text, depth),
			       stripped(name->text, depth));
	if (key->prefix != PATCH_PREFIX_GIT)
		return false;

	return !strcmp(stripped(key->text, MAX(depth, 1)),
		       stripped(name->text, MAX(depth - 1, 0)));
}

static bool operation_belongs(const struct patch_file *file, int depth)
{
	return names_operation(file, file->operation_name[0], depth) ||
	       names_operation(file, file->operation_name[1], depth);
}

char *patch_related_change(const struct patch_file *file,
			   const struct patch_document *other, int depth)
{
	char *note;

	for (size_t i = 0; i < other->nfiles; i++) {
		const struct patch_file *peer = &other->files[i];

		if (operation_belongs(peer, depth) &&
		    names_operation(file, peer->operation_name[0], depth)) {
			char *to __free(free) =
				git_quote_name(peer->operation_name[1]);

			xasprintf(&note,
				  "The other patch %s this file to %s.\n",
				  peer->copy ? "copies" : "renames", to);
			return note;
		}
	}
	if (!operation_belongs(file, depth))
		return NULL;

	for (size_t i = 0; i < other->nfiles; i++) {
		const struct patch_file *peer = &other->files[i];

		if (names_operation(peer, file->operation_name[0], depth)) {
			char *from __free(free) =
				git_quote_name(file->operation_name[0]);

			xasprintf(
				&note,
				"The other patch also names the source file %s (%s).\n",
				from,
				peer->nhunks ? "text changes" :
					       "file metadata");
			return note;
		}
	}
	return NULL;
}

#define FNV_OFFSET_BASIS 0xcbf29ce484222325ULL
#define FNV_PRIME 0x100000001b3ULL

enum change_extent {
	WHOLE_HUNK,
	EDIT_RUN
};

struct change_match {
	struct patch_file *file;
	const char *basename;

	/*
	 * Candidate interval in file->rows, including any intervening context.
	 */
	size_t first;
	size_t count;
	u64 hash;
	int leg;
	bool consumed;
};

static const struct patch_row *next_change(const struct change_match *match,
					   size_t *at)
{
	while (*at < match->first + match->count) {
		const struct patch_row *row = &match->file->rows[(*at)++];

		if (!row->neutral && (row->sign == '-' || row->sign == '+'))
			return row;
	}
	return NULL;
}

static int changed_rows_compare(const struct change_match *a,
				const struct change_match *b)
{
	size_t at = a->first, bt = b->first;

	if (a->hash != b->hash)
		return a->hash < b->hash ? -1 : 1;

	/* Hashes group candidates; signed source bytes decide equality */
	for (;;) {
		const struct patch_row *x = next_change(a, &at);
		const struct patch_row *y = next_change(b, &bt);
		int order;

		if (!x || !y)
			return !!x - !!y;

		if (x->sign != y->sign)
			return x->sign < y->sign ? -1 : 1;

		order = memcmp(x->text.ptr, y->text.ptr,
			       MIN(x->text.len, y->text.len));
		if (order)
			return order;

		if (x->text.len != y->text.len)
			return x->text.len < y->text.len ? -1 : 1;
	}
}

static int change_match_order(const void *a, const void *b)
{
	const struct change_match *x = a, *y = b;
	int order = changed_rows_compare(x, y);

	if (order)
		return order;

	order = strcmp(x->basename, y->basename);
	if (order)
		return order;

	if (x->leg != y->leg)
		return x->leg - y->leg;

	if (x->file->record->pos != y->file->record->pos)
		return x->file->record->pos < y->file->record->pos ? -1 : 1;

	return (x->first > y->first) - (x->first < y->first);
}

static bool change_signature(struct change_match *match,
			     enum change_extent extent)
{
	const struct patch_row *row;
	size_t at = match->first;
	bool changed = false, meaningful = false;

	match->hash = FNV_OFFSET_BASIS;

	/* Signs and row lengths keep different edit sequences distinct */
	while ((row = next_change(match, &at))) {
		match->hash = (match->hash ^ row->sign) * FNV_PRIME;
		for (size_t j = 0; j < row->text.len; j++) {
			match->hash =
				(match->hash ^ row->text.ptr[j]) * FNV_PRIME;
			meaningful |= isalnum(row->text.ptr[j]);
		}
		match->hash = (match->hash ^ row->text.len) * FNV_PRIME;
		changed = true;
	}

	/* Punctuation alone doesn't identify a fragment moved across files */
	return changed && (extent == WHOLE_HUNK || meaningful);
}

static void match_move(struct change_match *sides[2])
{
	struct patch_move *moves[2];

	/* Reciprocal indices require both records to have been reserved */
	for (int leg = 0; leg < 2; leg++) {
		struct patch_file *file = sides[leg]->file;

		moves[leg] = &file->moves[file->nmoves++];
	}

	for (int leg = 0; leg < 2; leg++) {
		const struct patch_file *partner = sides[!leg]->file;
		struct change_match *match = sides[leg];

		*moves[leg] =
			(typeof(*moves[0])){ .partner_file = partner,
					     .first = match->first,
					     .count = match->count,
					     .partner_move = moves[!leg] -
							     partner->moves };
		for (size_t i = match->first; i < match->first + match->count;
		     i++)
			match->file->rows[i].move = moves[leg];
	}
}

static const struct udiff_line *change_context(const struct change_match *match,
					       int direction)
{
	const struct patch_row *rows = match->file->rows;
	size_t at = direction < 0 ? match->first :
				    match->first + match->count - 1;
	size_t hunk = rows[at].hunk;

	for (at += direction; at < match->file->nrows; at += direction) {
		const struct patch_row *row = &rows[at];
		bool blank = true;

		if (row->hunk != hunk || row->sign != ' ' || row->neutral)
			return NULL;
		for (size_t i = 0; i < row->text.len; i++)
			blank &= !!isspace(row->text.ptr[i]);
		if (!blank)
			return &row->text;
	}
	return NULL;
}

static bool matching_context(const struct change_match *a,
			     const struct change_match *b)
{
	for (int direction = -1; direction <= 1; direction += 2) {
		const struct udiff_line *lines[2] = {
			change_context(a, direction),
			change_context(b, direction)
		};
		size_t length[2];
		bool meaningful = false;

		if (!lines[0] || !lines[1])
			return false;

		/* Keep call names as context when their arguments differ */
		for (int leg = 0; leg < 2; leg++) {
			const char *paren =
				memchr(lines[leg]->ptr, '(', lines[leg]->len);

			length[leg] =
				paren ? (size_t)(paren - lines[leg]->ptr) :
					lines[leg]->len;
		}
		if (length[0] != length[1] ||
		    memcmp(lines[0]->ptr, lines[1]->ptr, length[0]))
			return false;

		for (size_t i = 0; i < length[0]; i++)
			meaningful |= isalnum(lines[0]->ptr[i]);
		if (!meaningful)
			return false;
	}
	return true;
}

/*
 * After same-basename occurrences are accounted for, an exact changed-row
 * signature must occur once on each side to pair across different filenames.
 * The marks preserve input records and coordinates; no document is rewritten.
 */
static void pair_changes(struct change_match *matches, size_t n,
			 enum change_extent extent)
{
	qsort(matches, n, sizeof(*matches), change_match_order);
	for (size_t first = 0; first < n;) {
		size_t residue[2] = { SIZE_MAX, SIZE_MAX };
		size_t count[2] = {};
		size_t end;

		for (end = first + 1;
		     end < n &&
		     !changed_rows_compare(&matches[first], &matches[end]);
		     end++)
			;

		/* Same-basename occurrences belong to ordinary file pairing */
		for (size_t i = first; i < end;) {
			size_t next, mid, paired;

			for (next = i + 1;
			     next < end && !strcmp(matches[i].basename,
						   matches[next].basename);
			     next++)
				;
			for (mid = i; mid < next && !matches[mid].leg; mid++)
				;
			paired = MIN(mid - i, next - mid);
			for (size_t j = 0; j < paired; j++) {
				matches[i + j].consumed =
					matches[mid + j].consumed = true;
			}
			i = next;
		}
		for (size_t i = first; i < end; i++) {
			if (matches[i].consumed)
				continue;

			residue[matches[i].leg] = i;
			count[matches[i].leg]++;
		}
		if (count[0] == 1 && count[1] == 1) {
			struct change_match *sides[2] = {
				&matches[residue[0]], &matches[residue[1]]
			};

			if (extent == WHOLE_HUNK ||
			    matching_context(sides[0], sides[1]))
				match_move(sides);
		}
		first = end;
	}
}

/* A run advances one source view while its insertion point stays fixed */
static size_t change_run_end(const struct patch_file *file, size_t first)
{
	const struct patch_row *row = &file->rows[first];
	int stage = row->sign == '+';
	size_t end;

	for (end = first + 1; end < file->nrows; end++) {
		const struct patch_row *next = &file->rows[end];

		if (next->neutral || next->move || next->sign != row->sign ||
		    next->pos[stage] != row->pos[stage] + end - first ||
		    next->pos[!stage] != row->pos[!stage])
			break;
	}
	return end;
}

static size_t collect_changes(struct patch_file *file, int leg,
			      enum change_extent extent,
			      struct change_match *matches)
{
	const char *path = patch_file_path(file);
	const char *base = strrchr(path, '/');
	size_t n = 0, count = extent == EDIT_RUN ? file->nrows : file->nhunks;

	for (size_t i = 0; i < count; i++) {
		struct change_match match = { .file = file,
					      .basename = base ? base + 1 :
								 path,
					      .leg = leg };

		if (extent == EDIT_RUN) {
			const struct patch_row *row = &file->rows[i];
			size_t end;

			if (row->neutral || row->move ||
			    (row->sign != '+' && row->sign != '-'))
				continue;

			end = change_run_end(file, i);
			match.first = i;
			match.count = end - i;
			i = end - 1;
		} else {
			match.first = file->hunks[i].first;
			match.count = file->hunks[i].count;
		}
		if (change_signature(&match, extent))
			matches[n++] = match;
	}
	return n;
}

void patch_match_moved(struct patch_document docs[2])
{
	struct change_match *matches __free(free) = NULL;
	size_t total = 0;

	for (int leg = 0; leg < 2; leg++) {
		for (size_t i = 0; i < docs[leg].nfiles; i++) {
			struct patch_file *file = &docs[leg].files[i];

			/*
			 * Every move claims a row. Reserve that bound once,
			 * since rows keep pointers into this array after
			 * pairing.
			 */
			file->moves = xmalloc_array(file->nrows,
						    sizeof(*file->moves));
			total += file->nrows + file->nhunks;
		}
	}
	matches = xmalloc_array(total, sizeof(*matches));

	/* Whole hunks claim their rows before smaller runs can use them */
	for (enum change_extent extent = WHOLE_HUNK; extent <= EDIT_RUN;
	     extent++) {
		size_t n = 0;

		for (int leg = 0; leg < 2; leg++) {
			for (size_t i = 0; i < docs[leg].nfiles; i++) {
				n += collect_changes(&docs[leg].files[i], leg,
						     extent, matches + n);
			}
		}
		pair_changes(matches, n, extent);
	}
}

const struct patch_file *patch_row_partner(const struct patch_source *source,
					   size_t row)
{
	const struct patch_move *move = source->rows[row].move;

	return move ? move->partner_file : NULL;
}

static void append_source_row(struct patch_source *source,
			      const struct patch_row *row)
{
	if (source->nrows == 2 * LINE_CEILING)
		die("too many source lines in a comparison");

	if (source->nrows == source->capacity) {
		source->capacity = source->capacity ? source->capacity * 2 :
						      128;
		source->rows = xrealloc_array(source->rows, source->capacity,
					      sizeof(*source->rows));
	}
	source->rows[source->nrows++] = *row;
}

static void append_source_gap(struct patch_source *source, const size_t *pos,
			      int leg)
{
	/* Neither valid source nor the other operand can match this sentinel */
	static const char gaps[2][11] = { "\0unknown0\n", "\0unknown1\n" };
	struct patch_row row = { .text = { .ptr = gaps[leg],
					   .len = sizeof(gaps[0]) - 1 },
				 .pos = { pos[0], pos[1] },
				 .hunk = SIZE_MAX,
				 .sign = '?' };

	append_source_row(source, &row);
}

static void blob_lines(const struct iomem_buf *blob, struct source_view *view)
{
	size_t n = 0;

	for (size_t pos = 0; pos < blob->len; n++) {
		const char *lf =
			memchr(blob->base + pos, '\n', blob->len - pos);

		pos = lf ? (size_t)(lf - blob->base) + 1 : blob->len;
	}
	if (n > LINE_CEILING)
		die("too many lines in a source file");

	view->lines = xmalloc_array(n, sizeof(*view->lines));
	for (size_t pos = 0; pos < blob->len;) {
		const char *lf =
			memchr(blob->base + pos, '\n', blob->len - pos);
		size_t end = lf ? (size_t)(lf - blob->base) + 1 : blob->len;

		view->lines[view->count++] =
			(typeof(view->lines[0])){ .ptr = blob->base + pos,
						  .len = end - pos };
		pos = end;
	}
}

static void source_view_free(struct source_view *view)
{
	free(view->lines);
	free(view->rows);
	free(view->index);
	*view = (typeof(*view)){};
}

static bool view_at_eof(const struct source_view *view)
{
	const struct udiff_line *last;

	if (!view->count)
		return false;

	last = &view->lines[view->count - 1];
	return last->len && last->ptr[last->len - 1] != '\n';
}

static void source_views(struct patch_source *source)
{
	for (int s = 0; s < 2; s++) {
		struct source_view *view = &source->view[s];
		char absent_sign = s ? '-' : '+';

		view->lines =
			xmalloc_array(source->nrows, sizeof(*view->lines));
		view->rows = xmalloc_array(source->nrows, sizeof(*view->rows));
		view->index =
			xmalloc_array(source->nrows, sizeof(*view->index));

		/*
		 * Each stage omits the opposite edit sign. The view stores
		 * indices into source->rows; index maps those source rows back
		 * into the view, leaving SIZE_MAX for rows excluded from it.
		 */
		for (size_t i = 0; i < source->nrows; i++) {
			view->index[i] = SIZE_MAX;
			if (source->rows[i].sign == absent_sign)
				continue;

			/* EOF leaves no unknown tail for a gap to represent */
			if (source->rows[i].sign == '?' &&
			    ((source->file && source->file->empty[s]) ||
			     view_at_eof(view)))
				continue;

			if (view_at_eof(view))
				source->ambiguous = true;
			if (!source->rows[i].text.len)
				source->ambiguous = true;
			view->index[i] = view->count;
			view->lines[view->count] = source->rows[i].text;
			view->rows[view->count++] = i;
		}
	}
}

static void source_unchanged(struct patch_source *source,
			     const struct source_view *blobs, size_t *pos,
			     const size_t *end)
{
	if (pos[0] > end[0] || pos[1] > end[1] ||
	    end[0] - pos[0] != end[1] - pos[1])
		die("derived patch has inconsistent source coordinates");

	for (; pos[0] < end[0]; pos[0]++, pos[1]++) {
		struct patch_row row = { .text = blobs[0].lines[pos[0]],
					 .pos = { pos[0], pos[1] },
					 .hunk = SIZE_MAX,
					 .sign = ' ' };

		if (!patch_row_equal(&row.text, &blobs[1].lines[pos[1]]))
			die("derived patch omits a source change");

		append_source_row(source, &row);
	}
}

static void append_hunk(struct patch_source *source,
			const struct patch_file *file,
			const struct patch_hunk *hunk)
{
	/* First removal in the current exact self-replacement */
	size_t neutral_first = hunk->first;

	for (size_t i = hunk->first; i < hunk->first + hunk->count; i++) {
		struct patch_row row = file->rows[i];

		if (!row.neutral || row.sign == '+')
			neutral_first = i + 1;
		if (row.neutral) {
			if (row.sign == '+')
				continue;

			/*
			 * An exact self-replacement contributes one retained
			 * row to each image. Removed rows share the insertion
			 * point, so restore each row's result position within
			 * the run.
			 */
			row.pos[1] += i - neutral_first;
			row.sign = ' ';
		}
		append_source_row(source, &row);
	}
}

/*
 * Validation counted these rows, and patch_source_read checked their extents
 * against both Git images. A mismatch here is an inconsistent derived patch.
 */
static void check_quotation(const struct patch_file *file,
			    const struct patch_hunk *hunk,
			    const struct source_view *blobs)
{
	for (size_t i = hunk->first; i < hunk->first + hunk->count; i++) {
		const struct patch_row *row = &file->rows[i];

		for (int stage = 0; stage < 2; stage++) {
			char absent_sign = stage ? '-' : '+';
			const struct udiff_line *actual;

			if (row->sign == absent_sign)
				continue;

			actual = &blobs[stage].lines[row->pos[stage]];
			if (!patch_row_equal(&row->text, actual))
				die("derived patch disagrees with the source file");
		}
	}
}

void patch_source_read(struct patch_source *source,
		       const struct patch_file *file,
		       const struct patch_file *other, int leg, bool complete)
{
	struct source_view blobs[2] = {};
	size_t pos[2] = {}, end[2] = {};

	source->file = file;
	source->complete = complete;

	/* The other operand supplies the path when this patch omits the file */
	if (source->complete) {
		for (int s = 0; s < 2; s++) {
			const struct patch_file *named = file ? file : other;

			if (!file || !file->empty[s])
				gittree_source_bytes(leg,
						     patch_file_path(named), s,
						     &source->blobs[s]);
			blob_lines(&source->blobs[s], &blobs[s]);
			end[s] = blobs[s].count;
		}
	}
	if (file) {
		for (size_t h = 0; h < file->nhunks; h++) {
			const struct patch_hunk *hunk = &file->hunks[h];

			/*
			 * Tree objects supply every row between hunks. Verify
			 * the quoted rows too, so complete images and native
			 * edit coordinates cannot silently disagree.
			 */
			if (source->complete) {
				for (int s = 0; s < 2; s++) {
					if (hunk->pos[s] + hunk->lines[s] >
					    end[s])
						die("derived patch quotes beyond the source file");
				}
				source_unchanged(source, blobs, pos, hunk->pos);
				check_quotation(file, hunk, blobs);
			} else if (pos[0] != hunk->pos[0] ||
				   pos[1] != hunk->pos[1]) {
				/* A gap's source bytes remain unknown */
				source->ambiguous |= pos[0] > hunk->pos[0] ||
						     pos[1] > hunk->pos[1];
				append_source_gap(source, pos, leg);
			}
			append_hunk(source, file, hunk);
			for (int s = 0; s < 2; s++)
				pos[s] = hunk->pos[s] + hunk->lines[s];
		}
	}
	if (source->complete)
		source_unchanged(source, blobs, pos, end);
	else if (!file || !file->empty[0] || !file->empty[1])
		append_source_gap(source, pos, leg);
	source_view_free(&blobs[0]);
	source_view_free(&blobs[1]);
	source_views(source);
}

void patch_source_free(struct patch_source *source)
{
	for (int s = 0; s < 2; s++) {
		source_view_free(&source->view[s]);
		iomem_buf_free(&source->blobs[s]);
	}
	free(source->rows);
	*source = (typeof(*source)){};
}
