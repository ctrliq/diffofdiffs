// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ctrl IQ, Inc.
 *
 * Lay out the engine's selected rows without changing their correspondence.
 * Numbers belong only to source rows. Wrapping and alignment add no source.
 */
#define _GNU_SOURCE

#include <langinfo.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <unistd.h>

#include <cli.h>
#include <display.h>
#include <highlight.h>
#include <render.h>
#include <review.h>
#include <util.h>

#define ANSI_RESET "\033[0m"
#define ANSI_BOLD "\033[1m"
#define ANSI_UNDERLINE "\033[4m"
#define ANSI_UNDERLINE_OFF "\033[24m"
#define ANSI_RED "\033[31m"
#define ANSI_GREEN "\033[32m"
#define ANSI_CYAN "\033[36m"
#define ANSI_BRIGHT_RED "\033[91m"
#define ANSI_BRIGHT_GREEN "\033[92m"

DEFINE_FREE(text_highlight, struct text_highlight, text_highlight_free(&_T))

enum glyph_role {
	GUTTER,
	MIDDLE,
	GROUP_RULE,
	GROUP_CROSS,
	GUTTER_CROSS,
	FILE_RULE,
	BANNER_RULE
};

/* clang-format off */
static const char *const unicode_glyphs[] = {
	[GUTTER] = "\u2502",
	[MIDDLE] = "\u2503",
	[GROUP_RULE] = "\u2500",
	[GROUP_CROSS] = "\u2542",
	[GUTTER_CROSS] = "\u253c",
	[FILE_RULE] = "\u2501",
	[BANNER_RULE] = "\u2550"
};

static const char *const ascii_glyphs[] = {
	[GUTTER] = "|",
	[MIDDLE] = "|",
	[GROUP_RULE] = "-",
	[GROUP_CROSS] = "+",
	[GUTTER_CROSS] = "+",
	[FILE_RULE] = "-",
	[BANNER_RULE] = "="
};
/* clang-format on */

struct layout {
	const char *const *glyphs;
	size_t source;
	size_t digits;
	bool colored;
	bool tabs;
	bool tty;
};

enum line_kind {
	SOURCE_LINE,
	DELTA_HEADER,
	CONTEXT_HEADER,
	LABEL_LINE,
	FUNCTION_LINE,
	NOTE_LINE,
	NEWLINE_NOTE
};

static size_t measure_line_fragment(const char *text, size_t len, size_t start,
				    size_t limit, unsigned int tabstop,
				    size_t *columns)
{
	size_t count;

	*columns = 0;

	/*
	 * Escaped display text has no undecodable or nonprinting bytes. A tab
	 * or multibyte character must fit without splitting its bytes. Raw tabs
	 * use the absolute terminal origin; TTY measurement starts at zero and
	 * rendering expands tabs before wrapping.
	 */
	for (count = 0; count < len && text[count] != '\n';) {
		int width = 0;
		size_t n = 1;

		if (text[count] == '\t')
			width = tabstop - (start + *columns) % tabstop;
		else
			n = display_character_bytes(text + count, len - count,
						    &width);

		if (*columns + width > limit)
			break;

		*columns += width;
		count += n;
	}
	return count;
}

static size_t gutter_width(const struct layout *layout)
{
	return 2 * layout->digits + 3;
}

static size_t source_column(const struct layout *layout, int leg)
{
	return gutter_width(layout) +
	       leg * (gutter_width(layout) + layout->source + 1);
}

static size_t report_width(const struct layout *layout)
{
	return 2 * (gutter_width(layout) + layout->source) + 1;
}

static const char *line_ansi_style(const struct layout *layout,
				   enum line_kind kind,
				   const struct review_row *row)
{
	if (!layout->colored)
		return "";

	if (kind == LABEL_LINE || kind == DELTA_HEADER ||
	    kind == CONTEXT_HEADER || kind == NOTE_LINE)
		return ANSI_BOLD;

	if (kind == FUNCTION_LINE)
		return ANSI_CYAN;

	if (!row || row->shared)
		return "";

	if (row->sign == '+')
		return ANSI_GREEN;

	if (row->sign == '-')
		return ANSI_RED;

	return "";
}

