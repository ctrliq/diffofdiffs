/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef HIGHLIGHT_H
#define HIGHLIGHT_H

#include <iomem.h>

/* Half-open byte ranges in escaped source text, at character boundaries */
struct text_span {
	size_t start;
	size_t end;
};

/* Sorted, disjoint ranges */
struct highlight_ranges {
	struct text_span *spans;
	size_t count;
	size_t capacity;
};

/*
 * indent and common_indent count leading whitespace bytes, which escaping
 * preserves verbatim; indent_columns counts their display columns instead.
 * Equal column counts can still have different indentation bytes. An
 * indentation-only pair has equal bodies, including final-newline state.
 */
struct text_highlight {
	/* Both range sets exclude leading indentation */
	struct highlight_ranges words[2];
	struct highlight_ranges characters[2];
	size_t indent[2];
	size_t indent_columns[2];
	size_t common_indent;
	bool indentation_changed;
	bool indentation_only;
};

/*
 * Source slices retain the final LF; display text omits it and comes from
 * escape_source_text(). Missing sides have a NULL base; an empty source line
 * still has a base. The caller initializes out to zero and owns its ranges.
 */
void highlight_pair(const struct iomem_slice source[2],
		    const char *const text[2], struct text_highlight *out);
void text_highlight_free(struct text_highlight *highlight);

#endif /* HIGHLIGHT_H */
