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
