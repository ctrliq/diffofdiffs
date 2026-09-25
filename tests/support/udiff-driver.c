// SPDX-License-Identifier: GPL-2.0-only
/*
 * udiff-driver.c - the test driver for line comparison
 * Copyright (C) 2025-2026 Ctrl IQ, Inc.
 * Author: Sultan Alsawaf <sultan@ciq.com>
 */

/*
 * A standalone harness around udiff_run(). It materializes two operand images
 * from files, runs the engine, and writes the bare hunk stream to stdout. An
 * independent two-row LCS cost oracle, a format validator, and a dense name
 * provider check the public result. The shipped tool builds none of this file.
 *
 * Both operand loaders exercise the line interface: one includes the final
 * newline in its length and the other adapts a length that excludes it. The
 * harness proves that the resulting tables are identical.
 */

#include <ctype.h>
#include <errno.h>
#include <error.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include <udiff.h>
#include <reader.h>
#include <util.h>

/*
 * Usage breaches exit with this instead of EXIT_FAILURE so a harness can tell
 * "the driver was called wrong" from "the run refused or emitted".
 */
#define EXIT_USAGE 2

/*
 * The sanitizer lane's allocator aborts inside its runtime on an over-limit
 * request by default instead of returning NULL, which would turn the memory
 * bound's refusal into a lane-specific abort. Compiling the default in keeps
 * every invocation on the NULL-then-die path the x-wrappers own; the runtime
 * merges these defaults with ASAN_OPTIONS per flag, so an environment flag of
 * the same name still wins. The plain lane exports the symbol and nothing reads
 * it.
 */
const char *__asan_default_options(void);
const char *__asan_default_options(void)
{
	return "allocator_may_return_null=1";
}

/*
 * SHA-1 (FIPS 180-4), embedded so --dump-lines can name a line's bytes
 * unambiguously without dragging a hashing library in.
 */
struct sha1_ctx {
	u32 h[5];
	u64 nbytes;
	u8 block[64];
	size_t fill;
};

static void sha1_compress(struct sha1_ctx *ctx, const u8 *p)
{
	u32 a, b, cv, d, e, f, k, t;
	u32 w[80];
	int i;

	for (i = 0; i < 16; i++) {
		w[i] = ((u32)p[4 * i] << 24) | ((u32)p[4 * i + 1] << 16) |
		       ((u32)p[4 * i + 2] << 8) | (u32)p[4 * i + 3];
	}
	for (i = 16; i < 80; i++) {
		t = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
		w[i] = (t << 1) | (t >> 31);
	}

	a = ctx->h[0];
	b = ctx->h[1];
	cv = ctx->h[2];
	d = ctx->h[3];
	e = ctx->h[4];

	for (i = 0; i < 80; i++) {
		if (i < 20) {
			f = (b & cv) | (~b & d);
			k = 0x5a827999u;
		} else if (i < 40) {
			f = b ^ cv ^ d;
			k = 0x6ed9eba1u;
		} else if (i < 60) {
			f = (b & cv) | (b & d) | (cv & d);
			k = 0x8f1bbcdcu;
		} else {
			f = b ^ cv ^ d;
			k = 0xca62c1d6u;
		}
		t = ((a << 5) | (a >> 27)) + f + e + k + w[i];
		e = d;
		d = cv;
		cv = (b << 30) | (b >> 2);
		b = a;
		a = t;
	}

	ctx->h[0] += a;
	ctx->h[1] += b;
	ctx->h[2] += cv;
	ctx->h[3] += d;
	ctx->h[4] += e;
}

static void sha1_init(struct sha1_ctx *ctx)
{
	ctx->h[0] = 0x67452301u;
	ctx->h[1] = 0xefcdab89u;
	ctx->h[2] = 0x98badcfeu;
	ctx->h[3] = 0x10325476u;
	ctx->h[4] = 0xc3d2e1f0u;
	ctx->nbytes = 0;
	ctx->fill = 0;
}

