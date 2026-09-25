// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ctrl IQ, Inc.
 *
 * Read commits, trees, and blobs through libgit2 without a checkout. Reject
 * repositories whose history or object availability depends on features this
 * reader doesn't implement. Errors use stable diagnostics; version-dependent
 * library details are available under --debug.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <git2.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gitread.h>
#include <iomem.h>
#include <patch-types.h>
#include <util.h>

/* Earlier releases misresolve relative paths in nested alternate stores */
#ifndef LIBGIT2_VERSION_CHECK
#error "libgit2 1.9.1 or newer is required"
#elif !LIBGIT2_VERSION_CHECK(1, 9, 1)
#error "libgit2 1.9.1 or newer is required"
#endif

_Static_assert(GITREAD_OID_RAWSZ == GIT_OID_SHA1_SIZE,
	       "SHA-1 object ID sizes must agree");

struct gitread {
	git_repository *repo;
	git_odb *odb;
};

static void oid_from_libgit2(struct gitread_oid *out, const git_oid *oid)
{
	memcpy(out->raw, oid->id, GITREAD_OID_RAWSZ);
}

static void oid_to_libgit2(git_oid *out, const struct gitread_oid *oid)
{
	git_oid_fromraw(out, oid->raw);
}

static void dbg_last_error(const char *what)
{
	const git_error *err = git_error_last();

	pr_dbg("%s: %s\n", what,
	       err && err->message ? err->message : "(no library detail)");
}

/*
 * These extensions affect object availability or reference lookup. Check them
 * even at format version 0, where libgit2 doesn't validate extension keys.
 */
static void check_repo_format(git_repository *repo, const char *dir)
{
	git_config *cfg = NULL;
	git_buf val = {};
	int version = 0;
	int ret;

	ret = git_repository_config_snapshot(&cfg, repo);
	if (ret) {
		dbg_last_error("configuration snapshot");
		die("cannot read the configuration of the git repository at %s",
		    dir);
	}

	ret = git_config_get_int32(&version, cfg,
				   "core.repositoryformatversion");
	if (!ret && version > 1)
		die("the git repository at %s carries repository format version %d; this reader implements versions 0 and 1",
		    dir, version);

	ret = git_config_get_string_buf(&val, cfg, "extensions.refstorage");
	if (!ret && strcmp(val.ptr, "files"))
		die("the git repository at %s stores references via the %s backend; this reader implements the files backend",
		    dir, val.ptr);

	git_buf_dispose(&val);
	ret = git_config_get_string_buf(&val, cfg, "extensions.partialclone");
	if (!ret)
		die("the git repository at %s is a partial clone, whose object store is missing objects by design",
		    dir);

	git_buf_dispose(&val);
	git_config_free(cfg);
}

/* Flags the walk's payload and stops it at the first replace reference */
static int spot_replace_ref(const char *name, void *payload)
{
	bool *seen = payload;

	if (strncmp(name, "refs/replace/", strlen("refs/replace/")))
		return 0;

	*seen = true;
	return 1;
}

/*
 * Grafts and replacement references change the history Git reports. Shallow
 * repositories omit parents still named by their boundary commits. Reading only
 * the stored commits would ignore those conditions.
 */
static void check_history_guards(git_repository *repo, const char *dir)
{
	char *grafts __free(free) = NULL;
	bool replaced = false;
	struct stat st;
	int ret;

	if (git_repository_is_shallow(repo))
		die("the git repository at %s is shallow; its history is incomplete by design",
		    dir);

	xasprintf(&grafts, "%sinfo/grafts", git_repository_commondir(repo));
	ret = stat(grafts, &st);
	if (!ret && st.st_size)
		die("the git repository at %s carries an info/grafts file, which this reader does not implement",
		    dir);

	ret = git_reference_foreach_name(repo, spot_replace_ref, &replaced);

	/* The callback's deliberate stop also returns a nonzero status */
	if (replaced)
		die("the git repository at %s carries replace references, which this reader does not implement",
		    dir);

	if (ret) {
		dbg_last_error("reference walk");
		die("cannot enumerate the references of the git repository at %s",
		    dir);
	}
}

