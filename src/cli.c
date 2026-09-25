// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ctrl IQ, Inc.
 *
 * Parse command-line options and retain the two operand names for main().
 */
#define _GNU_SOURCE

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cli.h>
#include <display.h>
#include <util.h>

unsigned int max_context = 3;
unsigned int max_column_width;
unsigned int tab_width = TAB_WIDTH;
enum color_when color_when = COLOR_WHEN_AUTO;
enum output_format output_format = OUTPUT_TEXT;
enum highlight_mode highlight_mode = HIGHLIGHT_WORDS;
enum display_theme display_theme = THEME_DARK;
bool backport_labels;
const char *git_tree_dir = NULL;
const char *output_path;

/*
 * Only --help sends usage to stdout and exits successfully. Check its output
 * for write errors just as we do for a completed comparison.
 */
static void usage(int err)
{
	FILE *stream = err ? stderr : stdout;

	fprintf(stream,
		"usage: %s [OPTIONS] patch1 patch2\n"
		"       %s [OPTIONS] --git-tree=DIR rev1 rev2\n"
		"       %s --help|--version\n"
		"\n"
		"Compare the original changes in patch1 and patch2. Either operand may be\n"
		"a single \"-\" to read standard input, but not both.\n"
		"\n"
		"With --git-tree, each operand names a commit in DIR. Its patch describes\n"
		"the changes from its first parent, or an empty tree if it has no parent.\n"
		"For a merge commit, use PARENT..COMMIT to choose which parent's version to\n"
		"compare against. PARENT must be a direct parent of COMMIT.\n"
		"\n"
		"Empty comparison output with exit status 0 means no differences were found.\n"
		"\n"
		"OPTIONS are:\n"
		"  -U N, --unified=N       keep N context lines before and after each differing\n"
		"                          block; the default is 3\n"
		"\n"
		"  --max-column-width=N    cap each source column at N display columns (at least\n"
		"                          2); the default uses up to the terminal width, or 100\n"
		"                          per source column when no terminal size is available\n"
		"\n"
		"  --tab-width=N           use N-column tab stops for terminal and HTML display;\n"
		"                          N must be positive (default: 8)\n"
		"\n"
		"  --backport-labels       label patch1 Backport and patch2 Upstream\n"
		"\n"
		"  --git-tree=DIR          read both operands as revisions in the Git repository\n"
		"                          at DIR (a working tree or a bare store); full source\n"
		"                          files give a more complete comparison than the lines\n"
		"                          quoted in patch files\n"
		"\n"
		"  --color[=WHEN]          colorize the output; WHEN is 'auto' (the default),\n"
		"                          'always', or 'never'\n"
		"\n"
		"  -o FILE, --output=FILE  write to FILE instead of standard output; '-' uses\n"
		"                          stdout\n"
		"\n"
		"  --html                  write a standalone HTML report instead of text\n"
		"\n"
		"  --highlight=MODE        highlight changes within paired lines:\n"
		"                          'words': whole words, punctuation, and whitespace\n"
		"                          'characters': only the differing characters\n"
		"                          'none': no inline highlights; keep line colors\n"
		"                          The default is 'words'. Text output needs\n"
		"                          color enabled to show highlights.\n"
		"\n"
		"  --theme=THEME           'dark' or 'light' for HTML (default: dark)\n"
		"\n"
		"  --debug                 print parse diagnostics to standard error; the output\n"
		"                          is not a stable interface and may change at any time\n"
		"\n"
		"  --help                  print this text and exit\n"
		"\n"
		"  --version               print the version number and exit\n",
		progname, progname, progname);

	if (!err)
		check_output(stdout, "standard output");
	exit(err);
}

/*
 * Counts use decimal even with a leading zero. Accept strtoul's whitespace and
 * sign handling, but require a complete number that fits the engine's int
 * coordinates. In particular, "-1" becomes an unsigned value above INT_MAX.
 */
static int parse_count(const char *arg)
{
	unsigned long value;
	char *end;

	errno = 0;
	value = strtoul(arg, &end, 10);
	if (arg == end || *end || errno || value > INT_MAX)
		usage(1);

	return value;
}

