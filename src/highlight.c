// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ctrl IQ, Inc.
 *
 * Compare tokens with the same libgit2 engine used for source lines. These
 * ranges decorate an existing pair; they never establish source correspondence.
 */
#include <string.h>
#include <sys/param.h>
#include <wchar.h>
#include <wctype.h>

#include <display.h>
#include <highlight.h>
#include <udiff.h>
#include <util.h>

/* Long or unrelated lines retain their line colors without inline guesses */
#define HIGHLIGHT_LIMIT 8192

struct token_sequence {
	struct udiff_image image;
	struct udiff_line *lines;
	struct text_span *spans;
	char *bytes;
};

static void token_sequence_free(struct token_sequence *tokens)
{
	free(tokens->lines);
	free(tokens->spans);
	free(tokens->bytes);
}

DEFINE_FREE(token_sequence, struct token_sequence, token_sequence_free(&_T))
DEFINE_FREE(udiff_matches, struct udiff_matches, udiff_matches_free(&_T))

void text_highlight_free(struct text_highlight *highlight)
{
	for (int leg = 0; leg < 2; leg++) {
		free(highlight->words[leg].spans);
		free(highlight->characters[leg].spans);
	}
}

static bool word_character(const char *text, size_t len)
{
	mbstate_t state = {};
	wchar_t wc;

	return mbrtowc(&wc, text, len, &state) <= len &&
	       (wc == '_' || iswalnum(wc));
}

static size_t token_length(const char *text, size_t len, bool characters)
{
	bool word = word_character(text, len), space = *text == ' ';
	size_t at;

	at = display_character_bytes(text, len, NULL);
	if (characters || (!word && !space))
		return at;

	for (; at < len;) {
		if ((space && text[at] != ' ') ||
		    (word && !word_character(text + at, len - at)))
			break;

		at += display_character_bytes(text + at, len - at, NULL);
	}
	return at;
}

static void tokenize(const char *text, size_t start, size_t end,
		     bool characters, struct token_sequence *tokens)
{
	size_t len = end - start, written = 0;

	if (!len)
		return;

	/*
	 * Each token consumes at least one byte and adds one record separator.
	 */
	tokens->lines = xmalloc_array(len, sizeof(*tokens->lines));
	tokens->spans = xmalloc_array(len, sizeof(*tokens->spans));
	tokens->bytes = xmalloc_array(len, 2);
	tokens->image.lines = tokens->lines;
	tokens->image.bytes = tokens->bytes;

	/* Each token becomes one LF-terminated record for libgit2's line API */
	for (size_t at = start; at < end;) {
		size_t n = token_length(text + at, end - at, characters);
		size_t index = tokens->image.nlines++;

		tokens->lines[index] = (typeof(tokens->lines[0])){
			.ptr = tokens->bytes + written, .len = n + 1
		};
		tokens->spans[index] = (typeof(tokens->spans[0])){ at, at + n };
		memcpy(tokens->bytes + written, text + at, n);
		tokens->bytes[written + n] = '\n';
		written += n + 1;
		at += n;
	}
}

static void append_range(struct highlight_ranges *ranges, size_t start,
			 size_t end)
{
	if (start == end)
		return;

	if (ranges->count && ranges->spans[ranges->count - 1].end == start) {
		ranges->spans[ranges->count - 1].end = end;
		return;
	}
	if (ranges->count == ranges->capacity) {
		ranges->capacity = MAX(8, ranges->capacity * 2);
		ranges->spans = xrealloc_array(ranges->spans, ranges->capacity,
					       sizeof(*ranges->spans));
	}
	ranges->spans[ranges->count++] =
		(typeof(ranges->spans[0])){ start, end };
}

static size_t count_word_characters(const char *text, size_t start, size_t end)
{
	size_t count = 0;

	for (size_t at = start; at < end;) {
		count += word_character(text + at, end - at);
		at += display_character_bytes(text + at, end - at, NULL);
	}
	return count;
}

static size_t refine_replacement(const char *const text[2],
				 const size_t start[2], const size_t end[2],
				 struct text_highlight *out)
{
	struct token_sequence right __free(token_sequence) = {};
	struct udiff_matches matches __free(udiff_matches) = {};
	struct token_sequence left __free(token_sequence) = {};
	struct token_sequence *tokens[2] = { &left, &right };
	size_t common = 0;

