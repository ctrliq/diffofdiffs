// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ctrl IQ, Inc.
 *
 * Emit a self-contained report from the same rows used by the terminal
 * renderer. Source enters text nodes only; the script changes presentation, not
 * row pairing.
 */
#include <string.h>
#include <sys/param.h>

#include <cli.h>
#include <display.h>
#include <gittree.h>
#include <highlight.h>
#include <render.h>
#include <util.h>

#include "../build/html-assets.h"

DEFINE_FREE(text_highlight, struct text_highlight, text_highlight_free(&_T))
DEFINE_FREE(iomem_buf, struct iomem_buf, iomem_buf_free(&_T))

struct html_layout {
	size_t width;
	size_t digits;
	size_t row;
};

/* clang-format off */
static const char *const highlight_names[] = {
	[HIGHLIGHT_WORDS] = "words",
	[HIGHLIGHT_CHARACTERS] = "characters",
	[HIGHLIGHT_NONE] = "none"
};
static const char *const theme_names[] = {
	[THEME_DARK] = "dark",
	[THEME_LIGHT] = "light"
};
/* clang-format on */

static void write_html_text(FILE *out, const char *text, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		switch (text[i]) {
		case '&':
			fputs("&amp;", out);
			break;
		case '<':
			fputs("&lt;", out);
			break;
		case '>':
			fputs("&gt;", out);
			break;
		case '"':
			fputs("&quot;", out);
			break;
		default:
			fputc(text[i], out);
		}
	}
}

static void write_html_label(FILE *out, const char *text)
{
	char *expanded __free(free) =
		expand_display_text(text, text ? strlen(text) : 0);

	write_html_text(out, expanded, strlen(expanded));
}

static void write_highlight_spans(FILE *out, const char *text, size_t start,
				  size_t end, size_t word_start,
				  size_t word_end)
{
	/*
	 * Give each highlighted space its own block to distinguish it from
	 * tabs. The text nodes keep the original bytes; CSS controls the
	 * visible gaps.
	 */
	for (size_t at = start, next; at < end; at = next) {
		bool space = text[at] == ' ', tab = text[at] == '\t';

		for (next = at + 1; next < end && !space && !tab &&
				    text[next] != ' ' && text[next] != '\t';
		     next++)
			;
		if (tab) {
			fputs("<span class=\"whitespace-tab\" title=\"Tab\">",
			      out);
		} else if (space) {
			fputs("<span class=\"whitespace-space\" title=\"Space\">",
			      out);
		} else {
			/*
			 * Character refinement can split a token across spans.
			 * Keep its word highlight connected across them.
			 */
			bool left = at > word_start && text[at - 1] != ' ' &&
				    text[at - 1] != '\t';
			bool right = next < word_end && text[next] != ' ' &&
				     text[next] != '\t';

			fprintf(out, "<span class=\"highlight-text%s%s\">",
				left ? " word-join-left" : "",
				right ? " word-join-right" : "");
		}
		write_html_text(out, text + at, next - at);
		fputs("</span>", out);
	}
}

static void write_character_spans(FILE *out, const char *text, size_t start,
				  size_t end,
				  const struct highlight_ranges *ranges)
{
	size_t at = start;

	for (size_t i = 0; i < ranges->count; i++) {
		const struct text_span *span = &ranges->spans[i];
		size_t from = MAX(at, span->start), to = MIN(end, span->end);

		if (from >= to)
			continue;

		write_highlight_spans(out, text, at, from, start, end);
		fputs("<span class=\"character-change\">", out);
		write_highlight_spans(out, text, from, to, start, end);
		fputs("</span>", out);
		at = to;
	}
	write_highlight_spans(out, text, at, end, start, end);
}

