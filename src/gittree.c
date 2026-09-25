// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ctrl IQ, Inc.
 *
 * Open one repository for --git-tree, resolve both revisions, and derive their
 * patches. Keep the repository open so the comparison can read complete source
 * files at those same commits.
 */
#include <stdlib.h>
#include <string.h>

#include <assemble.h>
#include <gitread.h>
#include <gittree.h>
#include <iomem.h>
#include <reader.h>
#include <treediff.h>
#include <util.h>

/*
 * Keep the resolved commits available while the comparison reads source files.
 * Resolve once so a ref changing during the run cannot change its inputs.
 */
static struct gittree_run {
	struct gitread *gr;
	struct gitread_oid parent[2];
	struct gitread_oid tip[2];
	bool have_parent[2];
} run;

/*
 * One operand's resolved derivation: what the patch diffs from and to. A plain
 * commit-ish diffs its first parent against itself, with a parentless commit
 * diffing the empty tree, and BASE..TIP diffs BASE against TIP once BASE proves
 * to be one of TIP's parents. Ref names can't carry "..", and hex ids can't
 * carry '.' at all, so any ".." in the spelling unambiguously means the range
 * form.
 */
static void resolve_operand(struct gitread *gr, const char *spec,
			    struct gitread_oid *old, bool *have_old,
			    struct gitread_oid *new)
{
	const char *dots = strstr(spec, "..");

	if (dots) {
		char *base_spec __free(free) = NULL;
		const char *tip_spec = dots + 2;

		if (dots == spec || !tip_spec[0] || tip_spec[0] == '.' ||
		    strstr(tip_spec, ".."))
			die("cannot read %s as BASE..TIP: one \"..\" with a name on each side",
			    spec);

		base_spec = memdup(spec, dots - spec);
		gitread_resolve_commit(gr, base_spec, old);
		gitread_resolve_commit(gr, tip_spec, new);
		if (!gitread_commit_parent_of(gr, new, old))
			die("in %s, %s is not a parent of %s", spec, base_spec,
			    tip_spec);

		*have_old = true;
		return;
	}

	gitread_resolve_commit(gr, spec, new);
	*have_old = gitread_commit_parents(gr, new, old) > 0;
}

void gittree_begin(const char *dir, const char *rev1, const char *rev2,
		   unsigned int context, struct iomem_buf *doc1,
		   struct iomem_buf *doc2)
{
	struct iomem_buf *docs[2] = { doc1, doc2 };
	const char *revs[2] = { rev1, rev2 };

	gitread_open(&run.gr, dir);

	for (int i = 0; i < 2; i++) {
		struct treediff_map map __free(treediff_map) = {};
		struct gitread_oid old = {}, new;
		bool have_old = false;

		resolve_operand(run.gr, revs[i], &old, &have_old, &new);
		run.parent[i] = old;
		run.tip[i] = new;
		run.have_parent[i] = have_old;
		treediff_build(run.gr, have_old ? &old : NULL, &new, &map);
		assemble_patch(run.gr, &map, context, docs[i]);
	}
}

bool gittree_active(void)
{
	return run.gr != NULL;
}

char *gittree_commit_subject(enum gittree_leg leg)
{
	return gitread_commit_subject(run.gr, &run.tip[leg]);
}

bool gittree_source_bytes(enum gittree_leg leg, const char *path, bool result,
			  struct iomem_buf *out)
{
	const struct gitread_oid *commit;
	struct treediff_side side;

	if (!result && !run.have_parent[leg])
		return false;

	commit = result ? &run.tip[leg] : &run.parent[leg];
	if (!gitread_entry_by_path(run.gr, commit, path, &side.oid, &side.mode))
		return false;

	/* Assembly and later source lookup must spell gitlinks identically */
	side.kind = treediff_mode_kind(path, side.mode);
	assemble_side_bytes(run.gr, &side, path, out);
	return true;
}

void gittree_end(void)
{
	gitread_close(&run.gr);
}
