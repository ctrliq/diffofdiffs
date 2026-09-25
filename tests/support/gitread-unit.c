// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ctrl IQ, Inc.
 *
 * A driver for the gitread and treediff seams. Each case builds a synthetic
 * repository in the executor's arena through libgit2's own writers plus plain
 * text writes, then exercises the reader or the differ against it and prints
 * what it observed, so a spec pins the observation bytes. Construction runs on
 * a fixed signature and fixed content, so every printed object id is a
 * deterministic function of the case alone.
 */

/**
 * DOC: why the stores are built here instead of shipping as fixtures
 *
 * A git store is a directory tree, and the executor's arena intake copies plain
 * files alone (a directory can't be a tracked fixture), so a store can't be
 * checked in as bytes. Building it at run time in the arena keeps each case
 * self-contained and exec-free: objects are written through the same library
 * the reader links, and the files git defines as text (loose refs, packed-refs,
 * gitfiles) are written directly.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <git2.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __SANITIZE_ADDRESS__
#include <sanitizer/lsan_interface.h>
#endif

#include <assemble.h>
#include <gitread.h>
#include <iomem.h>
#include <patch-types.h>
#include <treediff.h>
#include <util.h>

/*
 * A death case ends inside a die() with construction state held by dead stack
 * frames alone, and whether the leak checker still counts that state as
 * reachable turns on register allocation, so its end-of-process report can't be
 * pinned and can't be kept out of stderr's pinned bytes either. Disarm the
 * checker for those cases; it stays armed for every case that runs to
 * completion, where a leak would be a real reader defect.
 */
static void quiet_leak_checker(void)
{
#ifdef __SANITIZE_ADDRESS__
	__lsan_disable();
#endif
}

static git_signature *fixed_sig(void)
{
	git_signature *sig = NULL;

	if (git_signature_new(&sig, "t", "t@t.invalid", 0, 0))
		die("cannot build the fixed signature");

	return sig;
}

static void write_text(const char *path, const char *text)
{
	FILE *f;

	f = fopen(path, "w");
	if (!f)
		edie(errno, "cannot create %s", path);

	if (fputs(text, f) == EOF || fclose(f))
		edie(errno, "cannot write %s", path);
}

static void put_blob(git_repository *repo, const char *bytes, size_t len,
		     git_oid *out)
{
	if (git_blob_create_from_buffer(out, repo, bytes, len))
		die("cannot write a blob");
}

/* A one-entry tree: name carrying id at mode */
static void put_tree1(git_repository *repo, const char *name,
		      git_filemode_t mode, const git_oid *id, git_oid *out)
{
	git_treebuilder *tb = NULL;

	if (git_treebuilder_new(&tb, repo, NULL) ||
	    git_treebuilder_insert(NULL, tb, name, id, mode) ||
	    git_treebuilder_write(out, tb))
		die("cannot write a tree");

	git_treebuilder_free(tb);
}

struct tree_spec {
	const char *name;
	git_filemode_t mode;
	const git_oid *id;
};

static void put_commit(git_repository *repo, const char *update_ref,
		       const git_oid *tree, const git_oid *parent,
		       const char *msg, git_oid *out)
{
	git_signature *sig = fixed_sig();
	const git_commit *parents[1];
	git_commit *pc = NULL;
	git_tree *t = NULL;
	git_oid id;
	int ret;

	if (git_tree_lookup(&t, repo, tree))
		die("cannot read a tree back");

	if (parent && git_commit_lookup(&pc, repo, parent))
		die("cannot read a parent back");

	parents[0] = pc;

	ret = git_commit_create(&id, repo, update_ref, sig, sig, NULL, msg, t,
				!!parent, parent ? parents : NULL);
	if (ret)
		die("cannot write a commit");

	git_commit_free(pc);
	git_tree_free(t);
	git_signature_free(sig);

	if (out)
		*out = id;
}

/*
 * The standard two-commit store the cases read: commit A adds f holding
 * "alpha\n", commit B (A's child, the tip) turns f into "beta\n"; refs/heads/x
 * names B as a loose reference and HEAD points at refs/heads/x.
 */
static git_repository *mk_basic(const char *path, bool bare, git_oid *a_out,
				git_oid *b_out)
{
	git_repository *repo = NULL;
	git_oid blob, tree, a, b;

	if (git_repository_init(&repo, path, bare))
		die("cannot initialize a repository at %s", path);

	put_blob(repo, "alpha\n", 6, &blob);
	put_tree1(repo, "f", GIT_FILEMODE_BLOB, &blob, &tree);
	put_commit(repo, NULL, &tree, NULL, "A", &a);
	put_blob(repo, "beta\n", 5, &blob);
	put_tree1(repo, "f", GIT_FILEMODE_BLOB, &blob, &tree);
	put_commit(repo, "refs/heads/x", &tree, &a, "B", &b);

	if (git_repository_set_head(repo, "refs/heads/x"))
		die("cannot point HEAD at refs/heads/x");

	if (a_out)
		*a_out = a;

	if (b_out)
		*b_out = b;

	return repo;
}

