/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2025-2026 Ctrl IQ, Inc.
 * Author: Sultan Alsawaf <sultan@ciq.com>
 *
 * Definition-line conventions shared by tree patch assembly and reporting.
 */
#ifndef FUNCNAME_H
#define FUNCNAME_H

#include <ctype.h>
#include <stdbool.h>
#include <string.h>

#include <udiff.h>

static inline bool is_def_line(const char *s)
{
	return isalpha(*s) || *s == '_' || *s == '$';
}

static inline bool path_names_c_source(const char *name)
{
	const char *dot;

	if (!name)
		return false;

	dot = strrchr(name, '.');
	if (!dot || strchr(dot, '/'))
		return false;

	return !strcmp(dot, ".c") || !strcmp(dot, ".h");
}

/*
 * A candidate definition's declaration, name, opening brace, and exclusive end
 * row. The name is the first parenthesized declaration line, or the declaration
 * itself when none is found; attributes and macros can make it approximate.
 */
struct c_scope {
	size_t first;
	size_t name;
	size_t body;
	size_t end;
};

/*
 * Infer definition boundaries from lexical layout without evaluating
 * preprocessor branches. A column-zero closing brace resynchronizes depth after
 * those branches. With scopes, collect complete regions; without it, return the
 * last definition name at first, allowing its declaration to finish after that
 * row. Comments, literals, and preprocessor lines cannot open scopes.
 */
static inline size_t c_scan(const struct udiff_image *image, size_t first,
			    struct c_scope *scopes, size_t *count)
{
	size_t declaration = SIZE_MAX, declarator = SIZE_MAX, name = SIZE_MAX;
	bool comment = false, escaped = false, preprocessor = false;
	size_t parentheses = 0, braces = 0;
	bool scope_open = false;
	char quote = 0;

	/*
	 * Comments, continued literals, and directives can span physical lines.
	 */
	for (size_t i = 0; i < image->nlines; i++) {
		const struct udiff_line *line = &image->lines[i];
		size_t start = 0, end = line->len;

		if (!scopes && i > first && (braces || declaration == SIZE_MAX))
			break;

		for (; start < end && isspace(line->ptr[start]); start++)
			;
		if (!comment && !quote &&
		    (preprocessor ||
		     (start < end && line->ptr[start] == '#'))) {
			for (; end && (line->ptr[end - 1] == '\n' ||
				       line->ptr[end - 1] == '\r');
			     end--)
				;
			preprocessor = end && line->ptr[end - 1] == '\\';
			continue;
		}

		if (!comment && !quote && !braces && declaration == SIZE_MAX &&
		    line->len && is_def_line(line->ptr))
			declaration = i;

		/* Preprocessor branches can leave unmatched braces */
		if (!comment && !quote && line->len && line->ptr[0] == '}') {
			parentheses = 0;
			braces = 1;
		}
		for (size_t j = 0; j < line->len; j++) {
			char next = j + 1 < line->len ? line->ptr[j + 1] : 0;
			char c = line->ptr[j];

			if (comment) {
				if (c == '*' && next == '/') {
					comment = false;
					j++;
				}
				continue;
			}

			if (quote) {
				if (escaped)
					escaped = false;
				else if (c == '\\')
					escaped = true;
				else if (c == quote)
					quote = 0;
				continue;
			}

			if (c == '/' && next == '/')
				break;

			if (c == '/' && next == '*') {
				comment = true;
				j++;
				continue;
			}
			if (c == '\'' || c == '"') {
				quote = c;
				continue;
			}
			if (c == '(') {
				if (!braces && declarator == SIZE_MAX)
					declarator = i;
				parentheses++;
				continue;
			}
			if (c == ')' && parentheses) {
				parentheses--;
				continue;
			}
			if (parentheses)
				continue;

			if (c == '{') {
				braces++;
				if (braces != 1 || declaration == SIZE_MAX)
					continue;

				name = declaration;
				if (declarator != SIZE_MAX)
					name = declarator;
				if (scopes) {
					scopes[*count] = (typeof(scopes[0])){
						.first = declaration,
						.name = name,
						.body = i
					};
					scope_open = true;
				}
				continue;
			}
			if (c == '}' && braces) {
				braces--;

				/* A recovered brace may have no open record */
				if (!braces && scope_open) {
					scopes[(*count)++].end = i + 1;
					scope_open = false;
				}
			}
			if (!braces && (c == ';' || c == '}' || c == ':'))
				declaration = declarator = SIZE_MAX;
		}
	}
	return name;
}

/* The first printed line can itself begin a new definition */
static inline size_t source_definition(const struct udiff_image *image,
				       size_t first, bool c_source)
{
	size_t end = first < image->nlines ? first + 1 : image->nlines;

	if (c_source)
		return c_scan(image, first, NULL, NULL);

	for (size_t i = end; i--;) {
		if (image->lines[i].len && is_def_line(image->lines[i].ptr))
			return i;
	}
	return SIZE_MAX;
}

#endif /* FUNCNAME_H */