static void sha1_update(struct sha1_ctx *ctx, const void *data, size_t len)
{
	const u8 *p = data;

	ctx->nbytes += len;
	while (len) {
		size_t take = sizeof(ctx->block) - ctx->fill;

		if (take > len)
			take = len;
		memcpy(ctx->block + ctx->fill, p, take);
		ctx->fill += take;
		p += take;
		len -= take;
		if (ctx->fill == sizeof(ctx->block)) {
			sha1_compress(ctx, ctx->block);
			ctx->fill = 0;
		}
	}
}

static void sha1_final(struct sha1_ctx *ctx, char *hex)
{
	static const char digits[] = "0123456789abcdef";
	u64 bits = ctx->nbytes * 8;
	u8 pad = 0x80;
	u8 zero = 0;
	u8 tail[8];
	int i;

	for (i = 0; i < 8; i++)
		tail[i] = (bits >> (56 - 8 * i));

	sha1_update(ctx, &pad, 1);
	while (ctx->fill != 56)
		sha1_update(ctx, &zero, 1);

	/*
	 * nbytes is meaningless past the padding; the length block goes in by
	 * hand so the counter can't fold itself back into the digest.
	 */
	memcpy(ctx->block + 56, tail, 8);
	sha1_compress(ctx, ctx->block);
	ctx->fill = 0;

	for (i = 0; i < 20; i++) {
		unsigned v = (ctx->h[i / 4] >> (24 - 8 * (i % 4))) & 0xffu;

		hex[2 * i] = digits[v >> 4];
		hex[2 * i + 1] = digits[v & 0xfu];
	}
	hex[40] = '\0';
}

static void sha1_hex(const void *data, size_t len, char *hex)
{
	struct sha1_ctx ctx;

	sha1_init(&ctx);
	sha1_update(&ctx, data, len);
	sha1_final(&ctx, hex);
}

/*
 * Operand materialization.
 */

/*
 * Which newline convention the intermediate table is built in before the
 * udiff_line invariants are established.
 */
enum loader {
	LOADER_GETLINE, /* Input lengths include the LF */
	LOADER_LINEINFO /* Input lengths exclude the LF */
};

struct operand {
	const char *path;
	char *buf; /* whole-file bytes; the lines borrow them */
	size_t nbytes;
	struct udiff_line *lines;
	size_t nlines;
	struct udiff_image img;
};

/*
 * Whole-file binary read: NUL is ordinary data, no text translation, so a CRLF
 * file keeps its CRs and an unterminated final line stays unterminated. Read in
 * chunks rather than off a stat size so a pipe works too.
 */
static void read_whole_file(struct operand *op)
{
	size_t cap = 4096, len = 0;
	char *buf = xmalloc(cap);
	FILE *f;

	f = fopen(op->path, "rb");
	if (!f)
		edie(errno, "opening %s", op->path);

	for (;;) {
		size_t got;

		if (len == cap) {
			cap *= 2;
			buf = xrealloc(buf, cap);
		}
		got = fread(buf + len, 1, cap - len, f);
		len += got;
		if (got == 0) {
			if (ferror(f))
				edie(errno, "reading %s", op->path);

			break;
		}
	}
	fclose(f);

	op->buf = buf;
	op->nbytes = len;
}

/*
 * Lines are LF-terminated runs; a trailing empty tail past the final LF is not
 * a line, and an empty file has none.
 */
static size_t count_lines(const char *buf, size_t n)
{
	size_t lines = 0;

	for (size_t i = 0; i < n; lines++) {
		const char *nl = memchr(buf + i, '\n', n - i);

		i = nl ? (size_t)(nl - buf) + 1 : n;
	}

	return lines;
}