static void print_resolved(struct gitread *gr, const char *name)
{
	char hex[GITREAD_OID_HEXSZ + 1];
	struct gitread_oid oid;

	gitread_resolve_commit(gr, name, &oid);
	gitread_oid_hex(&oid, hex);
	printf("%s %s\n", name, hex);
}

/*
 * Every ruled name spelling against one store: the full hex id, a 7-hex prefix,
 * the branch shorthand, the full reference path, and HEAD through its symbolic
 * chain. All five lines print B.
 */
static void case_resolve_forms(void)
{
	char full[GIT_OID_SHA1_HEXSIZE + 1];
	char abbrev[8];
	struct gitread *gr;
	git_oid b;

	git_repository_free(mk_basic("s", false, NULL, &b));
	git_oid_tostr(full, sizeof(full), &b);
	memcpy(abbrev, full, 7);
	abbrev[7] = '\0';

	gitread_open(&gr, "s");
	print_resolved(gr, full);
	print_resolved(gr, abbrev);
	print_resolved(gr, "x");
	print_resolved(gr, "refs/heads/x");
	print_resolved(gr, "HEAD");
	gitread_close(&gr);
}

static void case_resolve_missing(void)
{
	struct gitread *gr;

	quiet_leak_checker();
	git_repository_free(mk_basic("s", false, NULL, NULL));
	gitread_open(&gr, "s");
	print_resolved(gr, "nosuch");
}

static void case_resolve_not_commit(void)
{
	char hex[GIT_OID_SHA1_HEXSIZE + 1];
	git_repository *repo;
	struct gitread *gr;
	git_oid blob;

	quiet_leak_checker();
	repo = mk_basic("s", false, NULL, NULL);
	put_blob(repo, "loose\n", 6, &blob);
	git_repository_free(repo);
	git_oid_tostr(hex, sizeof(hex), &blob);

	gitread_open(&gr, "s");
	print_resolved(gr, hex);
}

/*
 * Writes blobs of distinct one-line contents until two share a 4-hex prefix,
 * then resolves that prefix, which must refuse as ambiguous. The collision
 * point is a pure function of the content sequence, so the case is exactly as
 * deterministic as the passing ones.
 */
static void case_resolve_ambiguous(void)
{
	char (*hexes)[GIT_OID_SHA1_HEXSIZE + 1];
	char prefix[5] = "";
	git_repository *repo;
	struct gitread *gr;
	char content[32];
	git_oid blob;

	quiet_leak_checker();
	repo = mk_basic("s", false, NULL, NULL);
	hexes = xmalloc_array(4096, sizeof(*hexes));

	for (int i = 0; !prefix[0] && i < 4096; i++) {
		snprintf(content, sizeof(content), "filler %d\n", i);
		put_blob(repo, content, strlen(content), &blob);
		git_oid_tostr(hexes[i], sizeof(hexes[i]), &blob);

		for (int j = 0; j < i; j++) {
			if (!memcmp(hexes[j], hexes[i], 4)) {
				memcpy(prefix, hexes[i], 4);
				prefix[4] = '\0';
				break;
			}
		}
	}

	git_repository_free(repo);
	free(hexes);
	if (!prefix[0])
		die("no 4-hex collision in 4096 blobs");

	gitread_open(&gr, "s");
	print_resolved(gr, prefix);
}

/*
 * An annotated tag peels to the commit it names through both accepted
 * spellings: the tag's shorthand rides the reference path and the tag object's
 * own id rides the object-store-first path. Both lines print B.
 */
static void case_resolve_tag(void)
{
	char hex[GIT_OID_SHA1_HEXSIZE + 1];
	git_object *target = NULL;
	git_repository *repo;
	git_signature *sig;
	struct gitread *gr;
	git_oid b, tag;

	repo = mk_basic("s", false, NULL, &b);

	if (git_object_lookup(&target, repo, &b, GIT_OBJECT_COMMIT))
		die("cannot read the tip back");

	sig = fixed_sig();

	if (git_tag_create(&tag, repo, "v1", target, sig, "T", 0))
		die("cannot write the annotated tag");

	git_signature_free(sig);
	git_object_free(target);
	git_repository_free(repo);
	git_oid_tostr(hex, sizeof(hex), &tag);

	gitread_open(&gr, "s");
	print_resolved(gr, "v1");
	print_resolved(gr, hex);
	gitread_close(&gr);
}

/*
 * The loose-before-packed precedence witness: packed-refs maps refs/heads/x at
 * A while the loose file maps it at B, so resolution must print B. The control
 * reopens the store with the loose file removed, and the same name then prints
 * A through packed-refs alone.
 */
static void case_refs_loose_over_packed(void)
{
	char a_hex[GIT_OID_SHA1_HEXSIZE + 1];
	git_repository *repo;
	char *packed = NULL;
	struct gitread *gr;
	char *loose = NULL;
	char *line = NULL;
	git_oid a, b;

	repo = mk_basic("s", false, &a, &b);
	git_oid_tostr(a_hex, sizeof(a_hex), &a);
	xasprintf(&packed, "%spacked-refs", git_repository_path(repo));
	xasprintf(&loose, "%srefs/heads/x", git_repository_path(repo));
	git_repository_free(repo);

	xasprintf(&line, "%s refs/heads/x\n", a_hex);
	write_text(packed, line);
	free(line);

	gitread_open(&gr, "s");
	print_resolved(gr, "x");
	gitread_close(&gr);

	if (unlink(loose))
		edie(errno, "cannot remove the loose reference %s", loose);

	gitread_open(&gr, "s");
	print_resolved(gr, "x");
	gitread_close(&gr);
	free(packed);
	free(loose);
}