static void write_highlighted_text(FILE *out, const char *text,
				   const struct text_highlight *h, int leg)
{
	const struct highlight_ranges *words = &h->words[leg];
	size_t at = 0;

	if (h->indentation_changed) {
		size_t first = h->common_indent;

		write_html_text(out, text, first);
		fputs("<span class=\"indent-change\" title=\"Leading whitespace differs.\">",
		      out);
		write_highlight_spans(out, text, first, h->indent[leg], first,
				      h->indent[leg]);
		fputs("</span>", out);
		at = h->indent[leg];
	}
	for (size_t i = 0; i < words->count; i++) {
		const struct text_span *span = &words->spans[i];

		write_html_text(out, text + at, span->start - at);
		fputs("<span class=\"word-change\" title=\"Text that differs within this aligned pair.\">",
		      out);
		write_character_spans(out, text, span->start, span->end,
				      &h->characters[leg]);
		fputs("</span>", out);
		at = span->end;
	}
	write_html_text(out, text + at, strlen(text) - at);
}

static void write_source_cell(FILE *out, const struct review_row *row,
			      const char *text, const struct text_highlight *h,
			      int leg, struct html_layout *layout)
{
	const char *kind = "context";

	if (row->shared)
		kind = "shared";
	else if (row->sign == '+')
		kind = "add";
	else if (row->sign == '-')
		kind = "remove";

	fprintf(out, "<div class=\"side source-side %s\" data-side=\"%d\"",
		row->text ? kind : "gap", leg);
	if (!row->text) {
		fputs(" title=\"No aligned source line; this is a gap, not a blank source line.\"></div>",
		      out);
		return;
	}
	if (row->shared)
		fputs(" title=\"Both patches make this edit; its counterpart may occur at another position.\"",
		      out);
	fprintf(out, " data-newline=\"%s\"><span class=\"sign\">%c</span>",
		row->len && row->text[row->len - 1] == '\n' ? "yes" : "no",
		row->sign);
	for (int stage = 0; stage < 2; stage++) {
		fprintf(out,
			"<span class=\"number %s\" title=\"%s line number\">",
			stage ? "new" : "old", stage ? "Result" : "Parent");
		if (row->pos[stage] != SIZE_MAX) {
			size_t digits = 1;

			for (size_t n = row->pos[stage] + 1; n >= 10; n /= 10)
				digits++;
			fprintf(out, "%zu", row->pos[stage] + 1);
			layout->digits = MAX(layout->digits, digits);
		}
		fputs("</span>", out);
	}
	fputs("<span class=\"source-text\">", out);
	write_highlighted_text(out, text, h, leg);
	fputs("</span></div>", out);
	layout->width = MAX(layout->width, display_text_width(text, tab_width));
}

static void write_note_pair(FILE *out, const char *const text[2],
			    const char *kind)
{
	if (!text[0] && !text[1])
		return;

	fprintf(out, "<div class=\"pair %s\">", kind);
	for (int leg = 0; leg < 2; leg++) {
		fputs("<div class=\"side\">", out);
		write_html_label(out, text[leg]);
		fputs("</div>", out);
	}
	fputs("</div>\n", out);
}

static bool pair_has_change(const struct review_pair *pair)
{
	for (int leg = 0; leg < 2; leg++) {
		const struct review_row *row = &pair->side[leg];

		if (row->text && !row->shared &&
		    (row->sign == '+' || row->sign == '-'))
			return true;
	}
	return false;
}

static ptrdiff_t indentation_shift(const char *const text[2],
				   const struct text_highlight *h)
{
	size_t columns[2] = {};

	/* Viewer tab stops don't change the source comparison's indentation */
	for (int leg = 0; leg < 2; leg++) {
		for (size_t i = 0; i < h->indent[leg]; i++) {
			if (text[leg][i] == '\t')
				columns[leg] +=
					tab_width - columns[leg] % tab_width;
			else
				columns[leg]++;
		}
	}
	return (ptrdiff_t)columns[1] - (ptrdiff_t)columns[0];
}

static void write_source_pair(FILE *out, const struct review_pair *pair,
			      struct html_layout *layout, bool change_start)
{
	struct text_highlight h __free(text_highlight) = {};
	struct iomem_slice source[2] = {};
	char *right __free(free) = NULL;
	char *left __free(free) = NULL;
	struct iomem_slice raw[2] = {};
	const char *notes[2] = {};
	const char *text[2];