/* Version 2 starts with magic and version words; version 1 has no header */
#define IDX_V2_MAGIC 0xff744f63U
#define IDX_V2_HEADER_SIZE (2 * sizeof(u32))

/* Fanout slots count objects through each possible first byte of the hash */
#define IDX_FANOUT_ENTRIES 256
#define IDX_FANOUT_SIZE (IDX_FANOUT_ENTRIES * sizeof(u32))

/* Both formats end with the pack checksum and then the index checksum */
#define IDX_TRAILER_SIZE (2 * GIT_OID_SHA1_SIZE)

/* Version 1 stores a 32-bit pack offset and a SHA-1 hash per object */
#define IDX_V1_ENTRY_SIZE (sizeof(u32) + GIT_OID_SHA1_SIZE)

/* Version 2 adds a CRC word and optionally an entry in the 64-bit offsets */
#define IDX_V2_ENTRY_SIZE (GIT_OID_SHA1_SIZE + 2 * sizeof(u32))
#define IDX_LARGE_OFFSET_SIZE sizeof(u64)

static u32 read_be32(const u8 *bytes)
{
	return (u32)bytes[0] << 24 | (u32)bytes[1] << 16 | (u32)bytes[2] << 8 |
	       bytes[3];
}

/*
 * Check the version and plausible length before libgit2 can skip a bad index
 * and report missing objects instead. The last fanout slot gives the object
 * count: version 1 has a fixed size per object, while version 2 can add one
 * large offset per object. This is an early diagnostic, not a checksum or
 * object-table validator; libgit2 still reads the index and packed objects.
 */
static void check_pack_index(const char *dir, const char *packdir,
			     const char *name)
{
	u8 head[IDX_V2_HEADER_SIZE], fan[sizeof(u32)];
	off_t last_fanout = IDX_FANOUT_SIZE - sizeof(fan);
	char *path __free(free) = NULL;
	u64 count, floor, ceiling;
	struct stat st;
	int fd;

	xasprintf(&path, "%s/%s", packdir, name);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		edie(errno,
		     "cannot read the pack index %s of the git repository at %s",
		     name, dir);

	if (fstat(fd, &st))
		edie(errno,
		     "cannot read the pack index %s of the git repository at %s",
		     name, dir);

	/* A directory here is store debris, not an index; leave it alone */
	if (!S_ISREG(st.st_mode)) {
		close(fd);
		return;
	}

	if (pread(fd, head, sizeof(head), 0) != sizeof(head))
		die("the git repository at %s carries a pack index this reader cannot parse (%s)",
		    dir, name);

	if (read_be32(head) == IDX_V2_MAGIC) {
		u32 version = read_be32(head + sizeof(u32));

		if (version != 2)
			die("the git repository at %s carries a version-%u pack index (%s); this reader implements versions 1 and 2",
			    dir, version, name);

		/* The last fanout slot, past the header's magic and version */
		if (pread(fd, fan, sizeof(fan),
			  IDX_V2_HEADER_SIZE + last_fanout) != sizeof(fan))
			die("the git repository at %s carries a pack index this reader cannot parse (%s)",
			    dir, name);

		count = read_be32(fan);
		floor = IDX_V2_HEADER_SIZE + IDX_FANOUT_SIZE +
			IDX_V2_ENTRY_SIZE * count + IDX_TRAILER_SIZE;
		ceiling = floor + IDX_LARGE_OFFSET_SIZE * count;
	} else {
		/* The last fanout slot; version 1 opens with the table */
		if (pread(fd, fan, sizeof(fan), last_fanout) != sizeof(fan))
			die("the git repository at %s carries a pack index this reader cannot parse (%s)",
			    dir, name);

		count = read_be32(fan);
		floor = IDX_FANOUT_SIZE + IDX_V1_ENTRY_SIZE * count +
			IDX_TRAILER_SIZE;
		ceiling = floor;
	}

	if ((u64)st.st_size < floor || (u64)st.st_size > ceiling)
		die("the git repository at %s carries a pack index this reader cannot parse (%s)",
		    dir, name);

	close(fd);
}

/*
 * libgit2 can ignore a malformed pack index and report its objects as missing.
 * Check versions and plausible lengths in this repository's pack directory to
 * give an earlier storage diagnostic. This does not establish index integrity.
 * Leave alternate stores to libgit2, whose discovery rules this reader does not
 * duplicate.
 */