void cli_parse(int argc, char **argv, const char **patch1, const char **patch2)
{
	/*
	 * Adding 1000 keeps long-only options outside the byte values returned
	 * for short options.
	 */
	static const struct option long_options[] = {
		{ "help", no_argument, NULL, 1000 + 'H' },
		{ "version", no_argument, NULL, 1000 + 'V' },
		{ "debug", no_argument, NULL, 1000 + 'D' },
		{ "git-tree", required_argument, NULL, 1000 + 'g' },
		{ "max-column-width", required_argument, NULL, 1000 + 'w' },
		{ "tab-width", required_argument, NULL, 1000 + 'T' },
		{ "backport-labels", no_argument, NULL, 1000 + 'b' },
		{ "color", optional_argument, NULL, 1000 + 'c' },
		{ "html", no_argument, NULL, 1000 + 'm' },
		{ "highlight", required_argument, NULL, 1000 + 'h' },
		{ "theme", required_argument, NULL, 1000 + 't' },
		{ "unified", required_argument, NULL, 'U' },
		{ "output", required_argument, NULL, 'o' },
		{ NULL, 0, NULL, 0 }
	};

	for (;;) {
		int c;

		c = getopt_long(argc, argv, "U:o:", long_options, NULL);
		if (c == -1)
			break;

		switch (c) {
		case 1000 + 'H':
			usage(0);
			break;
		case 1000 + 'V':
			printf("%s %s\n", progname, DIFFOFDIFFS_VERSION);
			check_output(stdout, "standard output");
			exit(0);
		case 1000 + 'D':
			debug = true;
			break;
		case 1000 + 'g':
			git_tree_dir = optarg;
			break;
		case 1000 + 'w':
			max_column_width = parse_count(optarg);
			if (max_column_width < 2 || strchr(optarg, '-'))
				usage(1);
			break;
		case 1000 + 'T':
			tab_width = parse_count(optarg);
			if (!tab_width || strchr(optarg, '-'))
				usage(1);
			break;
		case 1000 + 'b':
			backport_labels = true;
			break;
		case 1000 + 'c': {
			const char *when = optarg ? optarg : "auto";

			if (!strcmp(when, "auto"))
				color_when = COLOR_WHEN_AUTO;
			else if (!strcmp(when, "always"))
				color_when = COLOR_WHEN_ALWAYS;
			else if (!strcmp(when, "never"))
				color_when = COLOR_WHEN_NEVER;
			else
				usage(1);
			break;
		}
		case 1000 + 'm':
			output_format = OUTPUT_HTML;
			break;
		case 1000 + 'h':
			if (!strcmp(optarg, "words"))
				highlight_mode = HIGHLIGHT_WORDS;
			else if (!strcmp(optarg, "characters"))
				highlight_mode = HIGHLIGHT_CHARACTERS;
			else if (!strcmp(optarg, "none"))
				highlight_mode = HIGHLIGHT_NONE;
			else
				usage(1);
			break;
		case 1000 + 't':
			if (!strcmp(optarg, "dark"))
				display_theme = THEME_DARK;
			else if (!strcmp(optarg, "light"))
				display_theme = THEME_LIGHT;
			else
				usage(1);
			break;
		case 'U':
			max_context = parse_count(optarg);
			break;
		case 'o':
			output_path = strcmp(optarg, "-") ? optarg : NULL;
			break;
		default:
			usage(1);
		}
	}

	if (optind + 2 != argc)
		usage(1);

	/* In tree mode, operands name revisions and cannot consume stdin */
	if (git_tree_dir) {
		if (!strcmp(git_tree_dir, "-"))
			die("--git-tree takes a directory, never stdin");

		if (!strcmp(argv[optind], "-") ||
		    !strcmp(argv[optind + 1], "-"))
			die("a revision operand cannot be \"-\" under --git-tree");
	}

	if (!strcmp(argv[optind], "-") && !strcmp(argv[optind + 1], "-"))
		die("only one input file can come from stdin");

	*patch1 = argv[optind];
	*patch2 = argv[optind + 1];
}
