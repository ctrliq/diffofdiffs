// SPDX-License-Identifier: GPL-2.0-only
/* Emit the exact native tree-mode operand for independent source audits */
#include <stdio.h>

#include <assemble.h>
#include <gitread.h>
#include <iomem.h>
#include <treediff.h>
#include <util.h>

int main(int argc, char **argv)
{
	struct gitread_oid parent, tip;
	struct treediff_map map = {};
	struct iomem_buf patch = {};
	struct gitread *reader;
	bool have_parent;

	set_progname("derive-patch");
	if (argc != 3 && argc != 4)
		die("usage: derive-patch DIR REV [PARENT]");

	mem_limit_init_tree();
	gitread_open(&reader, argv[1]);
	gitread_resolve_commit(reader, argv[2], &tip);
	have_parent = gitread_commit_parents(reader, &tip, &parent) > 0;
	if (argc == 4) {
		gitread_resolve_commit(reader, argv[3], &parent);
		if (!gitread_commit_parent_of(reader, &tip, &parent))
			die("requested base is not a parent of the tip");

		have_parent = true;
	}
	treediff_build(reader, have_parent ? &parent : NULL, &tip, &map);
	assemble_patch(reader, &map, 3, &patch);
	fwrite(patch.base, 1, patch.len, stdout);
	iomem_buf_free(&patch);
	treediff_map_free(&map);
	gitread_close(&reader);
	check_output(stdout, "standard output");
	return 0;
}