/*
 * Materialize op->lines in the requested convention. Both paths must land on
 * the same table: the udiff_line contract wants the LF inside `len`, so the
 * lineinfo path builds the LF-excluding table first and then runs the
 * bounds-checked adapter over it. Getline also lends its complete byte buffer;
 * lineinfo exercises the engine's materialization of separate rows. Both must
 * produce the same comparison.
 */
static void load_operand(struct operand *op, enum loader loader)
{
	size_t i, k;

	read_whole_file(op);
	op->nlines = count_lines(op->buf, op->nbytes);
	op->lines = op->nlines ? xmalloc_array(op->nlines, sizeof(*op->lines)) :
				 NULL;

	for (i = 0, k = 0; i < op->nbytes; k++) {
		const char *at = op->buf + i;
		const char *nl = memchr(at, '\n', op->nbytes - i);
		size_t run = nl ? (size_t)(nl - at) : op->nbytes - i;

		op->lines[k].ptr = at;
		if (loader == LOADER_GETLINE) {
			op->lines[k].len = run + !!nl;
			i += op->lines[k].len;
		} else {
			op->lines[k].len = run;
			i += run + !!nl;
		}
	}

	if (loader == LOADER_LINEINFO) {
		for (k = 0; k < op->nlines; k++) {
			const char *s = op->lines[k].ptr;
			size_t flen = op->lines[k].len;

			/*
			 * len = flen + 1 only when the byte at flen is really
			 * the LF and really inside the buffer.
			 */
			if (s + flen < op->buf + op->nbytes && s[flen] == '\n')
				op->lines[k].len = flen + 1;
		}
	}

	/*
	 * A zero-length line isn't a line, so neither convention can produce
	 * one: an empty source line is the single byte LF.
	 */
	for (k = 0; k < op->nlines; k++) {
		if (!op->lines[k].len)
			die("%s line %zu materialized empty, which the interface forbids",
			    op->path, k + 1);
	}

	op->img = (typeof(op->img)){ .lines = op->lines,
				     .nlines = op->nlines,
				     .bytes = loader == LOADER_GETLINE ?
						      op->buf :
						      NULL };
}

static void operand_free(struct operand *op)
{
	free(op->lines);
	free(op->buf);
	op->lines = NULL;
	op->buf = NULL;
}

static void dump_lines(const struct operand *op)
{
	printf("file=%s nlines=%zu\n", op->path, op->nlines);
	for (size_t k = 0; k < op->nlines; k++) {
		char hex[41];

		sha1_hex(op->lines[k].ptr, op->lines[k].len, hex);
		printf("len=%zu sha1=%s\n", op->lines[k].len, hex);
	}
}

/*
 * The test name provider searches upward in the complete old file for a
 * definition-like line before the hunk. This exercises callback coordinates and
 * verbatim header labels independently of the patch reader.
 */
static bool name_first_byte_ok(char c)
{
	/* C locale by construction: no locale calls, no ctype tables */
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '$' ||
	       c == '_';
}

static const char *name_stub(void *ctx, unsigned long old_lineno,
			     size_t *len_out)
{
	const struct udiff_image *a = ctx;
	unsigned long n;

	/*
	 * old_lineno reads as the 1-based number of the line just above the
	 * window, so the walk is at-or-above and 0 means nothing above.
	 */
	for (n = old_lineno; n >= 1; n--) {
		const struct udiff_line *l = &a->lines[n - 1];
		size_t len = l->len;

		if (!name_first_byte_ok(l->ptr[0]))
			continue;

		/*
		 * The tail is the line minus its LF, verbatim at any length: no
		 * truncation, since the engine's contract takes a length.
		 */
		if (len && l->ptr[len - 1] == '\n')
			len--;

		/*
		 * Then drop one trailing CR, so a CRLF operand doesn't park a
		 * bare CR inside the hunk header. The engine's contract bans
		 * newline bytes in a tail (udiff.h's udiff_name_fn contract)
		 * and it copies the tail verbatim, checking only for '\n';
		 * every header it writes is otherwise CR-free. This is the
		 * driver's own policy for its dense provider, not the engine's.
		 */
		if (len && l->ptr[len - 1] == '\r')
			len--;
		if (!len)
			continue;

		*len_out = len;
		return l->ptr;
	}

	return NULL;
}

