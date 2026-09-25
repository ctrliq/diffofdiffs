// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ctrl IQ, Inc.
 *
 * Convert changed tree entries into an in-memory Git patch. Tree mode then uses
 * the same patch reader and comparison engine as user-supplied patches. File
 * operations and modes come from the tree entries; udiff supplies the text
 * hunks.
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include <assemble.h>
#include <funcname.h>
#include <udiff.h>
#include <gitread.h>
#include <iomem.h>
#include <treediff.h>
#include <util.h>

/*
 * Own the file bytes and the line table that points into them. A zeroed image
 * represents the absent side of a file creation or deletion.
 */
struct side_image {
	struct udiff_line *lines;
	struct udiff_image img;
	struct iomem_buf buf;
};

/*
 * Retain each line's LF so the diff engine can distinguish an unterminated
 * final line and emit its no-newline marker. Count first to allocate the line
 * table once.
 */
static void image_split(struct side_image *si)
{
	const char *base = si->buf.base;
	size_t len = si->buf.len;
	size_t n = 0;

	if (!len)
		return;

	for (size_t at = 0; at < len; n++) {
		const char *lf = memchr(base + at, '\n', len - at);

		at = lf ? (size_t)(lf - base) + 1 : len;
	}

	si->lines = xmalloc_array(n, sizeof(*si->lines));
	si->img = (typeof(si->img)){ .lines = si->lines,
				     .nlines = n,
				     .bytes = base };

	n = 0;
	for (size_t at = 0; at < len; n++) {
		const char *lf = memchr(base + at, '\n', len - at);
		size_t stop = lf ? (size_t)(lf - base) + 1 : len;

		si->lines[n].ptr = base + at;
		si->lines[n].len = stop - at;
		at = stop;
	}
}

void assemble_side_bytes(struct gitread *gr, const struct treediff_side *side,
			 const char *path, struct iomem_buf *out)
{
	char hex[GITREAD_OID_HEXSZ + 1];

	switch (side->kind) {
	case TREEDIFF_REG:
	case TREEDIFF_SYMLINK:
		gitread_blob_by_oid(gr, &side->oid, path, out);
		break;
	case TREEDIFF_GITLINK:
		gitread_oid_hex(&side->oid, hex);
		out->len = xasprintf(&out->base, "Subproject commit %s\n", hex);
		out->cap = out->len;
		break;
	}
}

/*
 * Reject NULs before generating text hunks. Keep this check out of
 * assemble_side_bytes(): later source lookups may read binary files whose
 * contents are unchanged and need no text hunks.
 */
static void side_load(struct gitread *gr, const struct treediff_side *side,
		      const char *path, struct side_image *si)
{
	assemble_side_bytes(gr, side, path, &si->buf);
	if (memchr(si->buf.base, '\0', si->buf.len))
		die("the blob at %s holds binary content, which a unified diff cannot carry",
		    path);

	image_split(si);
}

static void side_image_free(struct side_image *si)
{
	iomem_buf_free(&si->buf);
	free(si->lines);
}

/*
 * The patch reader treats LF as a line boundary and a tab in a file header as a
 * timestamp separator. Neither can represent part of a path in this format.
 */
static void guard_path(const char *path)
{
	if (strpbrk(path, "\n\t"))
		die("the tree path %s carries bytes a patch header cannot spell",
		    path);
}

static void emit_modes(const struct treediff_rec *rec, struct iomem_writer *w)
{
	switch (rec->op) {
	case TREEDIFF_CREATE:
		iomem_writer_printf(w, "new file mode %o\n", rec->new.mode);
		break;
	case TREEDIFF_DELETE:
		iomem_writer_printf(w, "deleted file mode %o\n", rec->old.mode);
		break;
	case TREEDIFF_MODIFY:
		if (rec->old.mode != rec->new.mode)
			iomem_writer_printf(w, "old mode %o\nnew mode %o\n",
					    rec->old.mode, rec->new.mode);
		break;
	}
}

struct blob_name_ctx {
	const struct udiff_image *img;
	bool c_source;
};

/* Use the same source definitions in derived patches and final reports */
static const char *blob_hunk_name(void *vctx, unsigned long old_lineno,
				  size_t *len_out)
{
	const struct blob_name_ctx *ctx = vctx;
	const struct udiff_image *img = ctx->img;
	size_t definition, len;
	const char *s, *cut;

	definition = source_definition(img, old_lineno, ctx->c_source);
	if (definition == SIZE_MAX)
		return NULL;

	s = img->lines[definition].ptr;
	len = img->lines[definition].len;
	cut = memchr(s, '\n', len);
	if (cut)
		len = cut - s;
	cut = memchr(s, '\r', len);
	if (cut)
		len = cut - s;
	for (; len && isspace(s[len - 1]); len--)
		;

	*len_out = len;
	return s;
}

static void emit_record(struct gitread *gr, const struct treediff_rec *rec,
			unsigned int context, struct iomem_writer *w)
{
	struct side_image old = {}, new = {};
	struct blob_name_ctx nctx = {
		.img = &old.img, .c_source = path_names_c_source(rec->path)
	};
	struct udiff_result res;

	guard_path(rec->path);
	iomem_writer_printf(w, "diff --git a/%s b/%s\n", rec->path, rec->path);
	emit_modes(rec, w);

	/* Equal object ids carry equal bytes, so there is nothing to read */
	if (rec->op == TREEDIFF_MODIFY &&
	    !memcmp(rec->old.oid.raw, rec->new.oid.raw, GITREAD_OID_RAWSZ))
		return;

	if (rec->op != TREEDIFF_CREATE)
		side_load(gr, &rec->old, rec->path, &old);

	if (rec->op != TREEDIFF_DELETE)
		side_load(gr, &rec->new, rec->path, &new);

	udiff_run(&old.img, &new.img, context, blob_hunk_name, &nctx, &res);
	if (!res.equal) {
		if (rec->op == TREEDIFF_CREATE)
			iomem_writer_printf(w, "--- /dev/null\n");
		else
			iomem_writer_printf(w, "--- a/%s\n", rec->path);

		if (rec->op == TREEDIFF_DELETE)
			iomem_writer_printf(w, "+++ /dev/null\n");
		else
			iomem_writer_printf(w, "+++ b/%s\n", rec->path);

		iomem_writer_write(w, res.out_buf, res.out_len);
	} else if (rec->op != TREEDIFF_MODIFY) {
		const struct gitread_oid absent = {};
		char old_hex[GITREAD_OID_HEXSZ + 1];
		char new_hex[GITREAD_OID_HEXSZ + 1];

		/*
		 * A lone creation or deletion mode line is not a complete Git
		 * file block. With no text hunks, add the index line so the
		 * reader recognizes an empty-file operation. Git spells the
		 * absent side as an all-zero object id.
		 */
		gitread_oid_hex(rec->op == TREEDIFF_CREATE ? &absent :
							     &rec->old.oid,
				old_hex);
		gitread_oid_hex(rec->op == TREEDIFF_DELETE ? &absent :
							     &rec->new.oid,
				new_hex);
		iomem_writer_printf(w, "index %s..%s\n", old_hex, new_hex);
	}

	udiff_result_free(&res);
	side_image_free(&old);
	side_image_free(&new);
}

void assemble_patch(struct gitread *gr, const struct treediff_map *map,
		    unsigned int context, struct iomem_buf *out)
{
	struct iomem_writer w;

	iomem_writer_open(&w);

	for (size_t i = 0; i < map->n; i++)
		emit_record(gr, &map->recs[i], context, &w);

	iomem_writer_publish(&w, out);
}
