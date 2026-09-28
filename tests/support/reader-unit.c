// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Ctrl IQ, Inc.
 *
 * Direct checks for filename boundaries, path ranking, and inferred stripping.
 * These expectations describe the input grammar without relying on a recorded
 * comparison report.
 */
#include <assert.h>
#include <string.h>

#include <reader.h>
#include <util.h>

static void expect_header(const char *header, size_t len, const char *expected,
			  size_t expected_len, bool verbatim)
{
	struct patch_name *name __cleanup(patch_name_pointer_free) =
		filename_from_header(header, len);

	assert(name->len == expected_len);
	assert(!memcmp(name->text, expected, expected_len));
	assert(!name->text[expected_len]);
	assert(name->verbatim == verbatim);
}

static void check_headers(void)
{
	/* The source need not end at a NUL, and embedded NULs remain visible */
#define HEADER(text, name, verbatim) \
	expect_header(text, sizeof(text) - 1, name, sizeof(name) - 1, verbatim)
	HEADER("", "", false);
	HEADER("a\0b", "a\0b", false);
	HEADER("dir/file", "dir/file", false);
	HEADER("a file name", "a file name", false);
	HEADER("file\tannotation", "file", false);
	HEADER("a file\t2026-01-02 03:04:05 +0000", "a file", false);
	HEADER("a file 2026-01-02 03:04:05.123 +0000", "a file", false);
	HEADER("a file \t2026-01-02 03:04:05 +0000", "a file", false);
	HEADER("file 2026-1-2 3:4:5 +0000", "file", false);
	HEADER("file Fri Jan  2 03:04:05 2026", "file", false);
	HEADER("file Jan 2026 03:04:05", "file", false);
	HEADER("a file \v2026-01-02 03:04:05", "a file", false);
	HEADER("a file \fJan 2026 03:04:05", "a file \fJan 2026 03:04:05",
	       false);
	HEADER("a file \f Jan 2026 03:04:05", "a file \f", false);
	HEADER("a file \r \v2026-01-02 03:04:05", "a file", false);
	HEADER("a file \v \fno date", "a file \v \fno date", false);
	HEADER("file 2026-99-99 03:04:05", "file 2026-99-99 03:04:05", false);
	HEADER("file \t", "file", false);
	HEADER("a file\n", "a", false);
	HEADER("a file\r", "a", false);
	HEADER("\"a\\tfile\"\tdate", "a\tfile", false);
	HEADER("\"a file\"\t2026-01-02 03:04:05 +0000", "a file", false);
	HEADER("\"bad\\q\"", "\"bad\\q\"", true);
#undef HEADER
	expect_header("fileignored", 4, "file", 4, false);
}

static void check_long_header(void)
{
	const size_t repetitions = 1024 * 1024;
	size_t len = 2 + repetitions * 2;
	char *header __autofree = xmalloc(len);

	/* Repeated scans must not make a long whitespace run quadratic */
	memcpy(header, "x\t", 2);
	for (size_t i = 2; i < len; i += 2) {
		header[i] = ' ';
		header[i + 1] = '\v';
	}
	expect_header(header, len, "x", 1, false);
}

static void check_stripping(void)
{
	static const struct {
		const char *name;
		const char *expected;
		int depth;
	} cases[] = { { "a/b/c", "a/b/c", 0 },
		      { "a/b/c", "b/c", 1 },
		      { "a/b/c", "c", 2 },
		      { "a/b/c", "c", 9 },
		      { "a///b//c", "b//c", 1 },
		      { "./a/b", "a/b", 1 },
		      { "a/b\0/c", "b", 2 },
		      { "//a/b", "a/b", 1 },
		      { "file", "file", 4 },
		      { "a/", "", 1 },
		      { "/dev/null", "/dev/null", 9 } };

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++)
		assert(!strcmp(stripped(cases[i].name, cases[i].depth),
			       cases[i].expected));
}

static void check_name_ranking(void)
{
	static const struct {
		const char *old;
		const char *new;
		bool select_new;
	} cases[] = { { "/dev/null", "file", true },
		      { "file", "/dev/null", false },
		      { "/dev/null", "/dev/null", false },
		      { "directory/file", "long-basename", true },
		      { "a/long-basename", "long-directory/f", true },
		      { "long-directory/f", "a/g", true },
		      { "a/f", "a/g", false },
		      { "a///f", "b/g", true } };

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		struct patch_name *old __cleanup(patch_name_pointer_free) =
			unquoted_name(cases[i].old, strlen(cases[i].old));
		struct patch_name *new __cleanup(patch_name_pointer_free) =
			unquoted_name(cases[i].new, strlen(cases[i].new));

		assert(best_patch_name(old, new) ==
		       (cases[i].select_new ? new : old));
	}
}

