// SPDX-License-Identifier: GPL-2.0-only
/* Return source correspondences for independent matching checks */
#include <stdlib.h>
#include <string.h>

#include <udiff.h>
#include <iomem.h>
#include <util.h>

struct match_input {
	struct iomem_buf bytes;
	struct udiff_line *lines;
	struct udiff_image image;
};

static void match_input_free(struct match_input *input)
{
	iomem_buf_free(&input->bytes);
	free(input->lines);
}

DEFINE_FREE(match_input, struct match_input, match_input_free(&_T))
DEFINE_FREE(udiff_matches, struct udiff_matches, udiff_matches_free(&_T))

static void match_input_read(struct match_input *input, const char *name)
{
	struct iomem_buf *bytes = &input->bytes;
	size_t count = 0, first = 0;

	iomem_acquire(bytes, name);
	for (size_t i = 0; i < bytes->len; i++)
		count += bytes->base[i] == '\n';
	count += bytes->len && bytes->base[bytes->len - 1] != '\n';
	input->lines = xmalloc_array(count, sizeof(*input->lines));
	input->image = (typeof(input->image)){ .lines = input->lines,
					       .nlines = count };
	for (size_t i = 0; i < count; i++) {
		const char *newline =
			memchr(bytes->base + first, '\n', bytes->len - first);
		size_t end = newline ? (size_t)(newline - bytes->base) + 1 :
				       bytes->len;

		input->lines[i] =
			(typeof(*input->lines)){ .ptr = bytes->base + first,
						 .len = end - first };
		first = end;
	}
}

int main(int argc, char **argv)
{
	struct udiff_matches matches __free(udiff_matches) = {};
	struct match_input a __free(match_input) = {};
	struct match_input b __free(match_input) = {};
	size_t previous = 0;
	bool seen = false;

	set_progname("match-driver");
	if (argc != 3)
		die("usage: match-driver OLD NEW");

	match_input_read(&a, argv[1]);
	match_input_read(&b, argv[2]);
	udiff_match(&a.image, &b.image, &matches);
	for (size_t i = 0; i < a.image.nlines; i++) {
		size_t other = matches.side[0][i];

		if (other == SIZE_MAX)
			continue;

		if (other >= b.image.nlines || matches.side[1][other] != i ||
		    (seen && other <= previous) ||
		    a.lines[i].len != b.lines[other].len ||
		    memcmp(a.lines[i].ptr, b.lines[other].ptr, a.lines[i].len))
			die("invalid correspondence at %zu,%zu", i, other);

		printf("%zu %zu\n", i, other);
		previous = other;
		seen = true;
	}
	for (size_t i = 0; i < b.image.nlines; i++) {
		size_t other = matches.side[1][i];

		if (other != SIZE_MAX &&
		    (other >= a.image.nlines || matches.side[0][other] != i))
			die("invalid reverse correspondence at %zu,%zu", i,
			    other);
	}
	check_output(stdout, "standard output");
	return 0;
}