static void check_pack_indexes(git_repository *repo, const char *dir)
{
	char *packdir __free(free) = NULL;
	struct dirent *de;
	DIR *pd;

	xasprintf(&packdir, "%sobjects/pack", git_repository_commondir(repo));
	pd = opendir(packdir);
	if (!pd) {
		/*
		 * A repository with only loose objects may have no pack
		 * directory.
		 */
		if (errno == ENOENT)
			return;

		edie(errno,
		     "cannot enumerate the pack directory of the git repository at %s",
		     dir);
	}

	while ((de = readdir(pd))) {
		size_t len = strlen(de->d_name);

		if (len < strlen(".idx") ||
		    strcmp(de->d_name + len - strlen(".idx"), ".idx"))
			continue;

		check_pack_index(dir, packdir, de->d_name);
	}

	closedir(pd);
}

void gitread_open(struct gitread **out, const char *dir)
{
	struct gitread *gr;
	int ret;

	ret = git_libgit2_init();
	if (ret < 0)
		die("cannot initialize the git object reader");

	/*
	 * Ignore system and user Git configuration so machine preferences don't
	 * change repository interpretation.
	 */
	if (git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, GIT_CONFIG_LEVEL_SYSTEM,
			     "") ||
	    git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, GIT_CONFIG_LEVEL_GLOBAL,
			     "") ||
	    git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, GIT_CONFIG_LEVEL_XDG,
			     "") ||
	    git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH,
			     GIT_CONFIG_LEVEL_PROGRAMDATA, ""))
		die("cannot confine the git object reader to the repository's own configuration");

	gr = xzalloc(sizeof(*gr));

	/*
	 * Without NO_SEARCH, an invalid directory could resolve to a repository
	 * in one of its ancestors.
	 */
	ret = git_repository_open_ext(&gr->repo, dir,
				      GIT_REPOSITORY_OPEN_NO_SEARCH, NULL);
	if (ret) {
		dbg_last_error("repository open");
		die("cannot open a git repository at %s", dir);
	}

	if (git_repository_oid_type(gr->repo) != GIT_OID_SHA1)
		die("the git repository at %s uses an object format other than sha1, which this reader does not implement",
		    dir);

	check_repo_format(gr->repo, dir);
	check_history_guards(gr->repo, dir);
	check_pack_indexes(gr->repo, dir);

	ret = git_repository_odb(&gr->odb, gr->repo);
	if (ret) {
		dbg_last_error("object database open");
		die("cannot open the object store of the git repository at %s",
		    dir);
	}

	*out = gr;
}

void gitread_close(struct gitread **gr)
{
	if (!*gr)
		return;

	git_odb_free((*gr)->odb);
	git_repository_free((*gr)->repo);
	free(*gr);
	*gr = NULL;
	git_libgit2_shutdown();
}

/*
 * Git requires at least four hex digits for an abbreviated object ID. Longer
 * names must still fit a complete ID before taking the object lookup path.
 */
static bool name_is_hex_prefix(const char *name, size_t *len)
{
	size_t n = strlen(name);

	if (n < 4 || n > GITREAD_OID_HEXSZ)
		return false;

	for (size_t i = 0; i < n; i++) {
		if (!isxdigit(name[i]))
			return false;
	}

	*len = n;
	return true;
}

void gitread_resolve_commit(struct gitread *gr, const char *name,
			    struct gitread_oid *out)
{
	git_object *peeled = NULL;
	git_object *obj = NULL;
	size_t len;
	int ret;

	if (name_is_hex_prefix(name, &len)) {
		git_oid full, prefix;

		ret = git_oid_fromstrp(&prefix, name);
		if (ret)
			die("cannot parse %s as an object id", name);

		ret = git_odb_exists_prefix(&full, gr->odb, &prefix, len);
		if (ret == GIT_EAMBIGUOUS)
			die("%s names more than one object; give a longer prefix",
			    name);

		/*
		 * A missing object permits a branch named with hex digits.
		 * Other failures must not be mistaken for a missing object.
		 */
		if (ret && ret != GIT_ENOTFOUND) {
			dbg_last_error("object-store search");
			die("cannot search the object store for %s", name);
		}

		if (!ret) {
			ret = git_object_lookup(&obj, gr->repo, &full,
						GIT_OBJECT_ANY);
			if (ret) {
				dbg_last_error("object lookup");
				die("cannot read the object %s names", name);
			}
		}
	}