static void expect_strip_depth(struct cds_list_head lists[2], int depth)
{
	assert(determine_ignore_components(lists, lists + 1) == depth);
	assert(determine_ignore_components(lists + 1, lists) == depth);
}

static void check_strip_depth(void)
{
	static const struct {
		const char *left;
		const char *right;
		int depth;
		enum patch_prefix prefix;
		bool verbatim;
		bool mode_only;
	} cases[] = { { "a/file", "a/file", .depth = 0 },
		      { "a/file", "b/file", .depth = 1 },
		      { "a/dir/file", "b/dir/file", .depth = 1 },
		      { "a/dir/file", "b/other/file", .depth = 2 },
		      { "a/dir/file", "file", .depth = 2 },
		      { "a///file", "b/file", .depth = 1 },
		      { "x/aa/b", "y/aaaa/b", .depth = 2 },
		      { "a//b/c", "d/b/c", .depth = 1 },
		      { "a/dir/", "b/other/", .depth = 2 },
		      { "/dev/null", "a/null", .depth = 0 },
		      { "/dev/null", "/dev/null", .depth = 0 },
		      { "a/file", "b/other", .depth = 0 },
		      { "a/file", "a/file", .depth = 1,
			.prefix = PATCH_PREFIX_GIT },
		      { "a/file", "b/file", .prefix = PATCH_PREFIX_AMBIGUOUS },
		      { "a/file", "b/file", .verbatim = true },
		      { "a/file", "b/file", .mode_only = true } };

	/* A common suffix starts at a complete component on both sides */
	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		struct cds_list_head lists[2];
		struct patch_name names[2] = {
			{ .text = (char *)cases[i].left,
			  .len = strlen(cases[i].left),
			  .prefix = cases[i].prefix,
			  .verbatim = cases[i].verbatim },
			{ .text = (char *)cases[i].right,
			  .len = strlen(cases[i].right) }
		};
		struct file_list files[2] = {};

		for (int leg = 0; leg < 2; leg++) {
			CDS_INIT_LIST_HEAD(&lists[leg]);
			files[leg].file = &names[leg];
			cds_list_add(&files[leg].node, &lists[leg]);
		}

		files[0].mode_only = cases[i].mode_only;
		expect_strip_depth(lists, cases[i].depth);
	}
}

static void check_multi_file_strip_depth(void)
{
	const char *text[2][2] = { { "a/one", "a/x/two" },
				   { "b/one", "b/y/two" } };
	struct cds_list_head lists[2];
	struct patch_name names[2][2] = {};
	struct file_list files[2][2] = {};

	for (int leg = 0; leg < 2; leg++) {
		CDS_INIT_LIST_HEAD(&lists[leg]);
		for (size_t i = 0; i < ARRAY_SIZE(text[leg]); i++) {
			names[leg][i].text = (char *)text[leg][i];
			names[leg][i].len = strlen(text[leg][i]);
			files[leg][i].file = &names[leg][i];
			cds_list_add_tail(&files[leg][i].node, &lists[leg]);
		}
	}

	/* The minimum depth must win in either file order */
	expect_strip_depth(lists, 1);
	for (int leg = 0; leg < 2; leg++) {
		cds_list_del(&files[leg][0].node);
		cds_list_add_tail(&files[leg][0].node, &lists[leg]);
		expect_strip_depth(lists, 1);
	}

	/* Unmatched names cannot lower the depth chosen by a valid pair */
	cds_list_del(&files[0][0].node);
	expect_strip_depth(lists, 2);
	cds_list_del(&files[0][1].node);
	expect_strip_depth(lists, 0);
	cds_list_del(&files[1][0].node);
	cds_list_del(&files[1][1].node);
	expect_strip_depth(lists, 0);
}

struct scope_witness {
	int *count;
	int value;
};

static void scope_witness_free(struct scope_witness *witness)
{
	*witness->count += witness->value;
}

static void leave_scope(int *count, bool early)
{
	struct scope_witness witness __cleanup(
		scope_witness_free) = { .count = count };
	char *text __autofree = memdup("owned", 5);
	char *empty __autofree = NULL;

	/* Both exits must free the buffer and see the updated struct */
	assert(!strcmp(text, "owned"));
	assert(!empty);
	witness.value = 1;
	if (early)
		return;

	witness.value = 2;
}

static void check_array_count(size_t count)
{
	int fixed_array[3], variable_array[count];
	_Static_assert(ARRAY_SIZE(fixed_array) == 3, "constant array size");

	assert(ARRAY_SIZE(variable_array) == count);
}

int main(void)
{
	int count = 0;

	check_headers();
	check_long_header();
	check_stripping();
	check_name_ranking();
	check_strip_depth();
	check_multi_file_strip_depth();
	check_array_count(17);
	leave_scope(&count, true);
	leave_scope(&count, false);
	assert(count == 3);
	puts("reader checks: passed");
	return 0;
}