static void render_line_gutter(FILE *out, const struct layout *layout,
			       const struct review_row *row,
			       enum line_kind kind, bool first)
{
	/* 32 bytes cover 20 decimal digits and a NUL for a 64-bit size_t */
	char old[32] = "", new[32] = "";
	char sign = ' ';

	/* Wrapped fragments don't introduce another source line or edit */
	if (first) {
		if (kind == DELTA_HEADER) {
			strcpy(old, "old");
			strcpy(new, "new");
		} else if (kind == CONTEXT_HEADER) {
			strcpy(new, "new");
		} else if (kind == NEWLINE_NOTE) {
			sign = '\\';
		} else if (row && row->text) {
			if (!row->shared)
				sign = row->sign;
			if (row->pos[0] != SIZE_MAX)
				snprintf(old, sizeof(old), "%zu",
					 row->pos[0] + 1);
			if (row->pos[1] != SIZE_MAX)
				snprintf(new, sizeof(new), "%zu",
					 row->pos[1] + 1);
		}
	}
	fprintf(out, "%c%*s %*s%s", sign, (int)layout->digits, old,
		(int)layout->digits, new, layout->glyphs[GUTTER]);
}

static void write_spaces(FILE *out, size_t count)
{
	for (size_t i = 0; i < count; i++)
		fputc(' ', out);
}

static bool horizontal_whitespace(char c)
{
	return c == ' ' || c == '\t';
}

static void render_underlined_spaces(FILE *out, const char *text, size_t start,
				     size_t end)
{
	/* Tabs move the cursor without painting cells; retain their bytes */
	for (size_t at = start, next; at < end; at = next) {
		bool space = text[at] == ' ';

		for (next = at + 1; next < end && (text[next] == ' ') == space;
		     next++)
			;
		if (space)
			fputs(ANSI_UNDERLINE, out);
		fwrite(text + at, 1, next - at, out);
		if (space)
			fputs(ANSI_UNDERLINE_OFF, out);
	}
}

static void render_ansi_span(FILE *out, const char *text,
			     const struct text_span *span,
			     const struct text_span *clip, const char *style,
			     const char *restore_style)
{
	/*
	 * A whitespace run needs emphasis only beside unchanged whitespace.
	 * Check the complete span so wrapping cannot create or hide a neighbor.
	 */
	for (size_t at = span->start, next; at < span->end; at = next) {
		bool space = horizontal_whitespace(text[at]);
		size_t from, to;
		bool adjacent;

		for (next = at + 1; next < span->end &&
				    horizontal_whitespace(text[next]) == space;
		     next++)
			;
		from = MAX(at, clip->start);
		to = MIN(next, clip->end);
		if (from >= to)
			continue;

		adjacent = (at == span->start && at &&
			    horizontal_whitespace(text[at - 1])) ||
			   (next == span->end &&
			    horizontal_whitespace(text[next]));
		if (space && adjacent) {
			fputs(ANSI_CYAN, out);
			render_underlined_spaces(out, text, from, to);
		} else {
			fputs(space ? restore_style : style, out);
			fwrite(text + from, 1, to - from, out);
		}
		fputs(ANSI_RESET, out);
		fputs(restore_style, out);
	}
}

