/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DISPLAY_H
#define DISPLAY_H

#include <stddef.h>

/* Source comparison and raw terminal tabs use the default eight-column stops */
#define TAB_WIDTH 8

/*
 * Return owned strings containing printable characters, tabs, newlines, and
 * ASCII \xNN escapes for all other bytes, with no embedded NUL. Decoding uses
 * the current LC_CTYPE locale, which must stay the same while measuring the
 * result. Source tabs survive escaping; labels expand them before centering.
 */
char *expand_display_text(const char *text, size_t len);
char *escape_source_text(const char *text, size_t len);

/*
 * Expand tabs in escaped source text from column zero. An optional offsets
 * array has strlen(text) + 1 entries and maps character boundaries to the
 * returned string, including its final NUL.
 */
char *expand_source_tabs(const char *text, unsigned int tabstop,
			 size_t *offsets);

/*
 * Return the first character's byte count, or zero for NUL, invalid encoding,
 * or incomplete input. If columns is non-NULL, always set it to the terminal
 * width, or -1 on a zero return or for a character that cannot be displayed.
 */
size_t display_character_bytes(const char *text, size_t len, int *columns);
size_t display_text_width(const char *text, unsigned int tabstop);

#endif /* DISPLAY_H */