	if (!obj) {
		git_reference *resolved = NULL;
		git_reference *ref = NULL;

		ret = git_reference_dwim(&ref, gr->repo, name);
		if (ret) {
			dbg_last_error("name resolution");
			die("cannot resolve %s to an object or reference in the repository",
			    name);
		}

		ret = git_reference_resolve(&resolved, ref);
		git_reference_free(ref);
		if (ret) {
			dbg_last_error("symbolic-reference chase");
			die("cannot resolve the symbolic reference %s names",
			    name);
		}

		ret = git_object_lookup(&obj, gr->repo,
					git_reference_target(resolved),
					GIT_OBJECT_ANY);
		git_reference_free(resolved);
		if (ret) {
			dbg_last_error("object lookup");
			die("cannot read the object %s names", name);
		}
	}

	ret = git_object_peel(&peeled, obj, GIT_OBJECT_COMMIT);
	git_object_free(obj);
	if (ret)
		die("%s does not name a commit", name);

	oid_from_libgit2(out, git_object_id(peeled));
	git_object_free(peeled);
}

static git_commit *lookup_commit(struct gitread *gr,
				 const struct gitread_oid *commit)
{
	char hex[GITREAD_OID_HEXSZ + 1];
	git_commit *c = NULL;
	git_oid oid;
	int ret;

	oid_to_libgit2(&oid, commit);
	ret = git_commit_lookup(&c, gr->repo, &oid);
	if (ret) {
		gitread_oid_hex(commit, hex);
		dbg_last_error("commit lookup");
		die("cannot read the commit %s", hex);
	}
	return c;
}

char *gitread_commit_subject(struct gitread *gr,
			     const struct gitread_oid *commit)
{
	char hex[GITREAD_OID_HEXSZ + 1];
	git_commit *c = lookup_commit(gr, commit);
	const char *summary;
	char *subject;

	summary = git_commit_summary(c);
	if (!summary) {
		gitread_oid_hex(commit, hex);
		dbg_last_error("commit subject");
		die("cannot read the subject of commit %s", hex);
	}
	subject = xstrdup(summary);
	git_commit_free(c);
	return subject;
}

int gitread_commit_parents(struct gitread *gr, const struct gitread_oid *commit,
			   struct gitread_oid *first_parent)
{
	git_commit *c = lookup_commit(gr, commit);
	int n;

	n = git_commit_parentcount(c);
	if (n > 0 && first_parent)
		oid_from_libgit2(first_parent, git_commit_parent_id(c, 0));

	git_commit_free(c);
	return n;
}

bool gitread_commit_parent_of(struct gitread *gr, const struct gitread_oid *tip,
			      const struct gitread_oid *candidate)
{
	git_commit *c = lookup_commit(gr, tip);
	bool found = false;
	unsigned int n;

	n = git_commit_parentcount(c);
	for (unsigned int i = 0; !found && i < n; i++) {
		struct gitread_oid parent;

		oid_from_libgit2(&parent, git_commit_parent_id(c, i));
		found = !memcmp(parent.raw, candidate->raw, GITREAD_OID_RAWSZ);
	}

	git_commit_free(c);
	return found;
}

static git_tree *commit_tree(struct gitread *gr,
			     const struct gitread_oid *commit, char *hex)
{
	git_tree *tree = NULL;
	git_commit *c;
	int ret;

	if (!commit)
		return NULL;

	c = lookup_commit(gr, commit);
	gitread_oid_hex(commit, hex);
	ret = git_commit_tree(&tree, c);
	git_commit_free(c);
	if (ret) {
		dbg_last_error("tree lookup");
		die("cannot read the tree of commit %s", hex);
	}
	return tree;
}