	for (int leg = 0; leg < 2; leg++)
		tokenize(text[leg], start[leg], end[leg], true, tokens[leg]);
	udiff_match(&tokens[0]->image, &tokens[1]->image, &matches);
	for (int leg = 0; leg < 2; leg++) {
		for (size_t i = 0; i < tokens[leg]->image.nlines; i++) {
			const struct text_span *span = &tokens[leg]->spans[i];

			if (matches.side[leg][i] == SIZE_MAX) {
				append_range(&out->characters[leg], span->start,
					     span->end);
			} else if (!leg) {
				common +=
					word_character(text[leg] + span->start,
						       span->end - span->start);
			}
		}
	}

	return common;
}

static size_t highlight_words(const char *const text[2], const size_t len[2],
			      struct text_highlight *out)
{
	struct token_sequence right __free(token_sequence) = {};
	struct udiff_matches matches __free(udiff_matches) = {};
	struct token_sequence left __free(token_sequence) = {};
	struct token_sequence *tokens[2] = { &left, &right };
	size_t start[2] = { out->indent[0], out->indent[1] };
	size_t common = 0;

	for (int leg = 0; leg < 2; leg++)
		tokenize(text[leg], start[leg], len[leg], false, tokens[leg]);
	udiff_match(&tokens[0]->image, &tokens[1]->image, &matches);

	/*
	 * Refine only the runs between shared words. A global character diff
	 * can otherwise borrow punctuation from the next argument or statement.
	 */
	for (size_t i = 0; i <= tokens[0]->image.nlines; i++) {
		bool last = i == tokens[0]->image.nlines;
		size_t end[2] = { len[0], len[1] };
		size_t next[2] = { len[0], len[1] };

		if (!last) {
			const struct text_span *span = &tokens[0]->spans[i];
			size_t j = matches.side[0][i];

			if (j == SIZE_MAX)
				continue;

			end[0] = span->start;
			end[1] = tokens[1]->spans[j].start;
			next[0] = span->end;
			next[1] = tokens[1]->spans[j].end;
			common +=
				count_word_characters(text[0], end[0], next[0]);
		}
		if (start[0] != end[0] || start[1] != end[1]) {
			for (int leg = 0; leg < 2; leg++) {
				append_range(&out->words[leg], start[leg],
					     end[leg]);
			}
			common += refine_replacement(text, start, end, out);
		}
		memcpy(start, next, sizeof(start));
	}
	return common;
}

void highlight_pair(const struct iomem_slice source[2],
		    const char *const text[2], struct text_highlight *out)
{
	size_t len[2], letters = 0, common;
	bool newline[2];
	bool same_body;

	if (!source[0].base || !source[1].base)
		return;

	for (int leg = 0; leg < 2; leg++) {
		newline[leg] = source[leg].len &&
			       source[leg].base[source[leg].len - 1] == '\n';
		len[leg] = strlen(text[leg]);
		if (len[leg] > HIGHLIGHT_LIMIT)
			return;

		for (size_t i = 0;
		     i < source[leg].len && (source[leg].base[i] == ' ' ||
					     source[leg].base[i] == '\t');
		     i++) {
			out->indent[leg]++;
			if (source[leg].base[i] == '\t')
				out->indent_columns[leg] +=
					TAB_WIDTH -
					out->indent_columns[leg] % TAB_WIDTH;
			else
				out->indent_columns[leg]++;
		}
	}
	if (len[0] + len[1] > HIGHLIGHT_LIMIT)
		return;

	/* Equal-width indentation can still differ in its source bytes */
	for (size_t i = 0; i < MIN(out->indent[0], out->indent[1]) &&
			   source[0].base[i] == source[1].base[i];
	     i++)
		out->common_indent++;
	out->indentation_changed = out->common_indent !=
				   MAX(out->indent[0], out->indent[1]);

	/* Indentation claims may ignore only leading spaces and tabs */
	same_body = source[0].len - out->indent[0] ==
			    source[1].len - out->indent[1] &&
		    !memcmp(source[0].base + out->indent[0],
			    source[1].base + out->indent[1],
			    source[0].len - out->indent[0]);
	out->indentation_only = out->indentation_changed && same_body &&
				source[0].len - newline[0] > out->indent[0];
	if (same_body)
		return;

	common = highlight_words(text, len, out);

	/*
	 * Compare shared word characters against the larger body, excluding
	 * punctuation. Below 35% shared text, suppress all emphasis so
	 * unrelated statements cannot suggest a meaningful word or indentation
	 * change.
	 */
	for (int leg = 0; leg < 2; leg++) {
		letters = MAX(letters,
			      count_word_characters(text[leg], out->indent[leg],
						    len[leg]));
	}
	if (!common || common * 100 < letters * 35) {
		for (int leg = 0; leg < 2; leg++) {
			out->words[leg].count = 0;
			out->characters[leg].count = 0;
		}
		out->indentation_changed = false;
	}
}
