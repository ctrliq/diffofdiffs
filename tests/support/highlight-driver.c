// SPDX-License-Identifier: GPL-2.0-only
/* Expose native ranges for tests without depending on source-row alignment */
#include <locale.h>
#include <string.h>

#include <display.h>
#include <highlight.h>
#include <util.h>

DEFINE_FREE(text_highlight, struct text_highlight, text_highlight_free(&_T))
DEFINE_FREE(iomem_buf, struct iomem_buf, iomem_buf_free(&_T))

/* Offsets index escaped UTF-8 bytes, not character or column counts */
static void print_ranges(const struct highlight_ranges ranges[2],
			 const char *name)
{
	for (int leg = 0; leg < 2; leg++) {
		for (size_t i = 0; i < ranges[leg].count; i++) {
			const struct text_span *span = &ranges[leg].spans[i];

			printf("%s %d %zu %zu\n", name, leg, span->start,
			       span->end);
		}
	}
}

int main(int argc, char **argv)
{
	struct text_highlight h __free(text_highlight) = {};
	struct iomem_buf right __free(iomem_buf) = {};
	struct iomem_buf left __free(iomem_buf) = {};
	struct iomem_slice source[2];
	char *b __free(free) = NULL;
	char *a __free(free) = NULL;
	const char *text[2];
	size_t len[2];

	set_progname("highlight-driver");
	mem_limit_init();
	if (argc != 3)
		die("expected two source files");

	setlocale(LC_CTYPE, "C.UTF-8");
	if (!strcmp(argv[1], "--display")) {
		size_t bytes, without_columns;
		int columns = 123;

		iomem_acquire(&left, argv[2]);
		bytes = display_character_bytes(left.base, left.len, &columns);
		without_columns =
			display_character_bytes(left.base, left.len, NULL);
		printf("%zu %d %zu\n", bytes, columns, without_columns);
		check_output(stdout, "standard output");
		return 0;
	}
	iomem_acquire(&left, argv[1]);
	iomem_acquire(&right, argv[2]);
	source[0] = (typeof(source[0])){ left.base, left.len };
	source[1] = (typeof(source[0])){ right.base, right.len };
	for (int leg = 0; leg < 2; leg++) {
		len[leg] = source[leg].len;
		if (len[leg] && source[leg].base[len[leg] - 1] == '\n')
			len[leg]--;
	}
	b = escape_source_text(right.base, len[1]);
	a = escape_source_text(left.base, len[0]);
	text[0] = a;
	text[1] = b;
	highlight_pair(source, text, &h);
	printf("indent %d %d %zu %zu\n", h.indentation_changed,
	       h.indentation_only, h.indent_columns[0], h.indent_columns[1]);
	print_ranges(h.words, "words");
	print_ranges(h.characters, "characters");
	check_output(stdout, "standard output");
	return 0;
}