	/*
	 * Display text omits the final LF, but indentation-only comparisons
	 * must include it. Keep raw bytes separately so an EOF change stays a
	 * change.
	 */
	for (int leg = 0; leg < 2; leg++) {
		const struct review_row *row = &pair->side[leg];
		size_t len = row->len;

		if (!row->text)
			continue;

		if (len && row->text[len - 1] == '\n')
			len--;
		else
			notes[leg] = "\\ No newline at end of file";
		raw[leg] = (typeof(raw[0])){ row->text, row->len };
		source[leg] = (typeof(source[0])){ row->text, len };
	}
	right = escape_source_text(source[1].base, source[1].len);
	left = escape_source_text(source[0].base, source[0].len);
	text[0] = left;
	text[1] = right;
	highlight_pair(raw, text, &h);
	fprintf(out, "<div class=\"pair code-pair\" id=\"row-%zu\"%s",
		layout->row++, change_start ? " data-change-start" : "");
	if (h.indentation_only)
		fprintf(out, " data-indent-shift=\"%td\"",
			indentation_shift(text, &h));
	fputs(">", out);
	for (int leg = 0; leg < 2; leg++) {
		write_source_cell(out, &pair->side[leg], text[leg], &h, leg,
				  layout);
	}
	fputs("</div>\n", out);
	write_note_pair(out, notes, "newline-note");
}

static void write_source_block(FILE *out, const struct review_section *section,
			       const struct review_block *block, bool context,
			       struct html_layout *layout)
{
	bool previous_change = false;

	fputs("<div class=\"hunk\"><div class=\"pair column-headings\">", out);
	for (int leg = 0; leg < 2; leg++) {
		fprintf(out,
			"<div class=\"side header-side\"><span></span><span class=\"number\">%s</span><span class=\"number\">new</span><span></span></div>",
			context ? "" : "old");
	}
	fputs("</div>", out);
	if (block->name[0] || block->name[1])
		write_note_pair(out, (const char *const *)block->name,
				"function-pair");

	/* Navigation stops once per consecutive run of changed pairs */
	for (size_t i = block->first; i < block->first + block->count; i++) {
		const struct review_pair *pair = &section->rows[i];
		bool changed = pair_has_change(pair);

		write_source_pair(out, pair, layout,
				  changed && !previous_change);
		previous_change = changed;
	}
	fputs("</div>", out);
}

static void write_file_section(FILE *out, const struct review_file *file,
			       size_t file_index, int section_index,
			       struct html_layout *layout)
{
	const struct review_section *section = &file->section[section_index];

	fprintf(out,
		"<article class=\"file-block\" id=\"file-%d-%zu\"><h3 class=\"pair file-heading\"%s>",
		section_index, file_index,
		!section_index && file->metadata_diff ? " data-change-start" :
							"");
	for (int leg = 0; leg < 2; leg++) {
		fputs("<span>", out);
		write_html_label(out, file->path[leg]);
		fputs("</span>", out);
	}
	fputs("</h3><div class=\"code-viewport\">", out);
	if (file->ambiguous) {
		const char *note =
			"Overlapping source quotations: edits shown without assuming correspondence.";
		const char *notes[2] = { note, note };

		write_note_pair(out, notes, "note-pair");
	}

	/* File operations belong to delta, even without source rows */
	if (!section_index) {
		if (file->metadata_diff)
			write_note_pair(out,
					(const char *const *)file->metadata,
					"note-pair");
		write_note_pair(out, (const char *const *)file->note,
				"note-pair");
	}
	for (size_t b = 0; b < section->nblocks; b++) {
		write_source_block(out, section, &section->blocks[b],
				   section_index, layout);
	}
	fputs("</div></article>\n", out);
}

static void write_plain_report(FILE *out, const struct review_report *report,
			       const char *const identities[2],
			       const bool present[2])
{
	struct iomem_buf text __free(iomem_buf) = {};
	struct iomem_writer writer = {};

	/* Copying keeps the whole report, including hidden sections */
	iomem_writer_open(&writer);
	render_text_report(writer.fp, report, identities, present, false);
	iomem_writer_publish(&writer, &text);
	fputs("<template id=\"plain-report\">", out);
	write_html_text(out, text.base, text.len);
	fputs("</template>\n", out);
}