static void render_highlighted_fragment(FILE *out, const char *text,
					size_t start, size_t end,
					const struct text_highlight *h, int leg,
					const char *restore_style)
{
	const struct highlight_ranges *ranges =
		highlight_mode == HIGHLIGHT_WORDS ? &h->words[leg] :
						    &h->characters[leg];
	const char *word = leg ? ANSI_BRIGHT_GREEN : ANSI_BRIGHT_RED;
	size_t at = start;

	/* Indentation and body ranges use the displayed byte offsets */
	if (h->indentation_changed && at < h->indent[leg]) {
		struct text_span span = { h->common_indent, h->indent[leg] };
		struct text_span clip = { MAX(at, span.start),
					  MIN(end, span.end) };

		if (clip.start < clip.end) {
			fwrite(text + at, 1, clip.start - at, out);
			render_ansi_span(out, text, &span, &clip, ANSI_CYAN,
					 restore_style);
			at = clip.end;
		}
	}

	/* Ranges keep whole-line offsets even when a span crosses a wrap */
	for (size_t i = 0; i < ranges->count; i++) {
		const struct text_span *span = &ranges->spans[i];
		struct text_span clip = { MAX(at, span->start),
					  MIN(end, span->end) };

		if (clip.start >= clip.end)
			continue;

		fwrite(text + at, 1, clip.start - at, out);
		render_ansi_span(out, text, span, &clip, word, restore_style);
		at = clip.end;
	}
	fwrite(text + at, 1, end - at, out);
}

struct text_column {
	const struct review_row *row;
	const char *text;
	size_t len;
	size_t at;
	bool present;
};

/*
 * Print one physical line of this column and advance its cursor. Metadata
 * newlines end a fragment without printing inside the cell; the caller ends the
 * paired line. True means this column still has text to wrap.
 */
static bool render_text_column(FILE *out, const struct layout *layout,
			       struct text_column *column, enum line_kind kind,
			       const struct text_highlight *highlight, int leg,
			       bool first)
{
	bool active = first || column->at < column->len;
	size_t columns = 0, count, printed, padding = 0;
	size_t start = source_column(layout, leg);
	size_t width = layout->source;
	const char *code;

	if (kind == LABEL_LINE) {
		start -= gutter_width(layout);
		width += gutter_width(layout);
	}
	code = active ? line_ansi_style(layout, kind, column->row) : "";
	count = measure_line_fragment(column->text + column->at,
				      column->len - column->at, start, width,
				      TAB_WIDTH, &columns);
	printed = count;

	/* Trailing source whitespace belongs to the patch */
	if (kind != SOURCE_LINE) {
		for (; printed && column->text[column->at + printed - 1] == ' ';
		     printed--)
			;
		columns -= count - printed;
	}
	if (kind == LABEL_LINE && printed) {
		padding = (width - columns) / 2;
		write_spaces(out, padding);
	}
	fputs(code, out);
	if (kind != LABEL_LINE)
		render_line_gutter(out, layout, column->row, kind,
				   first && column->present);
	if (highlight)
		render_highlighted_fragment(out, column->text, column->at,
					    column->at + printed, highlight,
					    leg, code);
	else
		fwrite(column->text + column->at, 1, printed, out);
	if (*code)
		fputs(ANSI_RESET, out);
	column->at += count;
	if (column->at < column->len && column->text[column->at] == '\n')
		column->at++;

	/* Never pad after the right side's source */
	if (!leg) {
		write_spaces(out, width - columns - padding);
		fputs(layout->glyphs[MIDDLE], out);
	}
	return column->at < column->len;
}

static void expand_source_column(char **text, struct text_highlight *highlight,
				 int leg, size_t common_indent)
{
	size_t *offsets __free(free) = NULL;
	char *expanded;

	if (!strchr(*text, '\t'))
		return;

	if (highlight)
		offsets = xmalloc_array(strlen(*text) + 1, sizeof(*offsets));
	expanded = expand_source_tabs(*text, tab_width, offsets);

	/*
	 * Match original tab bytes before expansion. Mapping character
	 * boundaries keeps indentation and body ranges on the same source
	 * characters, including each entire tab. Expansion preserves whitespace
	 * adjacency.
	 */
	if (highlight) {
		struct highlight_ranges *ranges[] = {
			&highlight->words[leg], &highlight->characters[leg]
		};

		for (size_t i = 0; i < ARRAY_SIZE(ranges); i++) {
			for (size_t j = 0; j < ranges[i]->count; j++) {
				struct text_span *span = &ranges[i]->spans[j];

				span->start = offsets[span->start];
				span->end = offsets[span->end];
			}
		}
		highlight->indent[leg] = offsets[highlight->indent[leg]];
		highlight->common_indent = offsets[common_indent];
	}
	free(*text);
	*text = expanded;
}