static git_tree_entry *read_path_entry(struct gitread *gr,
				       const struct gitread_oid *commit,
				       const char *path)
{
	char hex[GITREAD_OID_HEXSZ + 1];
	git_tree_entry *entry = NULL;
	git_tree *tree = commit_tree(gr, commit, hex);
	int ret;

	ret = git_tree_entry_bypath(&entry, tree, path);
	git_tree_free(tree);
	if (ret == GIT_ENOTFOUND)
		return NULL;

	if (ret) {
		dbg_last_error("path lookup");
		die("cannot look up %s in the tree of commit %s", path, hex);
	}

	return entry;
}

bool gitread_entry_by_path(struct gitread *gr, const struct gitread_oid *commit,
			   const char *path, struct gitread_oid *id, u32 *mode)
{
	git_tree_entry *entry = read_path_entry(gr, commit, path);

	if (!entry)
		return false;

	/*
	 * A directory replacing this file has separate entries for its
	 * contents.
	 */
	if (git_tree_entry_type(entry) == GIT_OBJECT_TREE) {
		git_tree_entry_free(entry);
		return false;
	}

	oid_from_libgit2(id, git_tree_entry_id(entry));
	*mode = git_tree_entry_filemode_raw(entry);
	git_tree_entry_free(entry);
	return true;
}

bool gitread_blob_by_path(struct gitread *gr, const struct gitread_oid *commit,
			  const char *path, struct iomem_buf *out, u32 *mode)
{
	char hex[GITREAD_OID_HEXSZ + 1];
	git_tree_entry *entry = read_path_entry(gr, commit, path);
	git_object_size_t size;
	git_blob *blob = NULL;
	int ret;

	if (!entry)
		return false;

	gitread_oid_hex(commit, hex);

	switch (git_tree_entry_type(entry)) {
	case GIT_OBJECT_BLOB:
		break;
	case GIT_OBJECT_TREE:
		die("%s in commit %s names a directory, not a file", path, hex);

	case GIT_OBJECT_COMMIT:
		die("%s in commit %s names a gitlink (a nested repository), not a file",
		    path, hex);

	default:
		die("%s in commit %s carries an entry type no tree may hold",
		    path, hex);
	}

	ret = git_blob_lookup(&blob, gr->repo, git_tree_entry_id(entry));
	if (ret) {
		dbg_last_error("blob lookup");
		die("cannot read the blob %s names in commit %s", path, hex);
	}

	size = git_blob_rawsize(blob);
	if (size > PAYLOAD_CEILING)
		die("the blob at %s in commit %s carries %llu bytes, over the diff engine's %zu MiB per-image payload ceiling",
		    path, hex, (unsigned long long)size, PAYLOAD_CEILING >> 20);

	out->len = size;
	out->cap = out->len;
	out->base = memdup(git_blob_rawcontent(blob), out->len);
	*mode = git_tree_entry_filemode_raw(entry);

	git_blob_free(blob);
	git_tree_entry_free(entry);
	return true;
}

void gitread_blob_by_oid(struct gitread *gr, const struct gitread_oid *id,
			 const char *path, struct iomem_buf *out)
{
	char hex[GITREAD_OID_HEXSZ + 1];
	git_object_size_t size;
	git_blob *blob = NULL;
	git_oid oid;
	int ret;

	gitread_oid_hex(id, hex);
	oid_to_libgit2(&oid, id);
	ret = git_blob_lookup(&blob, gr->repo, &oid);
	if (ret) {
		dbg_last_error("blob lookup");
		die("cannot read the blob %s at %s", hex, path);
	}

	size = git_blob_rawsize(blob);
	if (size > PAYLOAD_CEILING)
		die("the blob at %s carries %llu bytes, over the diff engine's %zu MiB per-image payload ceiling",
		    path, (unsigned long long)size, PAYLOAD_CEILING >> 20);

	out->len = size;
	out->cap = out->len;
	out->base = memdup(git_blob_rawcontent(blob), out->len);
	git_blob_free(blob);
}

/* Grows the entry array geometrically; a walk cannot know its count ahead */
static void tree_push(struct gitread_tree *out, size_t *cap, const char *dir,
		      const char *name, const git_oid *oid, u32 mode)
{
	struct gitread_tree_entry *entry;

	if (out->n == *cap) {
		*cap = *cap ? *cap * 2 : 64;
		out->entries =
			xrealloc_array(out->entries, *cap, sizeof(*entry));
	}

	entry = &out->entries[out->n++];
	xasprintf(&entry->path, "%s%s", dir, name);
	oid_from_libgit2(&entry->oid, oid);
	entry->mode = mode;
}

