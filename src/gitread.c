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

void gitread_oid_hex(const struct gitread_oid *oid,
		     char hex[GITREAD_OID_HEXSZ + 1])
{
	git_oid full;

	oid_to_libgit2(&full, oid);
	git_oid_fmt(hex, &full);
	hex[GITREAD_OID_HEXSZ] = '\0';
}