static void render_text_pair(FILE *out, const struct layout *layout,
			     const struct iomem_slice text[2],
			     const struct review_row *const rows[2],
			     enum line_kind kind)
{
	struct text_highlight highlight __free(text_highlight) = {};
	char *(*format_text)(const char *, size_t) =
		kind == SOURCE_LINE ? escape_source_text : expand_display_text;
	char *right __free(free) = format_text(text[1].base, text[1].len);
	char *left __free(free) = format_text(text[0].base, text[0].len);
	const struct text_highlight *selected = NULL;
	const char *displayed[2] = { left, right };
	struct text_column columns[2] = {};
	bool first = true, more;

	/* Wrapping must not change the ranges selected for the complete line */
	if (layout->colored && kind == SOURCE_LINE &&
	    highlight_mode != HIGHLIGHT_NONE) {
		struct iomem_slice source[2] = {};

		for (int leg = 0; leg < 2; leg++) {
			if (rows[leg])
				source[leg] =
					(typeof(source[0])){ rows[leg]->text,
							     rows[leg]->len };
		}
		highlight_pair(source, displayed, &highlight);
		if (highlight.indentation_changed || highlight.words[0].count ||
		    highlight.words[1].count || highlight.characters[0].count ||
		    highlight.characters[1].count)
			selected = &highlight;
	}
	if (layout->tty && kind == SOURCE_LINE) {
		size_t common_indent = highlight.common_indent;
		struct text_highlight *h = selected ? &highlight : NULL;

		expand_source_column(&left, h, 0, common_indent);
		expand_source_column(&right, h, 1, common_indent);
		displayed[0] = left;
		displayed[1] = right;
	}
	for (int leg = 0; leg < 2; leg++) {
		const struct review_row *row = rows ? rows[leg] : NULL;

		columns[leg] =
			(typeof(columns[0])){ .text = displayed[leg],
					      .len = strlen(displayed[leg]),
					      .row = row,
					      .present = text[leg].base ||
							 row };
	}

	/*
	 * Layout reserves room for a wide character, or a full tab when needed,
	 * so each active column consumes input on every pass. A finished column
	 * stays empty while its counterpart wraps onto further physical lines.
	 */
	do {
		more = false;
		for (int leg = 0; leg < 2; leg++) {
			more |= render_text_column(out, layout, &columns[leg],
						   kind, selected, leg, first);
		}
		fputc('\n', out);
		first = false;
	} while (more);
}

static void render_label_pair(FILE *out, const struct layout *layout,
			      const char *const text[2], enum line_kind kind)
{
	struct iomem_slice pair[2] = {};

	if (!text[0] && !text[1])
		return;

	for (int leg = 0; leg < 2; leg++) {
		pair[leg] = (typeof(pair[0])){
			.base = text[leg],
			.len = text[leg] ? strlen(text[leg]) : 0
		};
	}
	render_text_pair(out, layout, pair, NULL, kind);
}

static void render_source_pair(FILE *out, const struct layout *layout,
			       const struct review_pair *pair)
{
	const struct review_row *rows[2] = { &pair->side[0], &pair->side[1] };
	struct iomem_slice text[2] = {};
	const char *notes[2] = {};

	for (int leg = 0; leg < 2; leg++) {
		const struct review_row *row = rows[leg];
		size_t len = row->len;

		if (!row->text) {
			rows[leg] = NULL;
			continue;
		}

		if (len && row->text[len - 1] == '\n')
			len--;
		else
			notes[leg] = "No newline at end of file";
		text[leg] = (typeof(text[0])){ .base = row->text, .len = len };
	}
	render_text_pair(out, layout, text, rows, SOURCE_LINE);
	render_label_pair(out, layout, notes, NEWLINE_NOTE);
}