/*
 * The independent cost oracle: a two-row LCS over the two line tables, with
 * equality defined as the engine's unfolded relation (same length, same bytes).
 * Linear memory is mandatory here; the largest archived pair is ~1.1e8 cells,
 * and a full matrix isn't implementable.
 */
static void print_dp_cost(const struct operand *a, const struct operand *b)
{
	size_t na = a->nlines, nb = b->nlines;
	size_t *r0 __free(free) = xzalloc_array(nb + 1, sizeof(*r0));
	size_t *r1 __free(free) = xmalloc_array(nb + 1, sizeof(*r1));
	size_t *prev, *cur;
	size_t i, j, lcs;

	prev = r0;
	cur = r1;

	for (i = 1; i <= na; i++) {
		const char *ap = a->lines[i - 1].ptr;
		size_t alen = a->lines[i - 1].len;
		size_t *swap;

		cur[0] = 0;
		for (j = 1; j <= nb; j++) {
			const struct udiff_line *lb = &b->lines[j - 1];

			/*
			 * The first-byte test keeps memcmp off the hot path for
			 * the overwhelming majority of cell pairs.
			 */
			if (alen == lb->len && *ap == *lb->ptr &&
			    !memcmp(ap, lb->ptr, alen))
				cur[j] = prev[j - 1] + 1;
			else
				cur[j] = MAX(prev[j], cur[j - 1]);
		}
		swap = prev;
		prev = cur;
		cur = swap;
	}

	lcs = na ? prev[nb] : 0;
	printf("dp_cost=%lu\n", (unsigned long)((na - lcs) + (nb - lcs)));
}

/*
 * The format validator. It re-parses the emitted stream through
 * read_atatline_n, the same parser every tool in the tree uses on a hunk
 * header, and then walks the bodies: declared counts against real marker
 * counts, lawful markers, lawful incomplete-line marker placement, and ordering
 * on both sides. Window non-overlap is enforced outright even though it isn't
 * universal for the format (GNU diff 3.12 under -u -B really does emit
 * overlapping old windows), since nothing here ever runs with -B.
 */
static const char no_newline_text[] = "\\ No newline at end of file";

struct side_state {
	unsigned long start; /* previous window's 0-based first line */
	unsigned long end; /* previous window's 0-based end (exclusive) */
	bool seen; /* a previous window exists */
	bool closed; /* an incomplete-line marker ended this side */
};

static void validate_side(struct side_state *st, const char *which,
			  unsigned long start, unsigned long count,
			  unsigned long hunk)
{
	if (st->seen) {
		if (start <= st->start)
			die("hunk %lu's %s start %lu doesn't ascend past %lu",
			    hunk, which, start, st->start);
		if (start < st->end)
			die("hunk %lu's %s window starts at %lu, inside the previous window ending at %lu",
			    hunk, which, start, st->end);
	}

	st->start = start;
	st->end = start + count;
	st->seen = true;
}

/*
 * Charge the body line to each side it occupies. The caller keeps the marker to
 * associate a following incomplete-line note with those same sides.
 */
static void count_body_line(char marker, unsigned long hunk,
			    unsigned long *old_left, unsigned long *new_left,
			    struct side_state *old_st,
			    struct side_state *new_st)
{
	if (marker == ' ' || marker == '-') {
		if (!*old_left)
			die("hunk %lu has more old-side lines than its header declared",
			    hunk);
		if (old_st->closed)
			die("hunk %lu carries an old-side line past the old file's incomplete-line marker",
			    hunk);
		(*old_left)--;
	}
	if (marker == ' ' || marker == '+') {
		if (!*new_left)
			die("hunk %lu has more new-side lines than its header declared",
			    hunk);
		if (new_st->closed)
			die("hunk %lu carries a new-side line past the new file's incomplete-line marker",
			    hunk);
		(*new_left)--;
	}
}

