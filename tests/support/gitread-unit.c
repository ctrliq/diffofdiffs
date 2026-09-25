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

static void write_bytes(const char *path, const void *bytes, size_t len)
{
	FILE *f;

	f = fopen(path, "w");
	if (!f)
		edie(errno, "cannot create %s", path);

	if (fwrite(bytes, 1, len, f) != len || fclose(f))
		edie(errno, "cannot write %s", path);
}

/* An initialized store holding no objects or references of its own */
static void init_plain(const char *path)
{
	git_repository *repo = NULL;

	if (git_repository_init(&repo, path, false))
		die("cannot initialize a repository at %s", path);

	git_repository_free(repo);
}

/* Plants one configuration key in the store under construction */
static void set_config(git_repository *repo, const char *key, const char *val)
{
	git_config *cfg = NULL;

	if (git_repository_config(&cfg, repo) ||
	    git_config_set_string(cfg, key, val))
		die("cannot set %s in the store under construction", key);

	git_config_free(cfg);
}

/*
 * Removes every loose object file so the pack becomes the store's only source.
 * Loose fanout directories carry two-hex names by construction, and removing
 * files alone keeps the store's own layout (info/ and pack/) intact.
 */
static void rm_loose_objects(const char *objdir)
{
	struct dirent *de;
	DIR *od;

	od = opendir(objdir);
	if (!od)
		edie(errno, "cannot enumerate %s", objdir);

	while ((de = readdir(od))) {
		struct dirent *de2;
		char *sub = NULL;
		DIR *sd;

		if (strlen(de->d_name) != 2 || !isxdigit(de->d_name[0]) ||
		    !isxdigit(de->d_name[1]))
			continue;

		xasprintf(&sub, "%s/%s", objdir, de->d_name);
		sd = opendir(sub);
		if (!sd)
			edie(errno, "cannot enumerate %s", sub);

		while ((de2 = readdir(sd))) {
			char *file = NULL;

			if (de2->d_name[0] == '.')
				continue;

			xasprintf(&file, "%s/%s", sub, de2->d_name);
			if (unlink(file))
				edie(errno, "cannot remove %s", file);

			free(file);
		}

		closedir(sd);
		free(sub);
	}

	closedir(od);
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

/* A tree from a spec array, so cases spell multi-entry and nested forms */
static void put_tree(git_repository *repo, const struct tree_spec *ents,
		     size_t n, git_oid *out)
{
	git_treebuilder *tb = NULL;

	if (git_treebuilder_new(&tb, repo, NULL))
		die("cannot open a tree builder");

	for (size_t i = 0; i < n; i++) {
		if (git_treebuilder_insert(NULL, tb, ents[i].name, ents[i].id,
					   ents[i].mode))
			die("cannot insert %s into a tree", ents[i].name);
	}

	if (git_treebuilder_write(out, tb))
		die("cannot write a tree");

	git_treebuilder_free(tb);
}

/*
 * A one-entry tree written as raw object bytes, so the entry can carry a mode
 * spelling the tree builder refuses outright (its filemode validation admits
 * only the canonical constants, while real history also holds forms like
 * 100664). The tree format is the mode in ASCII octal, a space, the entry name,
 * a NUL, then the 20 raw id bytes.
 */
static void put_raw_tree1(git_repository *repo, const char *mode,
			  const char *name, const git_oid *id, git_oid *out)
{
	git_odb *odb = NULL;
	char buf[64];
	size_t n;

	n = (size_t)snprintf(buf, sizeof(buf), "%s %s", mode, name) + 1;
	memcpy(buf + n, id->id, GIT_OID_SHA1_SIZE);
	n += GIT_OID_SHA1_SIZE;

	if (git_repository_odb(&odb, repo) ||
	    git_odb_write(out, odb, buf, n, GIT_OBJECT_TREE))
		die("cannot write a raw tree");

	git_odb_free(odb);
}

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

/*
 * A three-store alternates chain: s3 borrows from s2, s2 from s1, and only s1
 * holds any objects, so each line below proves the reader chased the chain to
 * depth 3. References never cross an alternates boundary, so the spellings here
 * are the two object-id forms.
 */
static void case_alternates_recursive(void)
{
	char full[GIT_OID_SHA1_HEXSIZE + 1];
	struct gitread_oid commit;
	struct iomem_buf buf = {};
	struct gitread *gr;
	char abbrev[8];
	git_oid b;
	u32 mode;

	git_repository_free(mk_basic("s1", false, NULL, &b));
	init_plain("s2");
	init_plain("s3");
	write_text("s2/.git/objects/info/alternates",
		   "../../../s1/.git/objects\n");
	write_text("s3/.git/objects/info/alternates",
		   "../../../s2/.git/objects\n");
	git_oid_tostr(full, sizeof(full), &b);
	memcpy(abbrev, full, 7);
	abbrev[7] = '\0';

	gitread_open(&gr, "s3");
	print_resolved(gr, full);
	print_resolved(gr, abbrev);
	gitread_resolve_commit(gr, full, &commit);

	if (!gitread_blob_by_path(gr, &commit, "f", &buf, &mode))
		die("f is missing through the alternates chain");

	printf("f mode %o len %zu\n", mode, buf.len);
	fwrite(buf.base, 1, buf.len, stdout);
	free(buf.base);
	gitread_close(&gr);
}

/*
 * The packed store: every object mk_basic() wrote moves into one pack and the
 * loose copies are removed, so resolution by reference and by prefix, the
 * parent walk, and the blob read below can only be answered from the pack and
 * its index. Packing at this scale may store the objects whole rather than
 * deltified; this fixture does not establish delta-decoding coverage.
 */
static void case_packed_objects(void)
{
	char full[GIT_OID_SHA1_HEXSIZE + 1];
	char hex[GITREAD_OID_HEXSZ + 1];
	struct gitread_oid commit, parent;
	struct iomem_buf buf = {};
	git_packbuilder *pb = NULL;
	git_repository *repo;
	struct gitread *gr;
	char abbrev[8];
	git_oid a, b;
	u32 mode;
	int n;

	repo = mk_basic("s", false, &a, &b);

	if (git_packbuilder_new(&pb, repo) ||
	    git_packbuilder_insert_commit(pb, &a) ||
	    git_packbuilder_insert_commit(pb, &b) ||
	    git_packbuilder_write(pb, "s/.git/objects/pack", 0, NULL, NULL))
		die("cannot pack the store");

	git_packbuilder_free(pb);
	git_repository_free(repo);
	rm_loose_objects("s/.git/objects");
	git_oid_tostr(full, sizeof(full), &b);
	memcpy(abbrev, full, 7);
	abbrev[7] = '\0';

	gitread_open(&gr, "s");
	print_resolved(gr, "x");
	print_resolved(gr, abbrev);
	gitread_resolve_commit(gr, "x", &commit);
	n = gitread_commit_parents(gr, &commit, &parent);
	gitread_oid_hex(&parent, hex);
	printf("x parents %d first %s\n", n, hex);

	if (!gitread_blob_by_path(gr, &commit, "f", &buf, &mode))
		die("f is missing from the packed store");

	printf("f mode %o len %zu\n", mode, buf.len);
	fwrite(buf.base, 1, buf.len, stdout);
	free(buf.base);
	gitread_close(&gr);
}

/*
 * A pack index the format arithmetic rejects, beside an empty sibling pack:
 * libgit2 alone skips the pair and reads the store as if the pack were absent,
 * so the open must refuse loudly instead.
 */
static void case_garbage_idx(void)
{
	struct gitread *gr;

	quiet_leak_checker();
	git_repository_free(mk_basic("s", false, NULL, NULL));
	write_text("s/.git/objects/pack/pack-junk.idx", "garbage\n");
	write_text("s/.git/objects/pack/pack-junk.pack", "");
	gitread_open(&gr, "s");
}

/* A real index magic naming version 3 refuses by version number at open */
static void case_idx_version(void)
{
	static const u8 head[] = { 0xff, 't', 'O', 'c', 0, 0, 0, 3 };
	struct gitread *gr;

	quiet_leak_checker();
	git_repository_free(mk_basic("s", false, NULL, NULL));
	write_bytes("s/.git/objects/pack/pack-junk.idx", head, sizeof(head));
	gitread_open(&gr, "s");
}

/*
 * The refusal family: each form plants one repository feature the reader must
 * refuse at open, then opens. The v0 forms stay readable to the library, which
 * only enforces extensions at repository format version 1, so the reader's own
 * guards are what fire; the objectformat and formatversion forms are refused by
 * the library itself before any guard runs, and the reader reports its generic
 * open refusal.
 */
static void case_fail_loud(const char *which)
{
	char hex_a[GIT_OID_SHA1_HEXSIZE + 1];
	char hex_b[GIT_OID_SHA1_HEXSIZE + 1];
	git_repository *repo;
	struct gitread *gr;
	char *line = NULL;
	git_oid a, b;

	quiet_leak_checker();
	repo = mk_basic("s", false, &a, &b);
	git_oid_tostr(hex_a, sizeof(hex_a), &a);
	git_oid_tostr(hex_b, sizeof(hex_b), &b);

	if (!strcmp(which, "shallow")) {
		xasprintf(&line, "%s\n", hex_b);
		write_text("s/.git/shallow", line);
	} else if (!strcmp(which, "grafts")) {
		xasprintf(&line, "%s\n", hex_b);
		write_text("s/.git/info/grafts", line);
	} else if (!strcmp(which, "replace")) {
		char *ref = NULL;

		if (mkdir("s/.git/refs/replace", 0777))
			edie(errno, "cannot create refs/replace");

		xasprintf(&ref, "s/.git/refs/replace/%s", hex_b);
		xasprintf(&line, "%s\n", hex_a);
		write_text(ref, line);
		free(ref);
	} else if (!strcmp(which, "refstorage")) {
		set_config(repo, "extensions.refstorage", "reftable");
	} else if (!strcmp(which, "partialclone")) {
		set_config(repo, "extensions.partialclone", "origin");
	} else if (!strcmp(which, "objectformat")) {
		set_config(repo, "core.repositoryformatversion", "1");
		set_config(repo, "extensions.objectformat", "sha256");
	} else if (!strcmp(which, "formatversion")) {
		set_config(repo, "core.repositoryformatversion", "2");
	} else {
		die("unknown refusal form: %s", which);
	}

	free(line);
	git_repository_free(repo);
	gitread_open(&gr, "s");
}

/*
 * The blob read against B: the file itself (mode, length, and bytes print), a
 * path the tree lacks, and a path whose leading component isn't a directory.
 */
static void case_blob_read(void)
{
	char hex[GITREAD_OID_HEXSZ + 1];
	struct gitread_oid commit;
	struct iomem_buf buf = {};
	struct gitread *gr;
	u32 mode;

	git_repository_free(mk_basic("s", false, NULL, NULL));
	gitread_open(&gr, "s");
	gitread_resolve_commit(gr, "x", &commit);
	gitread_oid_hex(&commit, hex);

	if (!gitread_blob_by_path(gr, &commit, "f", &buf, &mode))
		die("f is missing from commit %s", hex);

	printf("f mode %o len %zu\n", mode, buf.len);
	fwrite(buf.base, 1, buf.len, stdout);
	free(buf.base);

	if (!gitread_blob_by_path(gr, &commit, "nosuch", &buf, &mode))
		printf("nosuch absent\n");

	if (!gitread_blob_by_path(gr, &commit, "f/under", &buf, &mode))
		printf("f/under absent\n");

	gitread_close(&gr);
}

/* A symlink entry: the mode prints 120000 and the bytes are the target */
static void case_blob_symlink(void)
{
	struct gitread_oid commit;
	struct iomem_buf buf = {};
	git_repository *repo;
	struct gitread *gr;
	git_oid blob, tree;
	u32 mode;

	repo = mk_basic("s", false, NULL, NULL);
	put_blob(repo, "f", 1, &blob);
	put_tree1(repo, "ln", GIT_FILEMODE_LINK, &blob, &tree);
	put_commit(repo, "refs/heads/ln", &tree, NULL, "L", NULL);
	git_repository_free(repo);

	gitread_open(&gr, "s");
	gitread_resolve_commit(gr, "ln", &commit);

	if (!gitread_blob_by_path(gr, &commit, "ln", &buf, &mode))
		die("ln is missing");

	printf("ln mode %o len %zu bytes %s\n", mode, buf.len, buf.base);
	free(buf.base);
	gitread_close(&gr);
}

/* A gitlink entry refuses: the reader has no file bytes to hand back */
static void case_blob_gitlink(void)
{
	struct gitread_oid commit;
	struct iomem_buf buf = {};
	git_repository *repo;
	struct gitread *gr;
	git_oid sub, tree;
	u32 mode;

	quiet_leak_checker();
	repo = mk_basic("s", false, NULL, &sub);
	put_tree1(repo, "mod", GIT_FILEMODE_COMMIT, &sub, &tree);
	put_commit(repo, "refs/heads/g", &tree, NULL, "G", NULL);
	git_repository_free(repo);

	gitread_open(&gr, "s");
	gitread_resolve_commit(gr, "g", &commit);
	gitread_blob_by_path(gr, &commit, "mod", &buf, &mode);
}

/*
 * A blob one byte past the engine's per-image payload ceiling refuses at the
 * read, before the oversized image can reach the engine's own refusal. Zeros
 * keep the loose object tiny on disk; the recorded size carries the excess.
 */
static void case_blob_over_ceiling(void)
{
	struct gitread_oid commit;
	struct iomem_buf buf = {};
	git_repository *repo;
	struct gitread *gr;
	git_oid blob, tree;
	char *bytes;
	u32 mode;

	quiet_leak_checker();
	repo = mk_basic("s", false, NULL, NULL);
	bytes = xzalloc(PAYLOAD_CEILING + 1);
	put_blob(repo, bytes, PAYLOAD_CEILING + 1, &blob);
	free(bytes);
	put_tree1(repo, "big", GIT_FILEMODE_BLOB, &blob, &tree);
	put_commit(repo, "refs/heads/big", &tree, NULL, "big", NULL);
	git_repository_free(repo);

	gitread_open(&gr, "s");
	gitread_resolve_commit(gr, "big", &commit);
	gitread_blob_by_path(gr, &commit, "big", &buf, &mode);
}

/*
 * Parent walks: the root prints 0, the tip prints 1 with A as its first parent,
 * and a merge of the tip and the root prints 2 with the tip first.
 */
static void case_parents(void)
{
	static const char *const names[] = { "x~", "x", "m" };
	struct gitread_oid commit, parent;
	char hex[GITREAD_OID_HEXSZ + 1];
	const git_commit *parents[2];
	git_commit *ac = NULL;
	git_commit *bc = NULL;
	git_repository *repo;
	git_signature *sig;
	struct gitread *gr;
	git_tree *t = NULL;
	git_oid a, b, m;

	repo = mk_basic("s", false, &a, &b);

	if (git_commit_lookup(&bc, repo, &b) ||
	    git_commit_lookup(&ac, repo, &a) || git_commit_tree(&t, bc))
		die("cannot read the two commits back");

	parents[0] = bc;
	parents[1] = ac;
	sig = fixed_sig();

	if (git_commit_create(&m, repo, "refs/heads/m", sig, sig, NULL, "M", t,
			      2, parents))
		die("cannot write the merge commit");

	git_signature_free(sig);
	git_tree_free(t);
	git_commit_free(ac);
	git_commit_free(bc);
	git_repository_free(repo);

	gitread_open(&gr, "s");

	for (size_t i = 0; i < ARRAY_SIZE(names); i++) {
		const char *name = names[i];
		int n;

		/*
		 * "x~" isn't reader grammar; the root is reached as the tip's
		 * first parent through the reader's own walk instead.
		 */
		if (!strcmp(name, "x~")) {
			gitread_resolve_commit(gr, "x", &commit);
			gitread_commit_parents(gr, &commit, &parent);
			commit = parent;
		} else {
			gitread_resolve_commit(gr, name, &commit);
		}

		n = gitread_commit_parents(gr, &commit, &parent);
		printf("%s parents %d", name, n);

		if (n > 0) {
			gitread_oid_hex(&parent, hex);
			printf(" first %s", hex);
		}

		printf("\n");
	}

	gitread_close(&gr);
}

static const char *kind_name(enum treediff_kind kind)
{
	switch (kind) {
	case TREEDIFF_REG:
		return "reg";

	case TREEDIFF_SYMLINK:
		return "symlink";

	case TREEDIFF_GITLINK:
		return "gitlink";
	}

	die("no such kind: %d", kind);
}

/*
 * One line per map record: the operation, the path, then each present side's
 * kind, raw octal mode, and object id. A modify prints its kind once, since
 * differing kinds record as a delete and a create instead.
 */
static void print_map(const struct treediff_map *map)
{
	char hex[GITREAD_OID_HEXSZ + 1];

	printf("map %zu\n", map->n);

	for (size_t i = 0; i < map->n; i++) {
		const struct treediff_rec *rec = &map->recs[i];

		switch (rec->op) {
		case TREEDIFF_CREATE:
			gitread_oid_hex(&rec->new.oid, hex);
			printf("create %s %s %o %s", rec->path,
			       kind_name(rec->new.kind), rec->new.mode, hex);
			break;
		case TREEDIFF_DELETE:
			gitread_oid_hex(&rec->old.oid, hex);
			printf("delete %s %s %o %s", rec->path,
			       kind_name(rec->old.kind), rec->old.mode, hex);
			break;
		case TREEDIFF_MODIFY:
			gitread_oid_hex(&rec->old.oid, hex);
			printf("modify %s %s %o %s", rec->path,
			       kind_name(rec->old.kind), rec->old.mode, hex);
			gitread_oid_hex(&rec->new.oid, hex);
			printf(" -> %o %s", rec->new.mode, hex);
			break;
		}

		printf("\n");
	}
}

/*
 * Diffs old_name's commit against new_name's in the store at dir and prints the
 * map; a null old_name exercises the empty-tree side (the differ's
 * parentless-commit form).
 */
static void diff_refs(const char *dir, const char *old_name,
		      const char *new_name)
{
	struct gitread_oid old_id, new_id;
	struct treediff_map map;
	struct gitread *gr;

	gitread_open(&gr, dir);
	gitread_resolve_commit(gr, new_name, &new_id);

	if (old_name) {
		gitread_resolve_commit(gr, old_name, &old_id);
		treediff_build(gr, &old_id, &new_id, &map);
	} else {
		treediff_build(gr, NULL, &new_id, &map);
	}

	print_map(&map);
	treediff_map_free(&map);
	gitread_close(&gr);
}

/* The two commits every diff case reads, on refs/heads/o and refs/heads/n */
static void put_pair(git_repository *repo, const struct tree_spec *old,
		     size_t nold, const struct tree_spec *new, size_t nnew)
{
	git_oid tree;

	put_tree(repo, old, nold, &tree);
	put_commit(repo, "refs/heads/o", &tree, NULL, "O", NULL);
	put_tree(repo, new, nnew, &tree);
	put_commit(repo, "refs/heads/n", &tree, NULL, "N", NULL);
}

static git_repository *init_repo(const char *path)
{
	git_repository *repo = NULL;

	if (git_repository_init(&repo, path, false))
		die("cannot initialize a repository at %s", path);

	return repo;
}

/*
 * The four ordinary operations against one tree pair: a content change, a
 * creation, a deletion, and a mode-only flip, with an unchanged file present to
 * prove unchanged paths stay out of the map.
 */
static void case_treediff_ops(void)
{
	git_oid a1, a2, keep, gone, fresh, flip;
	const struct tree_spec old[] = { { "a", GIT_FILEMODE_BLOB, &a1 },
					 { "d", GIT_FILEMODE_BLOB, &gone },
					 { "keep", GIT_FILEMODE_BLOB, &keep },
					 { "m", GIT_FILEMODE_BLOB, &flip } };
	const struct tree_spec new[] = { { "a", GIT_FILEMODE_BLOB, &a2 },
					 { "c", GIT_FILEMODE_BLOB, &fresh },
					 { "keep", GIT_FILEMODE_BLOB, &keep },
					 { "m", GIT_FILEMODE_BLOB_EXECUTABLE,
					   &flip } };
	git_repository *repo;

	repo = init_repo("s");
	put_blob(repo, "a1\n", 3, &a1);
	put_blob(repo, "a2\n", 3, &a2);
	put_blob(repo, "keep\n", 5, &keep);
	put_blob(repo, "gone\n", 5, &gone);
	put_blob(repo, "fresh\n", 6, &fresh);
	put_blob(repo, "flip\n", 5, &flip);

	put_pair(repo, old, ARRAY_SIZE(old), new, ARRAY_SIZE(new));
	git_repository_free(repo);
	diff_refs("s", "o", "n");
}

/*
 * Nested directories on both sides, plus the ordering the map guarantees:
 * path-byte order, which puts a.c before a/x before a0 (the dot, the slash, and
 * the digit sort by their bytes) and keeps the b/y records depth-first.
 */
static void case_treediff_order(void)
{
	git_oid v1, v2, w, z, dot, zero, ao, yo, bo, an, yn, bn;
	const struct tree_spec new[] = { { "a.c", GIT_FILEMODE_BLOB, &dot },
					 { "a", GIT_FILEMODE_TREE, &an },
					 { "a0", GIT_FILEMODE_BLOB, &zero },
					 { "b", GIT_FILEMODE_TREE, &bn } };
	const struct tree_spec old[] = { { "a", GIT_FILEMODE_TREE, &ao },
					 { "b", GIT_FILEMODE_TREE, &bo } };
	git_repository *repo;

	repo = init_repo("s");
	put_blob(repo, "v1\n", 3, &v1);
	put_blob(repo, "v2\n", 3, &v2);
	put_blob(repo, "w\n", 2, &w);
	put_blob(repo, "z\n", 2, &z);
	put_blob(repo, "dot\n", 4, &dot);
	put_blob(repo, "zero\n", 5, &zero);

	put_tree1(repo, "x", GIT_FILEMODE_BLOB, &v1, &ao);
	put_tree1(repo, "w", GIT_FILEMODE_BLOB, &w, &yo);
	put_tree1(repo, "y", GIT_FILEMODE_TREE, &yo, &bo);
	put_tree1(repo, "x", GIT_FILEMODE_BLOB, &v2, &an);
	put_tree1(repo, "z", GIT_FILEMODE_BLOB, &z, &yn);
	put_tree1(repo, "y", GIT_FILEMODE_TREE, &yn, &bn);

	put_pair(repo, old, ARRAY_SIZE(old), new, ARRAY_SIZE(new));
	git_repository_free(repo);
	diff_refs("s", "o", "n");
}

/*
 * Symlink and gitlink records: a retargeted symlink and a bumped gitlink are
 * modifies of their kinds, and a new symlink is a create. The gitlink targets
 * are two throwaway commits, so their printed ids stay deterministic.
 */
static void case_treediff_kinds(void)
{
	git_oid t1, t2, t3, t, tg, g1, g2;
	const struct tree_spec old[] = { { "ln", GIT_FILEMODE_LINK, &t1 },
					 { "mod", GIT_FILEMODE_COMMIT, &g1 } };
	const struct tree_spec new[] = { { "ln", GIT_FILEMODE_LINK, &t2 },
					 { "ln2", GIT_FILEMODE_LINK, &t3 },
					 { "mod", GIT_FILEMODE_COMMIT, &g2 } };
	git_repository *repo;

	repo = init_repo("s");
	put_blob(repo, "t1", 2, &t1);
	put_blob(repo, "t2", 2, &t2);
	put_blob(repo, "t3", 2, &t3);
	put_blob(repo, "t\n", 2, &t);
	put_tree1(repo, "f", GIT_FILEMODE_BLOB, &t, &tg);
	put_commit(repo, NULL, &tg, NULL, "G1", &g1);
	put_commit(repo, NULL, &tg, NULL, "G2", &g2);

	put_pair(repo, old, ARRAY_SIZE(old), new, ARRAY_SIZE(new));
	git_repository_free(repo);
	diff_refs("s", "o", "n");
}

/*
 * A kind change at one path records as a delete and a create at that path, the
 * delete first: the ruled delete-plus-create form for type changes, with no
 * merged record.
 */
static void case_treediff_typechange(void)
{
	git_repository *repo;
	git_oid data, tgt;
	const struct tree_spec old[] = { { "f", GIT_FILEMODE_BLOB, &data } };
	const struct tree_spec new[] = { { "f", GIT_FILEMODE_LINK, &tgt } };

	repo = init_repo("s");
	put_blob(repo, "data\n", 5, &data);
	put_blob(repo, "elsewhere", 9, &tgt);

	put_pair(repo, old, ARRAY_SIZE(old), new, ARRAY_SIZE(new));
	git_repository_free(repo);
	diff_refs("s", "o", "n");
}

/*
 * Moving foo's regular-file content to bar while replacing foo with a symlink
 * needs three records: create bar, delete the old foo, and create the new foo.
 * Keeping the same path must not merge entries with different kinds.
 */
static void case_treediff_typechange_edge(void)
{
	git_repository *repo;
	git_oid moved, tgt;
	const struct tree_spec old[] = { { "foo", GIT_FILEMODE_BLOB, &moved } };
	const struct tree_spec new[] = { { "bar", GIT_FILEMODE_BLOB, &moved },
					 { "foo", GIT_FILEMODE_LINK, &tgt } };

	repo = init_repo("s");
	put_blob(repo, "moved\n", 6, &moved);
	put_blob(repo, "bar", 3, &tgt);

	put_pair(repo, old, ARRAY_SIZE(old), new, ARRAY_SIZE(new));
	git_repository_free(repo);
	diff_refs("s", "o", "n");
}

/*
 * Each rename remains a delete and a create sharing one object. The second pair
 * also changes permissions, which must survive independently of content.
 */
static void case_treediff_rename(void)
{
	git_repository *repo;
	git_oid r1, r2;
	const struct tree_spec new[] = { { "b", GIT_FILEMODE_BLOB, &r1 },
					 { "d", GIT_FILEMODE_BLOB_EXECUTABLE,
					   &r2 } };
	const struct tree_spec old[] = { { "a", GIT_FILEMODE_BLOB, &r1 },
					 { "c", GIT_FILEMODE_BLOB, &r2 } };

	repo = init_repo("s");
	put_blob(repo, "r1\n", 3, &r1);
	put_blob(repo, "r2\n", 3, &r2);

	put_pair(repo, old, ARRAY_SIZE(old), new, ARRAY_SIZE(new));
	git_repository_free(repo);
	diff_refs("s", "o", "n");
}

/*
 * Retain every path when one deleted object reappears at two created paths, and
 * when two deleted copies reappear at one path. Equal object IDs must not
 * coalesce distinct file operations.
 */
static void case_treediff_rename_ambiguous(void)
{
	git_repository *repo;
	git_oid p, q;
	const struct tree_spec old[] = { { "x1", GIT_FILEMODE_BLOB, &p },
					 { "z1", GIT_FILEMODE_BLOB, &q },
					 { "z2", GIT_FILEMODE_BLOB, &q } };
	const struct tree_spec new[] = { { "w", GIT_FILEMODE_BLOB, &q },
					 { "y1", GIT_FILEMODE_BLOB, &p },
					 { "y2", GIT_FILEMODE_BLOB, &p } };

	repo = init_repo("s");
	put_blob(repo, "p\n", 2, &p);
	put_blob(repo, "q\n", 2, &q);

	put_pair(repo, old, ARRAY_SIZE(old), new, ARRAY_SIZE(new));
	git_repository_free(repo);
	diff_refs("s", "o", "n");
}

/*
 * A deleted regular file and a created symlink can share one blob (the file's
 * content equals the link's target bytes), but their records must retain
 * distinct kinds even when the object IDs match.
 */
static void case_treediff_kind_mismatch_edge(void)
{
	git_repository *repo;
	git_oid tgt;
	const struct tree_spec new[] = { { "ln", GIT_FILEMODE_LINK, &tgt } };
	const struct tree_spec old[] = { { "f", GIT_FILEMODE_BLOB, &tgt } };

	repo = init_repo("s");
	put_blob(repo, "tgt", 3, &tgt);

	put_pair(repo, old, ARRAY_SIZE(old), new, ARRAY_SIZE(new));
	git_repository_free(repo);
	diff_refs("s", "o", "n");
}

/*
 * The empty-tree side: with no old commit at all, every entry of the new
 * commit's tree records as a create. This pins the differ's contract alone;
 * what a parentless commit means for the CLI is a later stage's decision.
 */
static void case_treediff_root(void)
{
	git_repository *repo;
	git_oid xb, fb, sub;
	const struct tree_spec new[] = { { "d", GIT_FILEMODE_TREE, &sub },
					 { "f", GIT_FILEMODE_BLOB, &fb } };
	git_oid tree;

	repo = init_repo("s");
	put_blob(repo, "x\n", 2, &xb);
	put_blob(repo, "alpha\n", 6, &fb);
	put_tree1(repo, "x", GIT_FILEMODE_BLOB, &xb, &sub);

	put_tree(repo, new, ARRAY_SIZE(new), &tree);
	put_commit(repo, "refs/heads/n", &tree, NULL, "N", NULL);
	git_repository_free(repo);
	diff_refs("s", NULL, "n");
}

/* Identical trees diff to an empty map; the header line still prints */
static void case_treediff_identical(void)
{
	git_repository_free(mk_basic("s", false, NULL, NULL));
	diff_refs("s", "x", "x");
}

/*
 * Historical permission bits such as 100664 still classify a regular file. The
 * reader keeps the raw spelling, so a permission-only difference against the
 * canonical 100644 surfaces as a modify.
 */
static void case_treediff_rawmode(void)
{
	git_oid alpha, tree;
	git_repository *repo;

	repo = init_repo("s");
	put_blob(repo, "alpha\n", 6, &alpha);
	put_tree1(repo, "f", GIT_FILEMODE_BLOB, &alpha, &tree);
	put_commit(repo, "refs/heads/o", &tree, NULL, "O", NULL);
	put_raw_tree1(repo, "100664", "f", &alpha, &tree);
	put_commit(repo, "refs/heads/n", &tree, NULL, "N", NULL);
	git_repository_free(repo);
	diff_refs("s", "o", "n");
}

/*
 * A tree entry whose mode sits outside all four type families parses in the
 * library (the entry's type normalizes to a blob) but has no meaning the map
 * can carry, so the differ refuses it by number rather than guessing a kind.
 */
static void case_treediff_alien_mode(void)
{
	git_oid alpha, tree;
	git_repository *repo;

	quiet_leak_checker();
	repo = init_repo("s");
	put_blob(repo, "alpha\n", 6, &alpha);
	put_tree1(repo, "g", GIT_FILEMODE_BLOB, &alpha, &tree);
	put_commit(repo, "refs/heads/o", &tree, NULL, "O", NULL);
	put_raw_tree1(repo, "110000", "f", &alpha, &tree);
	put_commit(repo, "refs/heads/n", &tree, NULL, "N", NULL);
	git_repository_free(repo);
	diff_refs("s", "o", "n");
}

/*
 * A tree naming a subtree the store never held parses fine on its own, and the
 * walk's descent is that object's first reader, so the enumeration must die on
 * the missing tree rather than hand back a partial map.
 */
static void case_treediff_ghost_subtree(void)
{
	git_oid alpha, ghost, tree;
	git_repository *repo;
	git_oid sub = {};

	quiet_leak_checker();
	repo = init_repo("s");
	put_blob(repo, "alpha\n", 6, &alpha);
	put_tree1(repo, "g", GIT_FILEMODE_BLOB, &alpha, &tree);
	put_commit(repo, "refs/heads/o", &tree, NULL, "O", NULL);
	memset(sub.id, 0x42, GIT_OID_SHA1_SIZE);
	put_raw_tree1(repo, "40000", "d", &sub, &ghost);
	put_commit(repo, "refs/heads/n", &ghost, NULL, "N", NULL);
	git_repository_free(repo);
	diff_refs("s", "o", "n");
}

/*
 * A file becoming a directory: the union holds foo and foo/x as distinct paths
 * (a tree cannot hold both at once, but the two sides can), so the map records
 * a delete and a create with no shared-path pair involved.
 */
static void case_treediff_dir_boundary(void)
{
	git_oid k, fdata, xdata, sub;
	const struct tree_spec old[] = { { "anchor", GIT_FILEMODE_BLOB, &k },
					 { "foo", GIT_FILEMODE_BLOB, &fdata } };
	const struct tree_spec new[] = { { "anchor", GIT_FILEMODE_BLOB, &k },
					 { "foo", GIT_FILEMODE_TREE, &sub } };
	git_repository *repo;

	repo = init_repo("s");
	put_blob(repo, "k\n", 2, &k);
	put_blob(repo, "f\n", 2, &fdata);
	put_blob(repo, "x\n", 2, &xdata);
	put_tree1(repo, "x", GIT_FILEMODE_BLOB, &xdata, &sub);

	put_pair(repo, old, ARRAY_SIZE(old), new, ARRAY_SIZE(new));
	git_repository_free(repo);
	diff_refs("s", "o", "n");
}

/*
 * Diffs old_name's commit against new_name's in the store at dir, renders the
 * map through the assembler at the given context width and prints the document
 * bytes, which the fixture pins as the golden patch.
 */
static void assemble_refs(const char *dir, const char *old_name,
			  const char *new_name, unsigned int context)
{
	struct gitread_oid old_id, new_id;
	struct treediff_map map;
	struct iomem_buf doc;
	struct gitread *gr;

	gitread_open(&gr, dir);
	gitread_resolve_commit(gr, old_name, &old_id);
	gitread_resolve_commit(gr, new_name, &new_id);
	treediff_build(gr, &old_id, &new_id, &map);
	assemble_patch(gr, &map, context, &doc);
	fwrite(doc.base, 1, doc.len, stdout);
	iomem_buf_free(&doc);
	treediff_map_free(&map);
	gitread_close(&gr);
}

/*
 * The three ordinary operations rendered as one document: a content change, a
 * creation, and a deletion, with an unchanged file proving unchanged paths
 * yield no block. The golden pins the header forms, the /dev/null halves, and
 * the path-ordered file blocks.
 */
static void case_assemble_ops(void)
{
	git_oid a1, a2, keep, gone, fresh;
	const struct tree_spec old[] = { { "a", GIT_FILEMODE_BLOB, &a1 },
					 { "d", GIT_FILEMODE_BLOB, &gone },
					 { "keep", GIT_FILEMODE_BLOB, &keep } };
	const struct tree_spec new[] = { { "a", GIT_FILEMODE_BLOB, &a2 },
					 { "c", GIT_FILEMODE_BLOB, &fresh },
					 { "keep", GIT_FILEMODE_BLOB, &keep } };
	git_repository *repo;

	repo = init_repo("s");
	put_blob(repo, "one\ntwo\nthree\nfour\n", 19, &a1);
	put_blob(repo, "one\nTWO\nthree\nfour\n", 19, &a2);
	put_blob(repo, "keep\n", 5, &keep);
	put_blob(repo, "gone\n", 5, &gone);
	put_blob(repo, "fresh\n", 6, &fresh);

	put_pair(repo, old, ARRAY_SIZE(old), new, ARRAY_SIZE(new));
	git_repository_free(repo);
	assemble_refs("s", "o", "n", 3);
}

/* A mode flip over one unchanged blob: the hunkless fileop-only block */
static void case_assemble_mode_only(void)
{
	git_repository *repo;
	git_oid flip;
	const struct tree_spec old[] = { { "m", GIT_FILEMODE_BLOB, &flip } };
	const struct tree_spec new[] = { { "m", GIT_FILEMODE_BLOB_EXECUTABLE,
					   &flip } };

	repo = init_repo("s");
	put_blob(repo, "flip\n", 5, &flip);

	put_pair(repo, old, ARRAY_SIZE(old), new, ARRAY_SIZE(new));
	git_repository_free(repo);
	assemble_refs("s", "o", "n", 3);
}

/*
 * A mode flip landing together with a content change carries the old/new mode
 * pair above the header lines, and a creation born executable carries its own
 * mode in the creation form.
 */
static void case_assemble_mode_content(void)
{
	git_repository *repo;
	git_oid mc1, mc2, x;
	const struct tree_spec old[] = { { "mc", GIT_FILEMODE_BLOB, &mc1 } };
	const struct tree_spec new[] = {
		{ "mc", GIT_FILEMODE_BLOB_EXECUTABLE, &mc2 },
		{ "x", GIT_FILEMODE_BLOB_EXECUTABLE, &x }
	};

	repo = init_repo("s");
	put_blob(repo, "run\nme\n", 7, &mc1);
	put_blob(repo, "run\nME\n", 7, &mc2);
	put_blob(repo, "#!/bin/sh\n", 10, &x);

	put_pair(repo, old, ARRAY_SIZE(old), new, ARRAY_SIZE(new));
	git_repository_free(repo);
	assemble_refs("s", "o", "n", 3);
}

/*
 * Creating and deleting empty files: both sides of the content diff are empty,
 * so each record ends hunk-less, its mode word completed by the full-width
 * index line that keeps the block appliable under the intake's completeness
 * rule.
 */
static void case_assemble_empty_files(void)
{
	git_repository *repo;
	git_oid empty;
	const struct tree_spec old[] = { { "g", GIT_FILEMODE_BLOB, &empty } };
	const struct tree_spec new[] = { { "e", GIT_FILEMODE_BLOB, &empty } };

	repo = init_repo("s");
	put_blob(repo, "", 0, &empty);

	put_pair(repo, old, ARRAY_SIZE(old), new, ARRAY_SIZE(new));
	git_repository_free(repo);
	assemble_refs("s", "o", "n", 3);
}

/*
 * Symlinks diff as their target-path bytes: a creation and a retarget, each
 * hunk row carrying the no-newline marker, since a symlink blob never ends in a
 * line feed.
 */
static void case_assemble_symlink(void)
{
	git_repository *repo;
	git_oid t1, t2, t3;
	const struct tree_spec old[] = { { "lo", GIT_FILEMODE_LINK, &t1 } };
	const struct tree_spec new[] = { { "ln", GIT_FILEMODE_LINK, &t3 },
					 { "lo", GIT_FILEMODE_LINK, &t2 } };

	repo = init_repo("s");
	put_blob(repo, "old", 3, &t1);
	put_blob(repo, "new", 3, &t2);
	put_blob(repo, "target", 6, &t3);

	put_pair(repo, old, ARRAY_SIZE(old), new, ARRAY_SIZE(new));
	git_repository_free(repo);
	assemble_refs("s", "o", "n", 3);
}

/*
 * Gitlinks render as the Subproject-commit scalar line git prints for a
 * submodule, spelled from the map's object id alone: a creation and a pointer
 * bump. The referenced commits exist here; missing submodule objects are
 * exercised separately by the complete-tree checks.
 */
static void case_assemble_gitlink(void)
{
	git_oid t, tg, g1, g2;
	const struct tree_spec old[] = { { "sub2", GIT_FILEMODE_COMMIT, &g1 } };
	const struct tree_spec new[] = { { "sub", GIT_FILEMODE_COMMIT, &g1 },
					 { "sub2", GIT_FILEMODE_COMMIT, &g2 } };
	git_repository *repo;

	repo = init_repo("s");
	put_blob(repo, "t\n", 2, &t);
	put_tree1(repo, "f", GIT_FILEMODE_BLOB, &t, &tg);
	put_commit(repo, NULL, &tg, NULL, "G1", &g1);
	put_commit(repo, NULL, &tg, NULL, "G2", &g2);

	put_pair(repo, old, ARRAY_SIZE(old), new, ARRAY_SIZE(new));
	git_repository_free(repo);
	assemble_refs("s", "o", "n", 3);
}

/*
 * A kind change at one path renders as its delete block followed by its create
 * block, the ruled delete-plus-create form, each block carrying its own side's
 * mode and content.
 */
static void case_assemble_typechange(void)
{
	git_repository *repo;
	git_oid reg, lnk;
	const struct tree_spec old[] = { { "t", GIT_FILEMODE_BLOB, &reg } };
	const struct tree_spec new[] = { { "t", GIT_FILEMODE_LINK, &lnk } };

	repo = init_repo("s");
	put_blob(repo, "data\n", 5, &reg);
	put_blob(repo, "data", 4, &lnk);

	put_pair(repo, old, ARRAY_SIZE(old), new, ARRAY_SIZE(new));
	git_repository_free(repo);
	assemble_refs("s", "o", "n", 3);
}

/*
 * A moved file and a copied file render with no rename or copy headers: the
 * move is its delete block and its create block with the content spelled in
 * full both times, and the copy is a plain creation beside its surviving
 * source.
 */
static void case_assemble_rename_copy(void)
{
	git_repository *repo;
	git_oid body, same;
	const struct tree_spec old[] = { { "from", GIT_FILEMODE_BLOB, &body },
					 { "srcf", GIT_FILEMODE_BLOB, &same } };
	const struct tree_spec new[] = { { "copy", GIT_FILEMODE_BLOB, &same },
					 { "srcf", GIT_FILEMODE_BLOB, &same },
					 { "to", GIT_FILEMODE_BLOB, &body } };

	repo = init_repo("s");
	put_blob(repo, "body\n", 5, &body);
	put_blob(repo, "same\n", 5, &same);

	put_pair(repo, old, ARRAY_SIZE(old), new, ARRAY_SIZE(new));
	git_repository_free(repo);
	assemble_refs("s", "o", "n", 3);
}

/*
 * The context width threads through to the engine: the same change prints one
 * context line per side instead of the default three.
 */
static void case_assemble_width(void)
{
	git_repository *repo;
	git_oid w1, w2;
	const struct tree_spec old[] = { { "w", GIT_FILEMODE_BLOB, &w1 } };
	const struct tree_spec new[] = { { "w", GIT_FILEMODE_BLOB, &w2 } };

	repo = init_repo("s");
	put_blob(repo, "l1\nl2\nl3\nl4\nl5\nl6\nl7\nl8\nl9\n", 27, &w1);
	put_blob(repo, "l1\nl2\nl3\nl4\nL5\nl6\nl7\nl8\nl9\n", 27, &w2);

	put_pair(repo, old, ARRAY_SIZE(old), new, ARRAY_SIZE(new));
	git_repository_free(repo);
	assemble_refs("s", "o", "n", 1);
}

/*
 * A final line gaining its line feed: the old side's short last line makes the
 * engine print the no-newline marker under the removed row and none under the
 * added one.
 */
static void case_assemble_nonewline(void)
{
	git_repository *repo;
	git_oid n1, n2;
	const struct tree_spec old[] = { { "n", GIT_FILEMODE_BLOB, &n1 } };
	const struct tree_spec new[] = { { "n", GIT_FILEMODE_BLOB, &n2 } };

	repo = init_repo("s");
	put_blob(repo, "alpha\nbeta", 10, &n1);
	put_blob(repo, "alpha\nbeta\n", 11, &n2);

	put_pair(repo, old, ARRAY_SIZE(old), new, ARRAY_SIZE(new));
	git_repository_free(repo);
	assemble_refs("s", "o", "n", 3);
}

/* Identical trees yield the empty document, the identity patch */
static void case_assemble_identical(void)
{
	git_repository *repo;
	git_oid k;
	const struct tree_spec ents[] = { { "k", GIT_FILEMODE_BLOB, &k } };

	repo = init_repo("s");
	put_blob(repo, "k\n", 2, &k);

	put_pair(repo, ents, ARRAY_SIZE(ents), ents, ARRAY_SIZE(ents));
	git_repository_free(repo);
	assemble_refs("s", "o", "n", 3);
}

/*
 * A blob holding a NUL byte is binary content no unified diff can carry, so the
 * assembler refuses it loudly instead of feeding it to the engine.
 */
static void case_assemble_binary(void)
{
	git_repository *repo;
	git_oid b1, b2;
	const struct tree_spec old[] = { { "b", GIT_FILEMODE_BLOB, &b1 } };
	const struct tree_spec new[] = { { "b", GIT_FILEMODE_BLOB, &b2 } };

	quiet_leak_checker();
	repo = init_repo("s");
	put_blob(repo, "bin\0ary\n", 8, &b1);
	put_blob(repo, "text\n", 5, &b2);

	put_pair(repo, old, ARRAY_SIZE(old), new, ARRAY_SIZE(new));
	git_repository_free(repo);
	assemble_refs("s", "o", "n", 3);
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
	} else if (!strcmp(argv[1], "alternates-recursive")) {
		case_alternates_recursive();
	} else if (!strcmp(argv[1], "packed-objects")) {
		case_packed_objects();
	} else if (!strcmp(argv[1], "garbage-idx")) {
		case_garbage_idx();
	} else if (!strcmp(argv[1], "idx-version")) {
		case_idx_version();
	} else if (!strncmp(argv[1], "fail-", strlen("fail-"))) {
		case_fail_loud(argv[1] + strlen("fail-"));
	} else if (!strcmp(argv[1], "blob-read")) {
		case_blob_read();
	} else if (!strcmp(argv[1], "blob-symlink")) {
		case_blob_symlink();
	} else if (!strcmp(argv[1], "blob-gitlink")) {
		case_blob_gitlink();
	} else if (!strcmp(argv[1], "blob-over-ceiling")) {
		case_blob_over_ceiling();
	} else if (!strcmp(argv[1], "parents")) {
		case_parents();
	} else if (!strcmp(argv[1], "treediff-ops")) {
		case_treediff_ops();
	} else if (!strcmp(argv[1], "treediff-order")) {
		case_treediff_order();
	} else if (!strcmp(argv[1], "treediff-kinds")) {
		case_treediff_kinds();
	} else if (!strcmp(argv[1], "treediff-typechange")) {
		case_treediff_typechange();
	} else if (!strcmp(argv[1], "treediff-typechange-edge")) {
		case_treediff_typechange_edge();
	} else if (!strcmp(argv[1], "treediff-rename")) {
		case_treediff_rename();
	} else if (!strcmp(argv[1], "treediff-rename-ambiguous")) {
		case_treediff_rename_ambiguous();
	} else if (!strcmp(argv[1], "treediff-kind-mismatch-edge")) {
		case_treediff_kind_mismatch_edge();
	} else if (!strcmp(argv[1], "treediff-root")) {
		case_treediff_root();
	} else if (!strcmp(argv[1], "treediff-identical")) {
		case_treediff_identical();
	} else if (!strcmp(argv[1], "treediff-rawmode")) {
		case_treediff_rawmode();
	} else if (!strcmp(argv[1], "treediff-alien-mode")) {
		case_treediff_alien_mode();
	} else if (!strcmp(argv[1], "treediff-ghost-subtree")) {
		case_treediff_ghost_subtree();
	} else if (!strcmp(argv[1], "treediff-dir-boundary")) {
		case_treediff_dir_boundary();
	} else if (!strcmp(argv[1], "assemble-ops")) {
		case_assemble_ops();
	} else if (!strcmp(argv[1], "assemble-mode-only")) {
		case_assemble_mode_only();
	} else if (!strcmp(argv[1], "assemble-mode-content")) {
		case_assemble_mode_content();
	} else if (!strcmp(argv[1], "assemble-empty-files")) {
		case_assemble_empty_files();
	} else if (!strcmp(argv[1], "assemble-symlink")) {
		case_assemble_symlink();
	} else if (!strcmp(argv[1], "assemble-gitlink")) {
		case_assemble_gitlink();
	} else if (!strcmp(argv[1], "assemble-typechange")) {
		case_assemble_typechange();
	} else if (!strcmp(argv[1], "assemble-rename-copy")) {
		case_assemble_rename_copy();
	} else if (!strcmp(argv[1], "assemble-width")) {
		case_assemble_width();
	} else if (!strcmp(argv[1], "assemble-nonewline")) {
		case_assemble_nonewline();
	} else if (!strcmp(argv[1], "assemble-identical")) {
		case_assemble_identical();
	} else if (!strcmp(argv[1], "assemble-binary")) {
		case_assemble_binary();
	} else {
		fprintf(stderr, "unknown case: %s\n", argv[1]);
		return 2;
	}

	return 0;
}