bool render_section_has_output(const struct review_file *file, int section)
{
	return file->section[section].nrows ||
	       (!section && file->metadata_diff);
}

static void measure_source_row(const struct review_row *row, int leg,
			       struct layout *layout)
{
	char *text __free(free) = NULL;
	size_t columns;

	if (!row->text)
		return;

	text = escape_source_text(row->text, row->len);
	measure_line_fragment(text, strlen(text),
			      layout->tty ? 0 : source_column(layout, leg),
			      SIZE_MAX, layout->tty ? tab_width : TAB_WIDTH,
			      &columns);
	layout->source = MAX(layout->source, columns);
	layout->tabs |= memchr(row->text, '\t', row->len) != NULL;
	for (int stage = 0; stage < 2; stage++) {
		size_t digits = 1, n = row->pos[stage];

		if (n == SIZE_MAX)
			continue;

		for (n++; n >= 10; n /= 10)
			digits++;
		layout->digits = MAX(layout->digits, digits);
	}
}

static void measure_source_section(const struct review_section *section,
				   struct layout *layout)
{
	for (size_t i = 0; i < section->nrows; i++) {
		for (int leg = 0; leg < 2; leg++) {
			measure_source_row(&section->rows[i].side[leg], leg,
					   layout);
		}
	}
}

static void init_report_layout(FILE *out, const struct review_report *report,
			       struct layout *layout)
{
	size_t limit = max_column_width ?: 100;
	struct winsize terminal = {};
	size_t width, digits;
	int fd = fileno(out);

	*layout = (typeof(*layout)){ .source = 2,
				     .digits = 3,
				     .tty = isatty(fd) };

	/*
	 * Widening a pane moves the right source origin between terminal tab
	 * stops. Repeat until the number fields and source width stop growing.
	 */
	do {
		width = layout->source;
		digits = layout->digits;
		for (size_t f = 0; f < report->count; f++) {
			const struct review_file *file = &report->files[f];

			if (!render_section_has_output(file, 0) &&
			    !render_section_has_output(file, 1))
				continue;

			for (int s = 0; s < 2; s++) {
				measure_source_section(&file->section[s],
						       layout);
			}
		}
		if (layout->tabs && !layout->tty)
			layout->source = MAX(layout->source, TAB_WIDTH);
	} while (layout->tabs && !layout->tty &&
		 (layout->source != width || layout->digits != digits));
	if (layout->tty && !ioctl(fd, TIOCGWINSZ, &terminal) &&
	    terminal.ws_col) {
		size_t overhead = 2 * gutter_width(layout) + 1;
		size_t available = terminal.ws_col > overhead ?
					   (terminal.ws_col - overhead) / 2 :
					   0;

		/* Two columns leave room for any printable wide character */
		available = MAX(2, available);
		limit = max_column_width ? MIN(max_column_width, available) :
					   available;
	}

	/* Leave room for a whole tab when wrapping */
	if (layout->tabs && !layout->tty)
		limit = MAX(limit, TAB_WIDTH);
	layout->source = MIN(layout->source, limit);
	layout->colored = color_when == COLOR_WHEN_ALWAYS ||
			  (color_when == COLOR_WHEN_AUTO && layout->tty);
	layout->glyphs = strcmp(nl_langinfo(CODESET), "UTF-8") ? ascii_glyphs :
								 unicode_glyphs;
}

static void write_repeated_glyph(FILE *out, const char *glyph, size_t count)
{
	for (size_t i = 0; i < count; i++)
		fputs(glyph, out);
}