static void close_sides(char prev_marker, unsigned long hunk,
			struct side_state *old_st, struct side_state *new_st)
{
	if (prev_marker == ' ' || prev_marker == '-') {
		if (old_st->closed)
			die("hunk %lu repeats the old file's incomplete-line marker",
			    hunk);
		old_st->closed = true;
	}
	if (prev_marker == ' ' || prev_marker == '+') {
		if (new_st->closed)
			die("hunk %lu repeats the new file's incomplete-line marker",
			    hunk);
		new_st->closed = true;
	}
}

/*
 * Peel one LF-terminated line off the stream. The stream's last byte is always
 * an LF, so a missing one is itself a breach.
 */
static size_t peel_line(const char *buf, size_t len, size_t at, size_t *llen)
{
	const char *nl;

	nl = memchr(buf + at, '\n', len - at);
	if (!nl)
		die("the emitted stream's last line lacks its newline");

	*llen = (nl - (buf + at));

	return (size_t)(nl - buf) + 1;
}

static void validate_stream(const char *buf, size_t len, unsigned long hunks)
{
	struct side_state old_st, new_st;
	unsigned long hunk = 0;

	old_st = (typeof(old_st)){};
	new_st = (typeof(new_st)){};

	for (size_t at = 0; at < len;) {
		unsigned long ostart, ocount, nstart, ncount;
		unsigned long old_left, new_left;
		char prev_marker = 0;
		size_t llen;
		size_t next;
		int r;

		next = peel_line(buf, len, at, &llen);
		r = read_atatline_n(buf + at, llen, &ostart, &ocount, &nstart,
				    &ncount);
		if (r)
			die("hunk %lu's header %s: %.*s", hunk + 1,
			    r == ATAT_NOT_HEADER ? "isn't one" : "is malformed",
			    (int)llen, buf + at);
		hunk++;
		at = next;

		/*
		 * A nonzero count prints a 1-based start; a zero count prints
		 * the 0-based index of the gap. Normalize both to 0-based.
		 */
		if (ocount && !ostart)
			die("hunk %lu declares %lu old lines starting at 0",
			    hunk, ocount);
		if (ncount && !nstart)
			die("hunk %lu declares %lu new lines starting at 0",
			    hunk, ncount);
		validate_side(&old_st, "old", ocount ? ostart - 1 : ostart,
			      ocount, hunk);
		validate_side(&new_st, "new", ncount ? nstart - 1 : nstart,
			      ncount, hunk);

		if (!ocount && !ncount)
			die("hunk %lu declares no lines on either side", hunk);

		old_left = ocount;
		new_left = ncount;
		while (old_left || new_left) {
			char marker;

			if (at >= len)
				die("hunk %lu's body ends with %lu old and %lu new lines still declared",
				    hunk, old_left, new_left);
			next = peel_line(buf, len, at, &llen);
			marker = buf[at];

			if (marker == '\\') {
				if (llen != sizeof(no_newline_text) - 1 ||
				    memcmp(buf + at, no_newline_text, llen))
					die("hunk %lu has an unrecognized backslash line: %.*s",
					    hunk, (int)llen, buf + at);
				if (!prev_marker)
					die("hunk %lu opens with an incomplete-line marker",
					    hunk);
				close_sides(prev_marker, hunk, &old_st,
					    &new_st);
				prev_marker = 0;
				at = next;
				continue;
			}

			if (marker != ' ' && marker != '-' && marker != '+')
				die("hunk %lu has an unlawful body marker '%c'",
				    hunk, marker);
			count_body_line(marker, hunk, &old_left, &new_left,
					&old_st, &new_st);
			prev_marker = marker;
			at = next;
		}

		/* The final body line's marker can trail the counts */
		if (at < len && buf[at] == '\\') {
			next = peel_line(buf, len, at, &llen);
			if (llen != sizeof(no_newline_text) - 1 ||
			    memcmp(buf + at, no_newline_text, llen))
				die("hunk %lu has an unrecognized backslash line: %.*s",
				    hunk, (int)llen, buf + at);
			if (!prev_marker)
				die("hunk %lu's trailing incomplete-line marker follows no body line",
				    hunk);
			close_sides(prev_marker, hunk, &old_st, &new_st);
			at = next;
		}
	}

	if (hunk != hunks)
		die("the stream carries %lu hunks but the result reported %lu",
		    hunk, hunks);
}

