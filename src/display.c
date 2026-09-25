// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ctrl IQ, Inc.
 *
 * Keep source bytes separate from their display width so both renderers can
 * preserve tabs without allowing other control bytes to affect the display.
 */
#define _GNU_SOURCE

#include <string.h>
#include <sys/param.h>
#include <wchar.h>

#include <display.h>
#include <iomem.h>
#include <types.h>
#include <util.h>

size_t display_character_bytes(const char *text, size_t len, int *columns)
{
	mbstate_t state = {};
	wchar_t wc;
	size_t n;

	/*
	 * A decoded control character still has a byte count, but no printable
	 * width. Use the same width sentinel when decoding itself fails.
	 */
	if (columns)
		*columns = -1;
	n = mbrtowc(&wc, text, len, &state);
	if (!n || n == (size_t)-1 || n == (size_t)-2)
		return 0;

	if (columns)
		*columns = wcwidth(wc);
	return n;
}

static char *format_display_text(const char *text, size_t len,
				 unsigned int tabstop, size_t *offsets)
{
	struct iomem_writer writer = {};
	struct iomem_buf out = {};
	size_t column = 0, written = 0;

	iomem_writer_open(&writer);
	for (size_t i = 0; i < len;) {
		int columns;
		size_t n;

		if (offsets)
			offsets[i] = written;
		if (text[i] == '\t') {
			size_t width = tabstop ?: TAB_WIDTH;
			size_t spaces = width - column % width;

			/*
			 * Expand tabs from the source or label origin before
			 * wrapping. File output keeps the original tab byte.
			 */
			if (tabstop) {
				for (size_t j = 0; j < spaces; j++)
					fputc(' ', writer.fp);
				written += spaces;
			} else {
				fputc('\t', writer.fp);
				written++;
			}
			column += spaces;
			i++;
			continue;
		}
		if (text[i] == '\n') {
			fputc('\n', writer.fp);
			written++;
			column = 0;
			i++;
			continue;
		}

		n = display_character_bytes(text + i, len - i, &columns);
		if (n && columns >= 0) {
			fwrite(text + i, 1, n, writer.fp);
			written += n;
			column += columns;
			i += n;
		} else {
			/*
			 * Escape one byte at a time so an invalid sequence
			 * cannot swallow the valid character that follows it.
			 * NUL and control bytes also need printable escape
			 * sequences.
			 */
			fprintf(writer.fp, "\\x%02x", (u8)text[i++]);
			written += 4;
			column += 4;
		}
	}
	if (offsets)
		offsets[len] = written;
	iomem_writer_publish(&writer, &out);
	return out.base;
}

char *expand_display_text(const char *text, size_t len)
{
	return format_display_text(text, len, TAB_WIDTH, NULL);
}

char *escape_source_text(const char *text, size_t len)
{
	return format_display_text(text, len, 0, NULL);
}

char *expand_source_tabs(const char *text, unsigned int tabstop,
			 size_t *offsets)
{
	return format_display_text(text, strlen(text), tabstop, offsets);
}

size_t display_text_width(const char *text, unsigned int tabstop)
{
	size_t widest = 0, column = 0, len = strlen(text);

	/*
	 * Callers pass escaped display text, so every remaining character can
	 * be decoded and measured. Newlines separate widths rather than adding
	 * to them; the longest physical line determines the required column.
	 */
	for (size_t i = 0; i < len;) {
		int columns;

		if (text[i] == '\t') {
			column += tabstop - column % tabstop;
			i++;
		} else if (text[i] == '\n') {
			widest = MAX(widest, column);
			column = 0;
			i++;
		} else {
			i += display_character_bytes(text + i, len - i,
						     &columns);
			column += columns;
		}
	}
	return MAX(widest, column);
}