static void render_block_separator(FILE *out, const struct layout *layout)
{
	size_t numbers = gutter_width(layout) - 1;

	write_spaces(out, numbers);
	fputs(layout->glyphs[GUTTER], out);
	write_repeated_glyph(out, layout->glyphs[GROUP_RULE], layout->source);
	fputs(layout->glyphs[GROUP_CROSS], out);
	write_repeated_glyph(out, layout->glyphs[GROUP_RULE], numbers);
	fputs(layout->glyphs[GUTTER_CROSS], out);
	write_repeated_glyph(out, layout->glyphs[GROUP_RULE], layout->source);
	fputc('\n', out);
}

static void render_section_banner(FILE *out, const struct layout *layout,
				  const char *title)
{
	size_t width = report_width(layout), len = strlen(title), indent;

	if (len + 6 > width)
		len = strstr(title, " - ") - title;
	indent = MIN(4, width - len - 2);
	for (int line = 0; line < 3; line++) {
		if (layout->colored)
			fputs(ANSI_BOLD, out);
		if (line == 1) {
			fputc('*', out);
			write_spaces(out, indent);
			fwrite(title, 1, len, out);
			write_spaces(out, width - len - indent - 2);
			fputc('*', out);
		} else {
			write_repeated_glyph(out, layout->glyphs[BANNER_RULE],
					     width);
		}
		if (layout->colored)
			fputs(ANSI_RESET, out);
		fputc('\n', out);
	}
}

static void render_wrapped_paragraph(FILE *out, const struct layout *layout,
				     const char *text)
{
	size_t width = report_width(layout);
	size_t len = strlen(text);

	for (size_t at = 0; at < len;) {
		size_t columns,
			n = measure_line_fragment(text + at, len - at, 0, width,
						  TAB_WIDTH, &columns);
		size_t printed;

		/* Backtrack only when the width limit falls inside a word */
		if (n < len - at && text[at + n] != '\n' &&
		    text[at + n] != ' ') {
			size_t word = n;

			for (; word && text[at + word - 1] != ' '; word--)
				;
			if (word)
				n = word;
		}
		printed = n;
		for (; printed && text[at + printed - 1] == ' '; printed--)
			;
		fwrite(text + at, 1, printed, out);
		fputc('\n', out);
		at += n;
		for (; at < len && (text[at] == ' ' || text[at] == '\n'); at++)
			;
	}
}

static char *format_function_label(const char *name)
{
	char *label;

	xasprintf(&label, "%s%s", name && *name ? "@@ " : "@@", name ?: "");
	return label;
}

static char *format_operand_label(const char *name, int leg)
{
	const char *base = name ? strrchr(name, '/') : NULL;
	char *label;
	size_t n;

	if (base && !git_tree_dir)
		name = base + 1;
	n = name ? strlen(name) : 0;
	if (git_tree_dir && n == 40 && strspn(name, "0123456789abcdef") == n)
		n = 12;
	if (backport_labels)
		xasprintf(&label, "%s %s%s%.*s", leg ? "Upstream" : "Backport",
			  git_tree_dir ? "commit" : "patch", name ? ": " : "",
			  (int)n, name ? name : "");
	else
		xasprintf(&label, "%s %d%s%.*s",
			  git_tree_dir ? "Commit" : "Patch", leg + 1,
			  name ? ": " : "", (int)n, name ? name : "");
	return label;
}