/*
 * Instrument reporting.
 */

/*
 * Results are freed between iterations, so the final status is saved while its
 * result lives.
 */
struct status_snap {
	bool equal;
	unsigned long hunks;
	size_t out_len;
};

static void snap_status(struct status_snap *s, const struct udiff_result *r)
{
	s->equal = r->equal;
	s->hunks = r->hunks;
	s->out_len = r->out_len;
}

static void print_status(const struct status_snap *s)
{
	fprintf(stderr, "status: equal=%d hunks=%lu out_len=%zu\n", s->equal,
		s->hunks, s->out_len);
}

static void print_stream(const struct udiff_result *r)
{
	if (!r->out_len)
		return;

	if (fwrite(r->out_buf, 1, r->out_len, stdout) != r->out_len)
		edie(errno, "writing the hunk stream");
}

/* An empty hunk list must agree with the engine's equality result */
static void run_pair(const struct operand *a, const struct operand *b,
		     unsigned int context, bool want_names,
		     struct udiff_result *r)
{
	udiff_run(&a->img, &b->img, context, want_names ? name_stub : NULL,
		  want_names ? (void *)&a->img : NULL, r);
	if (!r->hunks && !r->equal)
		die("udiff_run() emitted no hunks without reporting equal");

	if (r->equal && (r->out_buf || r->out_len))
		die("udiff_run() reported equal but left output behind");
}

static void usage(void)
{
	fputs("usage: udiff_driver [-U N]\n"
	      "                    [--loader getline|lineinfo] [--name-stub]\n"
	      "                    [--dp-cost] [--validate]\n"
	      "                    [--dump-lines] [--many N] [--concurrent]\n"
	      "                    A B [C D]\n",
	      stderr);
	exit(EXIT_USAGE);
}

enum {
	OPT_LOADER = 256,
	OPT_NAME_STUB,
	OPT_DP_COST,
	OPT_VALIDATE,
	OPT_DUMP_LINES,
	OPT_MANY,
	OPT_CONCURRENT
};

static const struct option long_opts[] = {
	{ "loader", 1, NULL, OPT_LOADER },
	{ "name-stub", 0, NULL, OPT_NAME_STUB },
	{ "dp-cost", 0, NULL, OPT_DP_COST },
	{ "validate", 0, NULL, OPT_VALIDATE },
	{ "dump-lines", 0, NULL, OPT_DUMP_LINES },
	{ "many", 1, NULL, OPT_MANY },
	{ "concurrent", 0, NULL, OPT_CONCURRENT },
	{ NULL, 0, NULL, 0 }
};

static unsigned long parse_count(const char *s, const char *what)
{
	unsigned long v;
	char *end;

	errno = 0;
	v = strtoul(s, &end, 10);
	if (errno || !*s || *end)
		error(EXIT_USAGE, 0, "%s wants a number, not \"%s\"", what, s);

	return v;
}

