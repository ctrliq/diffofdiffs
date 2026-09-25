// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ctrl IQ, Inc.
 *
 * Read and normalize the two operands, then compare their original changes.
 */
#include <errno.h>
#include <git2/global.h>
#include <stdio.h>

#include <cli.h>
#include <gittree.h>
#include <iomem.h>
#include <reader.h>
#include <render.h>
#include <review.h>
#include <util.h>

/*
 * Keep allocation-limit failures on the program's normal error path when
 * running with AddressSanitizer.
 */
const char *__asan_default_options(void);
const char *__asan_default_options(void)
{
	return "allocator_may_return_null=1";
}

int main(int argc, char **argv)
{
	struct review_report report __free(review_report) = {};
	struct iomem_buf b1 = {}, b2 = {};
	const char *display_names[2] = {};
	const char *names[2] = {};
	FILE *out = stdout;
	int ret;

	set_progname("diffofdiffs");

	cli_parse(argc, argv, &names[0], &names[1]);
	for (int leg = 0; leg < 2; leg++)
		display_names[leg] = names[leg];

	/*
	 * The input mode determines the memory bound. Parsing must stay
	 * allocation-free so that the bound still measures the entry footprint.
	 */
	if (git_tree_dir)
		mem_limit_init_tree();
	else
		mem_limit_init();

	/* Line comparison and tree reads share one library lifetime */
	ret = git_libgit2_init();
	if (ret < 0)
		die("cannot initialize libgit2");

	if (git_tree_dir) {
		gittree_begin(git_tree_dir, names[0], names[1], 3, &b1, &b2);
	} else {
		if (!iomem_acquire(&b1, names[0]))
			display_names[0] = NULL;
		if (!iomem_acquire(&b2, names[1]))
			display_names[1] = NULL;
	}

	/* Skip patch validation when neither revision changes a file */
	if (!git_tree_dir || b1.len || b2.len) {
		/*
		 * Validation compacts CRLF input in place. No stored slices or
		 * offsets may refer to the buffers until their bytes settle.
		 */
		validate_patch(&b1, names[0], "patch1");
		validate_patch(&b2, names[1], "patch2");
		review_report_build(&report, &b1, &b2);
	}

	/* Preserve an existing destination if reading or comparison fails */
	if (output_path) {
		out = fopen(output_path, "w");
		if (!out)
			edie(errno, "cannot open %s for output", output_path);
	}
	render_report(out, &report, display_names);

	iomem_buf_free(&b2);
	iomem_buf_free(&b1);
	gittree_end();
	check_output(out, output_path ? output_path : "standard output");
	if (out != stdout && fclose(out))
		edie(errno, "closing %s", output_path);
	git_libgit2_shutdown();

	return 0;
}