/*
 * Every ruled directory form: the working tree itself, a bare store, a gitfile
 * pointing at another repository's .git, and a .git that is a symbolic link.
 * Each open resolves HEAD to prove the store behind it.
 */
static void case_open_forms(void)
{
	struct gitread *gr;

	git_repository_free(mk_basic("w", false, NULL, NULL));
	git_repository_free(mk_basic("b.git", true, NULL, NULL));

	if (mkdir("g", 0777))
		edie(errno, "cannot create g");

	write_text("g/.git", "gitdir: ../w/.git\n");

	if (mkdir("l", 0777))
		edie(errno, "cannot create l");

	if (symlink("../w/.git", "l/.git"))
		edie(errno, "cannot link l/.git");

	gitread_open(&gr, "w");
	print_resolved(gr, "HEAD");
	gitread_close(&gr);
	gitread_open(&gr, "b.git");
	print_resolved(gr, "HEAD");
	gitread_close(&gr);
	gitread_open(&gr, "g");
	print_resolved(gr, "HEAD");
	gitread_close(&gr);
	gitread_open(&gr, "l");
	print_resolved(gr, "HEAD");
	gitread_close(&gr);
}

static void case_open_missing(void)
{
	struct gitread *gr;

	quiet_leak_checker();

	if (mkdir("e", 0777))
		edie(errno, "cannot create e");

	gitread_open(&gr, "e");
}

/* Variant selector for the tree4 family's alpha document */
enum tree4_alpha {
	TREE4_ALPHA_BASE,
	TREE4_ALPHA_UP,
	TREE4_ALPHA_BP,
	TREE4_ALPHA_WILD,
	TREE4_ALPHA_WILD_TIP,
	TREE4_ALPHA_FRAG,
	TREE4_ALPHA_FRAG_TIP
};

/* One row a label file's legs rewrite, spelled per state */
struct label_edit {
	const char *row; /* The row as the root spells it */
	const char *bp; /* The bp leg's spelling, or NULL to keep the root's */
	const char *up; /* The up leg's spelling */
};

/*
 * One quoted row the up leg's base respells, so the two legs' patches quote the
 * same row differently. The up leg carries the respelling as is, and a
 * respelled row is never an edited one.
 */
struct label_respell {
	const char *row; /* The row as the root spells it */
	const char *text; /* The up base's spelling */
};

struct label_file {
	const char *name;
	const char *pair; /* The leg pair whose legs carry the edits */
	const char *base; /* The root text */
	struct label_edit edits[3];
	struct label_respell respells[5];
};

/* The states a label file's blobs are written in, the root's aside */
enum label_state {
	LABEL_BP, /* The bp leg: the root text with the bp edits */
	LABEL_UPBASE, /* The up leg's base: the root text respelled */
	LABEL_UP /* The up leg: the respelled text with the up edits */
};

int main(int argc, char **argv)
{
	set_progname("gitread-unit");

	if (argc < 2) {
		fputs("usage: gitread-unit <case> [args]\n", stderr);
		return 2;
	}

	if (git_libgit2_init() < 0)
		die("cannot initialize libgit2");

	/*
	 * Construction must be as hermetic as the reader: ambient git
	 * configuration would otherwise steer the builders (a default branch
	 * name, a format knob) and make the printed ids machine-dependent.
	 */
	if (git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, GIT_CONFIG_LEVEL_SYSTEM,
			     "") ||
	    git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, GIT_CONFIG_LEVEL_GLOBAL,
			     "") ||
	    git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, GIT_CONFIG_LEVEL_XDG,
			     "") ||
	    git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH,
			     GIT_CONFIG_LEVEL_PROGRAMDATA, ""))
		die("cannot confine construction to the arena");

	if (!strcmp(argv[1], "resolve-forms")) {
		case_resolve_forms();
	} else if (!strcmp(argv[1], "resolve-missing")) {
		case_resolve_missing();
	} else if (!strcmp(argv[1], "resolve-not-commit")) {
		case_resolve_not_commit();
	} else if (!strcmp(argv[1], "resolve-ambiguous")) {
		case_resolve_ambiguous();
	} else if (!strcmp(argv[1], "resolve-tag")) {
		case_resolve_tag();
	} else if (!strcmp(argv[1], "refs-loose-over-packed")) {
		case_refs_loose_over_packed();
	} else if (!strcmp(argv[1], "open-forms")) {
		case_open_forms();
	} else if (!strcmp(argv[1], "open-missing")) {
		case_open_missing();
	} else {
		fprintf(stderr, "unknown case: %s\n", argv[1]);
		return 2;
	}

	return 0;
}