static git_tree *lookup_subtree(struct gitread *gr, const git_tree_entry *entry,
				const char *dir, const char *hex)
{
	git_tree *tree = NULL;
	int ret;

	ret = git_tree_lookup(&tree, gr->repo, git_tree_entry_id(entry));
	if (ret) {
		dbg_last_error("subtree lookup");
		die("cannot read the tree at %s%s in commit %s", dir,
		    git_tree_entry_name(entry), hex);
	}
	return tree;
}

DEFINE_FREE(git_tree, git_tree *, git_tree_free(_T))

/*
 * Collect this tree's differing non-directory entries. Equal ids and raw modes
 * prune whole subtrees; a missing or non-directory peer prunes nothing. Entries
 * found only in the peer need a separate walk with the sides swapped. Gitlinks
 * contribute their ids without looking into another object store.
 */
static void tree_collect(struct gitread *gr, const git_tree *tree,
			 const git_tree *other, const char *dir,
			 const char *const hex[2], struct gitread_tree *out,
			 size_t *cap)
{
	size_t count = git_tree_entrycount(tree);

	for (size_t i = 0; i < count; i++) {
		const git_tree_entry *entry = git_tree_entry_byindex(tree, i);
		const char *name = git_tree_entry_name(entry);
		const git_tree_entry *peer =
			other ? git_tree_entry_byname(other, name) : NULL;
		git_tree *peer_sub __free(git_tree) = NULL;
		git_tree *sub __free(git_tree) = NULL;
		char *subdir __free(free) = NULL;

		if (peer &&
		    git_oid_equal(git_tree_entry_id(entry),
				  git_tree_entry_id(peer)) &&
		    git_tree_entry_filemode_raw(entry) ==
			    git_tree_entry_filemode_raw(peer))
			continue;

		if (git_tree_entry_type(entry) != GIT_OBJECT_TREE) {
			tree_push(out, cap, dir, name, git_tree_entry_id(entry),
				  git_tree_entry_filemode_raw(entry));
			continue;
		}

		sub = lookup_subtree(gr, entry, dir, hex[0]);
		if (peer && git_tree_entry_type(peer) == GIT_OBJECT_TREE)
			peer_sub = lookup_subtree(gr, peer, dir, hex[1]);
		xasprintf(&subdir, "%s%s/", dir, name);
		tree_collect(gr, sub, peer_sub, subdir, hex, out, cap);
	}
}

static int tree_entry_cmp(const void *a, const void *b)
{
	const struct gitread_tree_entry *ea = a;
	const struct gitread_tree_entry *eb = b;

	return strcmp(ea->path, eb->path);
}

void gitread_tree_read(struct gitread *gr, const struct gitread_oid *commit,
		       const struct gitread_oid *other_commit,
		       struct gitread_tree *out)
{
	char hex[2][GITREAD_OID_HEXSZ + 1] = {};
	git_tree *tree __free(git_tree) = commit_tree(gr, commit, hex[0]);
	git_tree *other __free(git_tree) =
		commit_tree(gr, other_commit, hex[1]);
	const char *labels[2] = { hex[0], hex[1] };
	size_t cap = 0;

	tree_collect(gr, tree, other, "", labels, out, &cap);
	if (out->n > 1)
		qsort(out->entries, out->n, sizeof(*out->entries),
		      tree_entry_cmp);
}

void gitread_tree_free(struct gitread_tree *tree)
{
	for (size_t i = 0; i < tree->n; i++)
		free(tree->entries[i].path);

	free(tree->entries);
	tree->entries = NULL;
	tree->n = 0;
}

void gitread_oid_hex(const struct gitread_oid *oid,
		     char hex[GITREAD_OID_HEXSZ + 1])
{
	git_oid full;

	oid_to_libgit2(&full, oid);
	git_oid_fmt(hex, &full);
	hex[GITREAD_OID_HEXSZ] = '\0';
}