static void render_file_section(FILE *out, const struct layout *layout,
				const struct review_file *file,
				int section_index)
{
	const struct review_section *section = &file->section[section_index];

	fputc('\n', out);
	write_repeated_glyph(out, layout->glyphs[FILE_RULE],
			     report_width(layout));
	fputc('\n', out);
	render_label_pair(out, layout, (const char *const *)file->path,
			  section_index ? CONTEXT_HEADER : DELTA_HEADER);
	if (file->ambiguous) {
		const char *note =
			"Overlapping source quotations: edits shown without assuming correspondence.";

		render_label_pair(out, layout,
				  (const char *const[]){ note, note },
				  NOTE_LINE);
	}

	/* File operations belong to delta, even without source rows */
	if (!section_index && file->metadata_diff)
		render_label_pair(out, layout,
				  (const char *const *)file->metadata,
				  NOTE_LINE);
	if (!section_index)
		render_label_pair(out, layout, (const char *const *)file->note,
				  NOTE_LINE);

	/* Preserve the engine's gaps rather than implying adjacent source */
	for (size_t b = 0; b < section->nblocks; b++) {
		const struct review_block *block = &section->blocks[b];
		char *right __free(free) =
			format_function_label(block->name[1]);
		char *left __free(free) = format_function_label(block->name[0]);
		const char *headings[2] = { left, right };

		if (b)
			render_block_separator(out, layout);
		render_label_pair(out, layout, headings, FUNCTION_LINE);
		for (size_t i = block->first; i < block->first + block->count;
		     i++)
			render_source_pair(out, layout, &section->rows[i]);
	}
}

void render_report(FILE *out, const struct review_report *report,
		   const char *const names[2])
{
	char *right __free(free) = format_operand_label(names[1], 1);
	char *left __free(free) = format_operand_label(names[0], 0);
	const char *identities[2] = { left, right };
	bool present[2] = {};

	for (size_t f = 0; f < report->count; f++) {
		for (int s = 0; s < 2; s++)
			present[s] |=
				render_section_has_output(&report->files[f], s);
	}

	/*
	 * Prefer UTF-8 source decoding even in a single-byte locale. If C.UTF-8
	 * is unavailable, both renderers escape undecodable bytes instead.
	 */
	setlocale(LC_CTYPE, "");
	if (MB_CUR_MAX == 1)
		setlocale(LC_CTYPE, "C.UTF-8");
	if (output_format == OUTPUT_HTML) {
		render_html_report(out, report, identities, present);
		return;
	}
	render_text_report(out, report, identities, present, true);
}

void render_text_report(FILE *out, const struct review_report *report,
			const char *const identities[2], const bool present[2],
			bool color)
{
	struct layout layout;

	if (!present[0] && !present[1])
		return;

	/* Keep source and number columns aligned across both sections */
	init_report_layout(out, report, &layout);
	layout.colored &= color;
	write_repeated_glyph(out, layout.glyphs[FILE_RULE],
			     report_width(&layout));
	fputc('\n', out);
	render_label_pair(out, &layout, identities, LABEL_LINE);
	write_repeated_glyph(out, layout.glyphs[FILE_RULE],
			     report_width(&layout));
	fputs("\n\n", out);
	render_wrapped_paragraph(
		out, &layout,
		git_tree_dir ?
			"Tree mode: comparisons use complete files before and after each commit." :
			"Patch mode: comparisons use only the source lines quoted in the patches.");
	fputc('\n', out);
	for (int s = 0; s < 2; s++) {
		if (!present[s])
			continue;

		if (s && present[0])
			fputc('\n', out);
		render_section_banner(
			out, &layout,
			s ? "CONTEXT DIFFERENCES - surrounding code differences between the patches" :
			    "DELTA DIFFERENCES - code changes that differ between the patches");
		render_wrapped_paragraph(
			out, &layout,
			s ? (backport_labels ?
				     "Signs mark lines present in only one result: - Backport, + Upstream." :
				     "Signs mark lines present in only one result: - patch1, + patch2.") :
			    "Signs mark edits made by only that patch. A sign-less line with one number is an edit both patches make: a new number means both add it, an old number means both remove it.");
		for (size_t f = 0; f < report->count; f++) {
			const struct review_file *file = &report->files[f];

			if (!render_section_has_output(file, s))
				continue;

			render_file_section(out, &layout, file, s);
		}
	}
}