int main(int argc, char *argv[])
{
	bool want_names = false, want_dp = false, want_validate = false;
	bool want_dump = false, concurrent = false;
	enum loader loader = LOADER_GETLINE;
	unsigned int context = 3;
	unsigned long many = 0;
	struct operand op[4];
	int exit_code = 0;
	int nops, i, c;

	/*
	 * Before the memory bound, so the bound's own diagnostic carries the
	 * canonical prefix too; progname otherwise degrades to "(unset)".
	 * set_progname() keeps the caller's literal instead of copying it, so
	 * it allocates nothing.
	 */
	set_progname("udiff_driver");

	/*
	 * Nothing above allocates, so the footprint the bound builds on
	 * reflects entry state alone.
	 */
	mem_limit_init();

	while ((c = getopt_long(argc, argv, "U:", long_opts, NULL)) != -1) {
		switch (c) {
		case 'U':
			context = parse_count(optarg, "-U");
			break;
		case OPT_LOADER:
			if (!strcmp(optarg, "getline"))
				loader = LOADER_GETLINE;
			else if (!strcmp(optarg, "lineinfo"))
				loader = LOADER_LINEINFO;
			else
				error(EXIT_USAGE, 0,
				      "--loader takes getline or lineinfo, not \"%s\"",
				      optarg);
			break;
		case OPT_NAME_STUB:
			want_names = true;
			break;
		case OPT_DP_COST:
			want_dp = true;
			break;
		case OPT_VALIDATE:
			want_validate = true;
			break;
		case OPT_DUMP_LINES:
			want_dump = true;
			break;
		case OPT_MANY:
			many = parse_count(optarg, "--many");
			if (!many)
				error(EXIT_USAGE, 0,
				      "--many wants at least one iteration");
			break;
		case OPT_CONCURRENT:
			concurrent = true;
			break;

		default:
			usage();
		}
	}

	nops = concurrent ? 4 : 2;
	if (nops != argc - optind)
		usage();
	if (concurrent && (many || want_dump || want_dp))
		error(EXIT_USAGE, 0,
		      "--concurrent doesn't combine with --many, --dump-lines, or --dp-cost");

	memset(op, 0, sizeof(op));
	for (i = 0; i < nops; i++) {
		op[i].path = argv[optind + i];
		load_operand(&op[i], loader);
	}

	if (want_dump) {
		/*
		 * No engine run: this mode exists to prove the two loader
		 * conventions materialize the same table.
		 */
		dump_lines(&op[0]);
		dump_lines(&op[1]);
	} else if (want_dp) {
		print_dp_cost(&op[0], &op[1]);
	} else if (concurrent) {
		struct udiff_result r1, r2;
		struct status_snap s1, s2;

		/* Both results are live at once on purpose */
		run_pair(&op[0], &op[1], context, want_names, &r1);
		run_pair(&op[2], &op[3], context, want_names, &r2);
		print_stream(&r1);
		print_stream(&r2);
		snap_status(&s1, &r1);
		snap_status(&s2, &r2);
		udiff_result_free(&r1);
		udiff_result_free(&r2);
		print_status(&s1);
		print_status(&s2);
		if (s1.hunks || s2.hunks)
			exit_code = 1;
	} else {
		unsigned long iter, iters = many ? many : 1;
		struct udiff_result r;
		struct status_snap s;

		s = (typeof(s)){};
		for (iter = 0; iter < iters; iter++) {
			run_pair(&op[0], &op[1], context, want_names, &r);

			/*
			 * One stream is the product; the rest of a --many run
			 * just exercises repeated run-and-destroy over the same
			 * pair.
			 */
			if (!iter)
				print_stream(&r);
			if (want_validate && r.out_len)
				validate_stream(r.out_buf, r.out_len, r.hunks);
			snap_status(&s, &r);
			udiff_result_free(&r);
		}
		print_status(&s);
		if (s.hunks)
			exit_code = 1;
	}

	if (fflush(stdout) || ferror(stdout))
		edie(errno, "writing stdout");

	for (i = 0; i < nops; i++)
		operand_free(&op[i]);

	return exit_code;
}