static void write_html_header(FILE *out, const char *const identities[2],
			      const char *const subjects[2])
{
	const char *titles[2];

	for (int leg = 0; leg < 2; leg++) {
		titles[leg] = subjects[leg] && subjects[leg][0] ?
				      subjects[leg] :
				      "(no commit subject)";
	}
	fputs("<head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>",
	      out);
	if (git_tree_dir) {
		write_html_label(out, titles[0]);
		if (strcmp(titles[0], titles[1])) {
			fputs(" / ", out);
			write_html_label(out, titles[1]);
		}
		fputs(" | diffofdiffs", out);
	} else {
		fputs("diffofdiffs report", out);
	}
	fputs("</title><style>\n", out);
	fputs(html_css, out);
	fputs("</style></head><body><main><header><h1>diffofdiffs</h1><p>",
	      out);
	fputs(git_tree_dir ?
		      "Tree mode: comparisons use complete files before and after each commit." :
		      "Patch mode: comparisons use only the source lines quoted in the patches.",
	      out);
	fputs("</p><div class=\"pair operands\">", out);
	for (int leg = 0; leg < 2; leg++) {
		fputs("<div>", out);
		write_html_label(out, identities[leg]);
		if (git_tree_dir) {
			fputs("<span class=\"operand-subject\">", out);
			write_html_label(out, titles[leg]);
			fputs("</span>", out);
		}
		fputs("</div>", out);
	}
	fputs("</div></header>\n", out);
}

void render_html_report(FILE *out, const struct review_report *report,
			const char *const identities[2], const bool present[2])
{
	char *left_subject __free(free) =
		git_tree_dir ? gittree_commit_subject(GITTREE_PATCH1) : NULL;
	char *right_subject __free(free) =
		git_tree_dir ? gittree_commit_subject(GITTREE_PATCH2) : NULL;
	const char *subjects[2] = { left_subject, right_subject };
	struct html_layout layout = { .width = 2, .digits = 3 };

	if (!present[0] && !present[1])
		return;

	fprintf(out,
		"<!doctype html>\n<html lang=\"en\" data-theme=\"%s\" data-highlight=\"%s\" data-layout=\"review\" data-shared=\"true\" data-gaps=\"true\" data-wrap=\"true\" data-text-size=\"medium\">\n",
		theme_names[display_theme], highlight_names[highlight_mode]);
	write_html_header(out, identities, subjects);
	fputs(html_controls_html, out);
	fputs("<p class=\"legend word-legend\">Within aligned pairs: <span class=\"chip left\">left text</span> / <span class=\"chip right\">right text</span> that differs, independently of patch signs. Whitespace uses the same colors: each highlighted space has its own block; tabs fill their tab stops. Hover for details.</p>",
	      out);
	fputs("<div id=\"comparison\">", out);
	for (int s = 0; s < 2; s++) {
		if (!present[s])
			continue;

		fprintf(out,
			"<section id=\"%s\"><div class=\"section-heading\"><h2>%s differences</h2></div><p class=\"legend\">",
			s ? "context" : "delta", s ? "Context" : "Delta");
		fputs(s ? "Surrounding code in the results. Signs mark lines present in only one result: − left, + right." :
			  "Code changes that differ between the patches. Old / new are each patch’s parent / result line numbers. Signs belong to that patch. <span class=\"chip shared\">Shared edits</span> are additions or removals both patches make.",
		      out);
		fputs("</p>", out);
		for (size_t f = 0; f < report->count; f++) {
			const struct review_file *file = &report->files[f];

			if (!render_section_has_output(file, s))
				continue;

			write_file_section(out, file, f, s, &layout);
		}
		fputs("</section>\n", out);
	}
	fputs("</div></main>\n", out);
	write_plain_report(out, report, identities, present);

	/*
	 * Row emission measures the report; late CSS applies to the whole page.
	 */
	fprintf(out,
		"<style>:root { --digits: %zuch; --report-width: %zuch; --tab-size: %u; }</style>\n",
		layout.digits + 1, 2 * (layout.width + 2 * layout.digits + 8),
		tab_width);
	fputs("<script>\n", out);
	fputs(html_js, out);
	fputs("</script></body></html>\n", out);
}
