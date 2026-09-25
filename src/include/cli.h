/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 Ctrl IQ, Inc.
 *
 * Parse command-line options and retain the two operand names for main().
 *
 * Call set_progname() before cli_parse() to name application diagnostics.
 * getopt_long() emits its own option errors using argv[0] as the prefix.
 */
#ifndef CLI_H
#define CLI_H

#include <stdbool.h>

/* Update the version here when preparing a release */
#define DIFFOFDIFFS_VERSION "1.0.0"

/*
 * How --color's WHEN argument resolves. AUTO defers the terminal check to the
 * output layer; cli.c never calls isatty() itself.
 */
enum color_when {
	COLOR_WHEN_AUTO = 0,
	COLOR_WHEN_ALWAYS,
	COLOR_WHEN_NEVER
};

enum output_format {
	OUTPUT_TEXT,
	OUTPUT_HTML
};

enum highlight_mode {
	HIGHLIGHT_WORDS,
	HIGHLIGHT_CHARACTERS,
	HIGHLIGHT_NONE
};

enum display_theme {
	THEME_DARK,
	THEME_LIGHT
};

extern enum output_format output_format;
extern enum highlight_mode highlight_mode;
extern enum display_theme display_theme;

/* Context width in lines around each change; the last -U wins */
extern unsigned int max_context;

/* How the output layer should decide whether to colorize */
extern enum color_when color_when;

/*
 * Source-column width, excluding signs, numbers, and separators. Zero leaves
 * the choice to the output layer.
 */
extern unsigned int max_column_width;

/* Tab stops for terminal and HTML display; raw text keeps literal tabs */
extern unsigned int tab_width;

/* Optional Backport/Upstream labels instead of patch1/patch2 */
extern bool backport_labels;

/* NULL selects stdout; otherwise this is the path supplied with -o */
extern const char *output_path;

/*
 * The --git-tree repository, or NULL for a patch-only run. When it's set, the
 * two positional operands are commit-ish names resolved in this repository's
 * store rather than patch files.
 */
extern const char *git_tree_dir;

/*
 * Parses argv[1..argc), exiting on --help, --version, or any parse error. On
 * return, *patch1 and *patch2 point at the two operand strings from argv;
 * neither is copied.
 */
void cli_parse(int argc, char **argv, const char **patch1, const char **patch2);

#endif /* CLI_H */
