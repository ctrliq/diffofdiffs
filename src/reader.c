// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ctrl IQ, Inc.
 *
 * Unified-diff validation, file indexing, and filename correspondence. Both
 * parser passes share the same line grammar.
 */
#define _GNU_SOURCE

#include <error.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <time.h>

#include <iomem.h>
#include <patch-types.h>
#include <reader.h>
#include <util.h>

/*
 * Suffix matching needs a directory component as well as the basename. A
 * basename alone can pair unrelated files in different directories.
 */
#define PAIR_MIN_COMPONENTS 2

static bool line_starts(const struct iomem_line *line, const char *prefix)
{
	size_t len = strlen(prefix);

	return line->len >= len && !memcmp(line->base, prefix, len);
}

/*
 * Compare the full name, including any embedded NUL: a path merely beginning
 * with /dev/null is not the absent-file marker.
 */
static bool name_is_dev_null(const char *name, size_t len)
{
	return len == sizeof("/dev/null") - 1 &&
	       !memcmp(name, "/dev/null", len);
}

static int removable_path_components(const char *name)
{
	int num = 0;

	while ((name = strchr(name, '/'))) {
		for (; *name == '/'; name++)
			;
		num++;
	}

	return num;
}

/*
 * Repeated slashes count as one separator. Excessive stripping stops at the
 * basename, while /dev/null always keeps its special meaning.
 */
const char *stripped(const char *name, int num_components)
{
	const char *basename = strrchr(name, '/');

	if (name_is_dev_null(name, strlen(name)))
		return name;

	if (basename)
		basename++;
	else
		basename = name;

	for (int i = 0; i < num_components && (name = strchr(name, '/')); i++) {
		for (; *name == '/'; name++)
			;
	}

	return name ? name : basename;
}

/*
 * Ignore /dev/null, then prefer fewer path components, a shorter basename, and
 * a shorter full name, in that order. Equal candidates keep the first name,
 * preserving the old-side preference for a header pair.
 */
const struct patch_name *best_patch_name(const struct patch_name *oldname,
					 const struct patch_name *newname)
{
	const struct patch_name *names[2] = { oldname, newname };
	size_t best_baselen = SIZE_MAX, best_len = SIZE_MAX;
	int best_components = INT_MAX, best = 0;

	for (int i = 0; i < 2; i++) {
		const char *name = names[i]->text;
		size_t len = strlen(name), baselen;
		const char *base;
		int components;

		if (name_is_dev_null(name, len))
			continue;

		components = removable_path_components(name);
		base = strrchr(name, '/');
		baselen = base ? strlen(base + 1) : len;
		if (components > best_components)
			continue;

		if (components == best_components && baselen > best_baselen)
			continue;

		if (components == best_components && baselen == best_baselen &&
		    len >= best_len)
			continue;

		best_components = components;
		best_baselen = baselen;
		best_len = len;
		best = i;
	}

	return names[best];
}

struct patch_name *patch_name_dup(const struct patch_name *name)
{
	struct patch_name *copy = xmalloc(sizeof(*copy));

	*copy = *name;
	copy->text = memdup(name->text, name->len);
	return copy;
}

struct patch_name *patch_name_strip(const struct patch_name *name, int depth)
{
	const char *text = stripped(name->text, depth);
	struct patch_name part = { .text = (char *)text,
				   .len = name->len - (text - name->text),
				   .verbatim = name->verbatim };

	return patch_name_dup(&part);
}

void patch_name_free(struct patch_name *name)
{
	if (!name)
		return;

	free(name->text);
	free(name);
}

/*
 * Recognize common diff timestamps to locate the filename boundary. The
 * fractional seconds and timezone need no parsing once the prefix matches.
 */
static bool header_timestamp(const char *s)
{
	struct tm tm = {};

	s += strspn(s, " \t");

	return strptime(s, "%Y-%m-%d %H:%M:%S", &tm) ||
	       strptime(s, "%a %b %e %T %Y", &tm) ||
	       strptime(s, "%b %Y %H:%M:%S", &tm);
}

static size_t skip_blank(const char *s, size_t len, size_t at)
{
	for (; at < len && (s[at] == ' ' || s[at] == '\t'); at++)
		;

	return at;
}

/* The offset of the first blank or line ending, or len when there is none */
static size_t skip_to_blank(const char *s, size_t len, size_t at)
{
	for (; at < len && s[at] != ' ' && s[at] != '\t' && s[at] != '\n' &&
	       s[at] != '\r';
	     at++)
		;

	return at;
}

static bool octal_digit(char c)
{
	return c >= '0' && c <= '7';
}

/*
 * Decode Git's quoted path into an owned allocation. Octal escapes must have
 * exactly three digits and fit in one nonzero byte. Invalid quoting returns
 * NULL so the caller can preserve the original label instead of guessing its
 * identity. Decoded separators participate in path matching like literal ones.
 */
static char *git_unquote(const char *name, size_t len, size_t *outlen)
{
	char *out;
	size_t n = 0;

	if (len < 2 || name[0] != '"')
		return NULL;

	out = xmalloc(len);
	for (size_t in = 1; in < len;) {
		char c = name[in++];

		if (c == '"') {
			if (in != len)
				goto malformed;
			out[n] = '\0';
			*outlen = n;
			return out;
		}

		if (c == '\\') {
			if (in >= len)
				goto malformed;

			c = name[in++];
			switch (c) {
			case '"':
			case '\\':
				break;
			case 'a':
				c = '\a';
				break;
			case 'b':
				c = '\b';
				break;
			case 'f':
				c = '\f';
				break;
			case 'n':
				c = '\n';
				break;
			case 'r':
				c = '\r';
				break;
			case 't':
				c = '\t';
				break;
			case 'v':
				c = '\v';
				break;
			case '0':
			case '1':
			case '2':
			case '3':
				if (in + 2 > len || !octal_digit(name[in]) ||
				    !octal_digit(name[in + 1]))
					goto malformed;
				c = ((c - '0') << 6) | ((name[in] - '0') << 3) |
				    (name[in + 1] - '0');
				in += 2;
				if (!c)
					goto malformed;
				break;
			default:
				goto malformed;
			}
		}

		out[n++] = c;
	}

malformed:
	free(out);
	return NULL;
}

/*
 * A tab, trailing blanks, or a timestamp ends the name. Spaces within it can
 * belong to the path, so probe the suffix before treating them as a delimiter.
 *
 * strptime() lets format whitespace match newlines. A terminated copy keeps its
 * timestamp probe from consuming the next patch line; length-based name parsing
 * still preserves embedded NUL bytes for later validation.
 */
struct patch_name *filename_from_header(const char *base, size_t len)
{
	char *header __free(free) = memdup(base, len);
	size_t first_blank = skip_to_blank(header, len, 0);
	size_t at;

	for (at = first_blank; at < len && header[at] == ' ';) {
		size_t past;

		past = skip_blank(header, len, at);
		if (past >= len || header_timestamp(header + past))
			break;

		at = skip_to_blank(header, len, past + 1);
	}

	if (at < len && (header[at] == '\n' || header[at] == '\r') &&
	    at > first_blank)
		at = first_blank;

	return unquoted_name(header, at);
}

struct patch_name *unquoted_name(const char *base, size_t len)
{
	struct patch_name *name = xmalloc(sizeof(*name));
	size_t outlen = len;
	char *text = git_unquote(base, len, &outlen);

	*name = (typeof(*name)){ .text = text ? text : memdup(base, len),
				 .len = outlen,
				 .verbatim = !text && len && base[0] == '"' };
	return name;
}

/*
 * The expected identity bounds an unquoted name even when it contains spaces. A
 * quoted token supplies its own boundary, so both spellings compare without
 * guessing which space separates the opener's two paths.
 */
static bool git_name_token(const char *base, size_t len,
			   const struct patch_name *name, size_t *used)
{
	struct patch_name *parsed __free(patch_name) = NULL;
	size_t end;

	if (!len || name->verbatim)
		return false;

	if (base[0] != '"') {
		if (len < name->len || memcmp(base, name->text, name->len))
			return false;

		*used = name->len;
		return true;
	}

	for (end = 1; end < len;) {
		char c = base[end++];

		if (c == '"')
			break;

		if (c == '\\' && end < len)
			end++;
	}

	parsed = unquoted_name(base, end);
	if (parsed->verbatim || parsed->len != name->len ||
	    memcmp(parsed->text, name->text, name->len))
		return false;

	*used = end;
	return true;
}

bool git_block_names(const struct iomem_line *line,
		     const struct patch_name *oldname,
		     const struct patch_name *newname)
{
	size_t first, second, len;
	const char *base;

	if (!line_starts(line, "diff --git "))
		return false;

	base = line->base + 11;
	len = chomp_header(base, line->len - 11);
	if (!git_name_token(base, len, oldname, &first) || first >= len ||
	    base[first] != ' ')
		return false;

	first++;
	return git_name_token(base + first, len - first, newname, &second) &&
	       first + second == len;
}

/*
 * Search backward only through leading metadata. Crossing a file or hunk header
 * would attach a previous section's opener to the current record.
 */
long block_opener_above(const struct iomem_buf *buf, size_t at)
{
	const char *base = buf->base;

	while (at) {
		const char *nl = memrchr(base, '\n', at - 1);
		size_t start = nl ? (size_t)(nl - base) + 1 : 0;

		if (!strncmp(base + start, "diff --git ", 11))
			return start;

		if (!strncmp(base + start, "--- ", 4) ||
		    !strncmp(base + start, "+++ ", 4) ||
		    !strncmp(base + start, "@@ ", 3))
			return -1;

		at = start;
	}

	return -1;
}

/* Rename and copy metadata supplies paths without display prefixes */
static void git_story_paths(const struct iomem_buf *buf,
			    const struct iomem_line *opener,
			    struct patch_name **src, struct patch_name **dst)
{
	const struct iomem_slice *from, *to;
	struct block_story story;

	block_story_read(&story, buf, opener->offset, "patch");
	if (story.rename_from.base && story.rename_to.base) {
		from = &story.rename_from;
		to = &story.rename_to;
	} else if (story.copy_from.base && story.copy_to.base) {
		from = &story.copy_from;
		to = &story.copy_to;
	} else {
		return;
	}

	*src = unquoted_name(from->base, from->len);
	*dst = unquoted_name(to->base, to->len);
}

static bool same_path(const struct patch_name *a, const struct patch_name *b)
{
	return !a->verbatim && !b->verbatim && a->len == b->len &&
	       !memcmp(a->text, b->text, a->len);
}

static bool prefixed_path(const struct patch_name *name,
			  const struct patch_name *path, char prefix)
{
	return !name->verbatim && !path->verbatim &&
	       name->len == path->len + 2 && name->text[0] == prefix &&
	       name->text[1] == '/' &&
	       !memcmp(name->text + 2, path->text, path->len);
}

static bool git_prefixed_paths(const struct iomem_line *opener,
			       const struct patch_name *src,
			       const struct patch_name *dst)
{
	struct patch_name oldname, newname;
	char *old __free(free) = NULL;
	char *new __free(free) = NULL;

	if (src->verbatim || dst->verbatim || strlen(src->text) != src->len ||
	    strlen(dst->text) != dst->len)
		return false;

	xasprintf(&old, "a/%s", src->text);
	xasprintf(&new, "b/%s", dst->text);
	oldname = (typeof(oldname)){ .text = old, .len = src->len + 2 };
	newname = (typeof(newname)){ .text = new, .len = dst->len + 2 };
	return git_block_names(opener, &oldname, &newname);
}

/*
 * A leading a/ can be a real directory in a --no-prefix patch. The paired
 * labels establish the convention for an edit, while rename and copy metadata
 * establish it for an operation whose paths differ. The metadata takes
 * precedence because a literal a/x -> b/x rename otherwise looks prefixed.
 */
enum patch_prefix patch_name_prefix(const struct iomem_buf *buf, long pos,
				    const char *operand,
				    const struct patch_name *key)
{
	struct patch_name *oldname __free(patch_name) = NULL;
	struct patch_name *newname __free(patch_name) = NULL;
	struct patch_name *src __free(patch_name) = NULL;
	struct patch_name *dst __free(patch_name) = NULL;
	long start = block_opener_above(buf, pos);
	struct iomem_line opener = {}, line;
	struct iomem_cursor cur;

	if (key->verbatim)
		return PATCH_PREFIX_UNKNOWN;

	iomem_cursor_init(&cur, buf);
	if (start >= 0) {
		iomem_cursor_seek(&cur, start, operand);
		iomem_cursor_next(&cur, &opener);
		git_story_paths(buf, &opener, &src, &dst);
	}

	iomem_cursor_seek(&cur, pos, operand);
	while (iomem_cursor_next(&cur, &line)) {
		if (line_starts(&line, "diff --git ") ||
		    line_starts(&line, "@@ "))
			break;

		if (!line_starts(&line, "--- "))
			continue;

		oldname = filename_from_header(line.base + 4, line.len - 4);
		if (iomem_cursor_next(&cur, &line) &&
		    line_starts(&line, "+++ "))
			newname = filename_from_header(line.base + 4,
						       line.len - 4);
		break;
	}

	if (src) {
		/* Operation paths are literal and disambiguate the headers */
		if (git_block_names(&opener, src, dst) &&
		    (!oldname || same_path(oldname, src)) &&
		    (!newname || same_path(newname, dst)))
			return PATCH_PREFIX_LITERAL;

		if (git_prefixed_paths(&opener, src, dst) &&
		    (!oldname || prefixed_path(oldname, src, 'a')) &&
		    (!newname || prefixed_path(newname, dst, 'b')) &&
		    (prefixed_path(key, src, 'a') ||
		     prefixed_path(key, dst, 'b')))
			return PATCH_PREFIX_GIT;

		return PATCH_PREFIX_UNKNOWN;
	}

	if (oldname && newname) {
		if (same_path(oldname, newname)) {
			if (!opener.base ||
			    git_block_names(&opener, oldname, newname))
				return PATCH_PREFIX_LITERAL;

			return PATCH_PREFIX_UNKNOWN;
		}

		if (!oldname->verbatim && !newname->verbatim &&
		    !strncmp(oldname->text, "a/", 2) &&
		    !strncmp(newname->text, "b/", 2) &&
		    !strcmp(oldname->text + 2, newname->text + 2) &&
		    (!opener.base ||
		     git_block_names(&opener, oldname, newname)))
			return PATCH_PREFIX_GIT;
	}

	if (git_block_names(&opener, key, key))
		return PATCH_PREFIX_LITERAL;

	if (key->len >= 2 && (key->text[0] == 'a' || key->text[0] == 'b') &&
	    key->text[1] == '/') {
		struct patch_name path = { .text = key->text + 2,
					   .len = key->len - 2 };

		if (git_prefixed_paths(&opener, &path, &path))
			return PATCH_PREFIX_GIT;
	}

	/*
	 * With one /dev/null label, a bare header cannot prove the prefix.
	 * Leave that decision to evidence from other files in the same operand.
	 */
	if (!opener.base && oldname && newname && key->len > 2 &&
	    key->text[1] == '/' &&
	    ((name_is_dev_null(oldname->text, oldname->len) &&
	      key->text[0] == 'b') ||
	     (name_is_dev_null(newname->text, newname->len) &&
	      key->text[0] == 'a')))
		return PATCH_PREFIX_INCOMPLETE;

	return PATCH_PREFIX_UNKNOWN;
}

/*
 * Follow Git's default core.quotePath escaping, preserving undecodable labels
 * verbatim. A decoded path beginning with a quote still needs normal escaping.
 */
char *git_quote_name(const struct patch_name *path)
{
	bool quote = false;
	const char *name;
	char *out, *p;
	size_t len;

	if (!path)
		return NULL;

	name = path->text;
	len = strlen(name);
	if (!path->verbatim) {
		for (size_t i = 0; i < len && !quote; i++) {
			char c = name[i];

			quote = c == '"' || c == '\\' || c < 0x20 || c >= 0x7f;
		}
	}

	/* Every byte may need four for octal escaping, plus quotes and NUL */
	out = xmalloc(len * 4 + 3);
	p = out;
	if (!quote) {
		memcpy(p, name, len);
		p += len;
	} else {
		*p++ = '"';
		for (size_t i = 0; i < len; i++) {
			char c = name[i];

			if (c == '"' || c == '\\') {
				*p++ = '\\';
				*p++ = c;
			} else if (c >= '\a' && c <= '\r') {
				*p++ = '\\';
				*p++ = "abtnvfr"[c - '\a'];
			} else if (c < 0x20 || c >= 0x7f) {
				*p++ = '\\';
				*p++ = '0' + (c >> 6);
				*p++ = '0' + ((c >> 3) & 7);
				*p++ = '0' + (c & 7);
			} else {
				*p++ = c;
			}
		}
		*p++ = '"';
	}

	*p = '\0';

	return out;
}

/*
 * Cursor lines omit LF, but headers read before normalization may retain CR.
 * Neither line-ending byte belongs to the filename or metadata field.
 */
size_t chomp_header(const char *base, size_t len)
{
	size_t at;

	for (at = 0; at < len; at++) {
		if (base[at] == '\r' || base[at] == '\n')
			break;
	}

	return at;
}

/*
 * Check the coordinate ceiling before each multiplication. Unlike strtoul(),
 * this parser rejects signs rather than converting negative offsets to large
 * unsigned values.
 */
static int read_atat_num(const char *s, size_t len, size_t *at,
			 unsigned long *num)
{
	unsigned long val = 0;
	size_t i;

	for (i = *at; i < len && s[i] >= '0' && s[i] <= '9'; i++) {
		unsigned long digit = s[i] - '0';

		if (val > (ATAT_MAX_COORD - digit) / 10)
			return ATAT_MALFORMED;

		val = val * 10 + digit;
	}

	if (i == *at)
		return ATAT_MALFORMED;

	*at = i;
	*num = val;
	return 0;
}

/*
 * One side of an @@ line, an offset with an optional count behind a comma and
 * blanks tolerated around either. A side with no comma covers one line.
 */
static int read_atat_side(const char *s, size_t len, size_t *at,
			  unsigned long *offset, unsigned long *count)
{
	unsigned long off, cnt = 1;
	int ret;

	*at = skip_blank(s, len, *at);
	ret = read_atat_num(s, len, at, &off);
	if (ret)
		return ret;

	*at = skip_blank(s, len, *at);
	if (*at < len && s[*at] == ',') {
		*at = skip_blank(s, len, *at + 1);
		ret = read_atat_num(s, len, at, &cnt);
		if (ret)
			return ret;

		*at = skip_blank(s, len, *at);
	}

	if (offset)
		*offset = off;
	if (count)
		*count = cnt;

	return 0;
}

int read_atatline_n(const char *base, size_t len, unsigned long *orig_offset,
		    unsigned long *orig_count, unsigned long *new_offset,
		    unsigned long *new_count)
{
	size_t at;
	int ret;

	if (len < 3 || base[0] != '@' || base[1] != '@' ||
	    (base[2] != ' ' && base[2] != '\t'))
		return ATAT_NOT_HEADER;

	at = skip_blank(base, len, 3);
	if (at >= len || base[at] != '-')
		return ATAT_NOT_HEADER;

	at++;

	ret = read_atat_side(base, len, &at, orig_offset, orig_count);
	if (ret)
		return ret;

	if (at >= len || base[at] != '+')
		return ATAT_MALFORMED;

	at++;

	ret = read_atat_side(base, len, &at, new_offset, new_count);
	if (ret)
		return ret;

	/*
	 * A hand-edited header may omit its closing '@' characters. Other text
	 * still needs that delimiter, rather than being part of a coordinate.
	 */
	if (at < chomp_header(base, len) && base[at] != '@')
		return ATAT_MALFORMED;

	return 0;
}

size_t atat_tail(const char *base, size_t len, const char **tail)
{
	size_t end = chomp_header(base, len);
	const char *first, *second;

	*tail = "";
	first = memmem(base, end, "@@", 2);
	if (!first)
		return 0;

	first += 2;
	second = memmem(first, end - (first - base), "@@", 2);
	if (!second)
		return 0;

	second += 2;
	*tail = second;
	return end - (second - base);
}

/*
 * Accept context with a missing leading space when it starts with a tab or is
 * empty. In those forms every byte belongs to the source, unlike the explicit
 * context and edit signs which occupy one patch-only byte.
 *
 * A leading CR is not a context sign. Section-specific CRLF normalization has
 * already removed line-ending CR bytes before this parser runs.
 */
enum body_class classify_body_line(const char *base, size_t len,
				   size_t *content)
{
	enum body_class class = BODY_CONTEXT;
	size_t at = 0;

	if (len) {
		switch (base[0]) {
		case ' ':
			at = 1;
			break;
		case '\t':
			break;
		case '-':
			at = 1;
			class = BODY_REMOVED;
			break;
		case '+':
			at = 1;
			class = BODY_ADDED;
			break;
		case '\\':
			class = BODY_NO_NEWLINE;
			break;
		default:
			class = BODY_JUNK;
			break;
		}
	}

	if (content)
		*content = at;

	return class;
}

/*
 * printf precision takes an int even though input lengths use size_t.
 */
void malformed_patch_line(const char *base, size_t len)
{
	int show = len > INT_MAX ? INT_MAX : (int)len;

	die("malformed patch: %.*s", show, base);
}

void check_hunk_chop(unsigned long old_left, unsigned long new_left)
{
	if (new_left > 3)
		die("unexpected end of file in patch");

	if (old_left != new_left)
		die("malformed patch: hunk short by %lu old and %lu new lines",
		    old_left, new_left);
}

/*
 * Even a truncated hunk must contain an edit; a bare header is not a change.
 */
static void hunk_without_change(unsigned long hunkline, const char *which)
{
	die("hunk without '-' or '+' lines at line %lu of %s", hunkline, which);
}

/*
 * Normalize CRLF per file section, not per operand, so adjacent CRLF and LF
 * sections can coexist. The caller decides whether this line's CR is part of
 * the patch framing or source content.
 *
 * Compaction moves bytes only toward the front. Unread input remains intact,
 * and saved views of compacted headers remain valid. The buffer length must
 * stay unchanged until the cursor finishes reading the original input.
 */
static void compact_line(struct iomem_buf *buf, struct iomem_line *line,
			 size_t *at, bool drop_cr)
{
	size_t keep = line->len - drop_cr;

	if (*at != line->offset)
		memmove(buf->base + *at, line->base, keep);

	line->base = buf->base + *at;
	line->len = keep;
	line->offset = *at;
	*at += keep;
	if (line->has_lf)
		buf->base[(*at)++] = '\n';
}

/*
 * Recognize operation-only input that has no text hunks. Require a Git opener
 * so ordinary prose mentioning a rename or copy does not qualify by itself.
 *
 * A binary summary additionally needs a decoded filename; without one, the
 * input cannot produce a file record and must not silently disappear.
 */
static bool git_fileop_patch(const struct iomem_buf *buf)
{
	struct iomem_cursor cur;
	struct iomem_line line;
	bool named = false;
	bool armed = false;

	iomem_cursor_init(&cur, buf);
	while (iomem_cursor_next(&cur, &line)) {
		if (line_starts(&line, "diff --git ")) {
			struct patch_name *name __free(patch_name) =
				git_block_name(buf, &line);

			armed = true;
			named = name != NULL;
			continue;
		}

		if (!armed)
			continue;

		if (line_starts(&line, "--- ")) {
			named = false;
			continue;
		}

		if (line_starts(&line, "rename from ") ||
		    line_starts(&line, "rename to ") ||
		    line_starts(&line, "copy from ") ||
		    line_starts(&line, "copy to ") ||
		    line_starts(&line, "GIT binary patch"))
			return true;

		if (named && line_is_binary_word(line.base, line.len))
			return true;
	}

	return false;
}

struct hunk_progress {
	unsigned long old_left;
	unsigned long new_left;
	unsigned long header_line;
	int markers;
	bool active;
	bool changed;
};

/*
 * Consume only the coordinates present on this row. Exceeding either declared
 * count is malformed. A no-newline marker may follow the last row of a stage; a
 * context row can finish both stages and therefore permit two markers.
 */
static void spend_body_line(struct hunk_progress *hunk, enum body_class class,
			    const struct iomem_line *line)
{
	switch (class) {
	case BODY_CONTEXT:
		if (!hunk->old_left || !hunk->new_left)
			break;

		hunk->markers = (hunk->old_left == 1) + (hunk->new_left == 1);
		hunk->old_left--;
		hunk->new_left--;
		return;

	case BODY_REMOVED:
		if (!hunk->old_left)
			break;

		hunk->markers = hunk->old_left == 1;
		hunk->old_left--;
		hunk->changed = true;
		return;

	case BODY_ADDED:
		if (!hunk->new_left)
			break;

		hunk->markers = hunk->new_left == 1;
		hunk->new_left--;
		hunk->changed = true;
		return;

	case BODY_NO_NEWLINE:
	case BODY_JUNK:
		break;
	}

	malformed_patch_line(line->base, line->len);
}

static bool validate_hunk_body(struct hunk_progress *hunk,
			       const struct iomem_line *line, const char *which)
{
	enum body_class class;

	if (!hunk->active)
		return false;

	class = classify_body_line(line->base, line->len, NULL);

	/* A final source row's EOF marker still belongs to this hunk */
	if (class == BODY_NO_NEWLINE && hunk->markers) {
		hunk->markers--;
		return true;
	}

	if (hunk->old_left || hunk->new_left) {
		spend_body_line(hunk, class, line);
		return true;
	}

	/*
	 * The first line outside the hunk must still reach the section parser.
	 */
	if (!hunk->changed)
		hunk_without_change(hunk->header_line, which);
	hunk->active = false;
	return false;
}

static bool validate_hunk_header(struct hunk_progress *hunk,
				 const struct iomem_line *line,
				 unsigned long linenum)
{
	unsigned long old_count, new_count;
	int ret;

	ret = read_atatline_n(line->base, line->len, NULL, &old_count, NULL,
			      &new_count);

	/* A broken @@ header is an error; ordinary inter-hunk text is not */
	if (ret == ATAT_MALFORMED)
		malformed_patch_line(line->base, line->len);
	if (ret)
		return false;

	*hunk = (typeof(*hunk)){ .old_left = old_count,
				 .new_left = new_count,
				 .header_line = linenum,
				 .active = true };
	return true;
}

/*
 * Save the first unsupported context-diff header. Delay the diagnostic so a
 * wholly foreign input gets the simpler "doesn't contain a patch" error, while
 * a mixed input cannot silently lose its context-diff section.
 */
static void keep_foreign_half(struct iomem_line *foreign,
			      const struct iomem_line *half, bool open,
			      bool starred)
{
	if (open && starred && !foreign->base)
		*foreign = *half;
}

/*
 * Normalize line endings before any consumer saves offsets. Validate hunk
 * counts only inside a file section introduced by adjacent ---/+++ headers.
 * Text between hunks does not close the section, so later hunks are checked
 * too.
 *
 * Metadata-only Git changes count as patches through the same block state used
 * by indexing. Unrecognized preamble is ignored, but unsupported context diffs
 * mixed with unified sections must be diagnosed rather than dropped.
 */
void validate_patch(struct iomem_buf *buf, const char *name, const char *which)
{
	bool star_here = false, star_above = false, half_star = false;
	bool pre_minus = false, header_seen = false, strip_cr = false;
	bool saw_hunk = false, saw_mode_only = false;
	struct iomem_line half = {}, foreign = {};
	struct hunk_progress hunk = {};
	struct block_latch latch = {};
	unsigned long linenum = 0;
	struct iomem_cursor cur;
	struct iomem_line line;
	size_t write_at = 0;

	iomem_cursor_init(&cur, buf);
	while (iomem_cursor_next(&cur, &line)) {
		struct patch_name *block_name __free(patch_name) = NULL;
		bool raw_cr, body, opener;

		/*
		 * Body text resembling a header cannot change CRLF handling.
		 * Outside a hunk, the Git opener and each file header set it
		 * from their own ending before compaction removes that CR.
		 */
		raw_cr = line.len && line.base[line.len - 1] == '\r';
		body = hunk.active &&
		       (hunk.old_left || hunk.new_left ||
			(hunk.markers && line.len && line.base[0] == '\\'));
		opener = !body && line_starts(&line, "diff --git ");

		/*
		 * Read metadata before moving the opener. CRLF compaction can
		 * leave stale bytes between it and the next unread line.
		 */
		if (opener)
			block_name = git_block_name(buf, &line);

		if (!body && (opener || line_starts(&line, "--- ") ||
			      line_starts(&line, "+++ ")))
			strip_cr = raw_cr;
		compact_line(buf, &line, &write_at, strip_cr && raw_cr);

		linenum++;

		/* Only the line directly above a half can mark it foreign */
		star_above = star_here;
		star_here = line_starts(&line, "*** ");
		if (validate_hunk_body(&hunk, &line, which))
			continue;

		if (header_seen &&
		    validate_hunk_header(&hunk, &line, linenum)) {
			/* File headers cannot pair across a hunk */
			keep_foreign_half(&foreign, &half, pre_minus,
					  half_star);
			pre_minus = false;
			saw_hunk = true;
			continue;
		}

		/*
		 * A metadata-only block must qualify under the same rules as
		 * indexing. Partial metadata cannot validate a file record that
		 * the index would omit.
		 */
		if (opener) {
			/*
			 * Finish the previous block before resetting its state.
			 */
			if (latch.armed && latch.complete)
				saw_mode_only = true;

			block_latch_opener(&latch, block_name != NULL);
		} else {
			block_latch_line(&latch, line.base, line.len);
		}

		if (line_starts(&line, "--- ")) {
			/* This section names a file, so it isn't mode-only */
			block_latch_opener(&latch, false);

			/* A half this line displaces met no "+++ " line */
			keep_foreign_half(&foreign, &half, pre_minus,
					  half_star);
			pre_minus = true;
			half = line;
			half_star = star_above;
			continue;
		}

		if (pre_minus && line_starts(&line, "+++ ")) {
			pre_minus = false;
			header_seen = true;
			continue;
		}

		keep_foreign_half(&foreign, &half, pre_minus, half_star);
		pre_minus = false;
	}

	/* The cursor is done, so its original length is no longer needed */
	buf->len = write_at;
	buf->base[write_at] = '\0';

	/* Check a pending --- header even when the input ends on it */
	keep_foreign_half(&foreign, &half, pre_minus, half_star);

	/* Only a small, equal context shortfall is allowed at EOF */
	if (hunk.active) {
		if (!hunk.changed)
			hunk_without_change(hunk.header_line, which);

		check_hunk_chop(hunk.old_left, hunk.new_left);
	}

	/* A mode-only block at the very end of the operand */
	if (latch.armed && latch.complete)
		saw_mode_only = true;

	/* Mixed diff formats must not silently lose an unsupported section */
	if (saw_hunk || saw_mode_only) {
		if (foreign.base)
			malformed_patch_line(foreign.base, foreign.len);
		return;
	}

	/*
	 * Empty input and recognized file operations may lack text hunks. Warn
	 * for those cases; reject other input instead of comparing it as empty.
	 */
	if (buf->len && !git_fileop_patch(buf))
		die("%s doesn't contain a patch", name);

	error(0, 0, "%s doesn't contain a patch", name);
}

static struct patch_name *git_opener_pair(const struct iomem_line *line,
					  size_t split)
{
	struct patch_name *oldname __free(patch_name) = NULL;
	struct patch_name *newname __free(patch_name) = NULL;
	size_t len = chomp_header(line->base, line->len);
	size_t opener_len = sizeof("diff --git ") - 1;
	size_t path_prefix_len = sizeof("a/") - 1;

	if (split >= len || line->base[split] != ' ')
		return NULL;

	oldname = unquoted_name(line->base + opener_len, split - opener_len);
	newname = unquoted_name(line->base + split + 1, len - split - 1);
	if (!git_block_names(line, oldname, newname))
		return NULL;

	/* Git's a/ and b/ prefixes have equal lengths and name the two sides */
	if (same_path(oldname, newname)) {
		newname->prefix = PATCH_PREFIX_LITERAL;
	} else if (!strncmp(oldname->text, "a/", path_prefix_len) &&
		   !strncmp(newname->text, "b/", path_prefix_len)) {
		if (!strcmp(oldname->text + path_prefix_len,
			    newname->text + path_prefix_len))
			newname->prefix = PATCH_PREFIX_GIT;
	} else {
		return NULL;
	}

	return no_free_ptr(newname);
}

/*
 * Unquoted paths can contain the same blanks as the separator. Metadata binds
 * both complete names for a rename or copy, while equal old/new paths bind a
 * mode-only block. Without either, only an unambiguous token pair is usable.
 */
struct patch_name *git_block_name(const struct iomem_buf *buf,
				  const struct iomem_line *line)
{
	struct patch_name *best __free(patch_name) = NULL;
	struct patch_name *src __free(patch_name) = NULL;
	struct patch_name *dst __free(patch_name) = NULL;
	size_t len = chomp_header(line->base, line->len);
	size_t opener_len = sizeof("diff --git ") - 1;
	const char *base = line->base;
	const char *separator;
	size_t split;

	git_story_paths(buf, line, &src, &dst);
	if (src && git_block_names(line, src, dst)) {
		dst->prefix = PATCH_PREFIX_LITERAL;
		return no_free_ptr(dst);
	}

	if (src && git_prefixed_paths(line, src, dst)) {
		char *text __free(free) = NULL;

		xasprintf(&text, "b/%s", dst->text);
		best = unquoted_name(text, dst->len + sizeof("b/") - 1);
		best->prefix = PATCH_PREFIX_GIT;
		return no_free_ptr(best);
	}

	if (len <= opener_len + 1)
		return NULL;

	if (base[opener_len] == '\"') {
		for (split = opener_len + 1; split < len;) {
			char c = base[split++];

			if (c == '\"')
				break;

			if (c == '\\' && split < len)
				split++;
		}

		return git_opener_pair(line, split);
	}

	separator = memmem(base + opener_len, len - opener_len, " \"",
			   sizeof(" \"") - 1);
	if (separator)
		return git_opener_pair(line, separator - base);

	split = opener_len + (len - opener_len) / 2;
	best = git_opener_pair(line, split);
	if (best && best->prefix != PATCH_PREFIX_UNKNOWN)
		return no_free_ptr(best);

	patch_name_free(best);
	best = NULL;
	separator = memmem(base + opener_len, len - opener_len, " b/",
			   sizeof(" b/") - 1);
	if (!separator || memmem(separator + 1, len - (separator + 1 - base),
				 " b/", sizeof(" b/") - 1))
		return NULL;

	return git_opener_pair(line, separator - base);
}

/*
 * Steps over a validated hunk's body using both declared line counts. Markers
 * spend neither count, and a deletion-only hunk still consumes its old rows.
 * The operand can end before the counts when its last context rows were cut.
 */
static void skip_hunk_body(struct iomem_cursor *cur, unsigned long old_left,
			   unsigned long new_left)
{
	struct iomem_line line;

	while (old_left || new_left) {
		enum body_class class;

		if (!iomem_cursor_next(cur, &line))
			return;

		class = classify_body_line(line.base, line.len, NULL);
		old_left -= class == BODY_CONTEXT || class == BODY_REMOVED;
		new_left -= class == BODY_CONTEXT || class == BODY_ADDED;
	}
}

/*
 * Count out every hunk before looking for file headers, so body rows that begin
 * with "--- " or "+++ " keep their role. Index both operands identically,
 * retaining hunkless records too.
 *
 * A record starts at its first operation metadata line, or at --- when there is
 * none. A named Git block without file headers can still yield a metadata
 * record when the next block begins or the input ends.
 */
void index_patch(const struct iomem_buf *buf, struct cds_list_head *list)
{
	struct patch_name *git_name __free(patch_name) = NULL;
	struct block_latch latch = {};
	bool header_seen = false;
	struct iomem_cursor cur;
	struct iomem_line line;
	long arm_pos = -1;

	iomem_cursor_init(&cur, buf);
	while (iomem_cursor_next(&cur, &line)) {
		struct patch_name *name0 __free(patch_name) = NULL;
		struct patch_name *name1 __free(patch_name) = NULL;
		unsigned long old_count, new_count;
		long pos = line.offset;

		if (header_seen &&
		    !read_atatline_n(line.base, line.len, NULL, &old_count,
				     NULL, &new_count)) {
			skip_hunk_body(&cur, old_count, new_count);
			continue;
		}

		if (line_starts(&line, "diff --git ")) {
			/*
			 * Emit only complete metadata records. Reset the name,
			 * state, and offset together so none can leak into the
			 * next block.
			 */
			if (git_name && arm_pos >= 0 && latch.complete) {
				struct file_list *rec;

				rec = add_to_list(list, git_name, arm_pos);
				rec->mode_only = true;
			}

			patch_name_free(git_name);
			git_name = git_block_name(buf, &line);
			block_latch_opener(&latch, git_name != NULL);
			arm_pos = -1;
			continue;
		}

		if (!line_starts(&line, "--- ")) {
			/*
			 * Save the first qualifying metadata offset within this
			 * named block. File headers close that opportunity; the
			 * following lines then belong to the text section.
			 */
			if (block_latch_line(&latch, line.base, line.len))
				arm_pos = pos;

			continue;
		}

		/* This section names a file, so it isn't mode-only */
		patch_name_free(git_name);
		git_name = NULL;
		block_latch_opener(&latch, false);

		if (arm_pos >= 0) {
			pos = arm_pos;
			arm_pos = -1;
		}

		name0 = filename_from_header(line.base + 4, line.len - 4);

		/*
		 * A lone header must not consume the next section's first line.
		 */
		if (!iomem_cursor_next(&cur, &line))
			break;

		if (!line_starts(&line, "+++ ")) {
			iomem_cursor_seek(&cur, line.offset, "patch");
			continue;
		}

		name1 = filename_from_header(line.base + 4, line.len - 4);
		name0->prefix = patch_name_prefix(buf, pos, "patch", name0);
		name1->prefix = patch_name_prefix(buf, pos, "patch", name1);
		header_seen = true;

		add_to_list(list, best_patch_name(name0, name1), pos);
	}

	/* A mode-only block at the very end of the operand */
	if (git_name && arm_pos >= 0 && latch.complete)
		add_to_list(list, git_name, arm_pos)->mode_only = true;
}

struct file_list *add_to_list(struct cds_list_head *list,
			      const struct patch_name *file, long pos)
{
	struct file_list *make = xmalloc(sizeof(*make));

	*make = (typeof(*make)){ .file = patch_name_dup(file), .pos = pos };
	cds_list_add_tail(&make->node, list);
	return make;
}

struct patch_name_resolution {
	struct patch_name *name;
	enum patch_prefix prefix;
};

enum file_pair_stage {
	FILE_PAIR_FULL_NAME,
	FILE_PAIR_STRIPPED_NAME,
	FILE_PAIR_SUFFIX
};

struct file_group {
	const struct patch_name *name;
	struct file_list **records;
	struct file_group *pair;
	struct file_group *best;
	size_t count;
	int score;
	bool exact;
	bool tied;
};

struct file_groups {
	struct file_group *groups;
	size_t count;
};

/*
 * Keep mode recognition and field parsing on the same vocabulary. Creation and
 * deletion have distinct flags even though each supplies only one mode.
 */
/* clang-format off */
static const struct {
	const char *word;
	size_t len;
	bool old_side;
	bool created;
	bool deleted;
} mode_words[] = {
	{
		.word = "old mode",
		.len = sizeof("old mode") - 1,
		.old_side = true
	},
	{
		.word = "new mode",
		.len = sizeof("new mode") - 1
	},
	{
		.word = "new file mode",
		.len = sizeof("new file mode") - 1,
		.created = true
	},
	{
		.word = "deleted file mode",
		.len = sizeof("deleted file mode") - 1,
		.old_side = true,
		.deleted = true
	}
};
/* clang-format on */

bool line_is_mode_word(const char *base, size_t len)
{
	for (size_t i = 0; i < ARRAY_SIZE(mode_words); i++) {
		size_t at = mode_words[i].len;

		if (len > at && !memcmp(base, mode_words[i].word, at) &&
		    (base[at] == ' ' || base[at] == '\t'))
			return true;
	}

	return false;
}

bool scan_mode_line(const char *base, size_t len, struct file_mode *m)
{
	for (size_t i = 0; i < ARRAY_SIZE(mode_words); i++) {
		size_t at = mode_words[i].len, n = 0;
		char token[sizeof(m->old_mode)];

		if (len < at || memcmp(base, mode_words[i].word, at))
			continue;

		/* Separate the keyword from its octal mode */
		for (; at < len && (base[at] == ' ' || base[at] == '\t'); at++)
			;

		if (at == mode_words[i].len)
			return false;

		/* Keep the written digits without normalizing their value */
		for (; at < len && base[at] >= '0' && base[at] <= '7'; at++) {
			/*
			 * Truncation would report a different mode, so reject
			 * tokens that do not fit with their NUL terminator.
			 */
			if (n == sizeof(token) - 1)
				return false;

			token[n++] = base[at];
		}

		/* An octal prefix of an invalid token is not a complete mode */
		if (!n || (at < len && base[at] != ' ' && base[at] != '\t' &&
			   base[at] != '\r'))
			return false;

		/* Record a valid field without clearing earlier flags */
		token[n] = '\0';
		memcpy(mode_words[i].old_side ? m->old_mode : m->new_mode,
		       token, n + 1);
		m->created |= mode_words[i].created;
		m->deleted |= mode_words[i].deleted;
		return true;
	}

	return false;
}

/*
 * Rename and copy need adjacent from/to lines. A valid similarity percentage
 * can establish complete metadata on its own.
 */
#define WORD_LEN(word) word, sizeof(word) - 1

/* clang-format off */
static const struct {
	const char *word;
	size_t len;
	enum block_op op;
	bool similarity;
} operation_words[] = {
	{ WORD_LEN("rename from "),         BLOCK_OP_RENAME_FROM, false },
	{ WORD_LEN("rename to "),           BLOCK_OP_RENAME_TO,   false },
	{ WORD_LEN("copy from "),           BLOCK_OP_COPY_FROM,   false },
	{ WORD_LEN("copy to "),             BLOCK_OP_COPY_TO,     false },
	{ WORD_LEN("similarity index "),    BLOCK_OP_NONE,        true },
	{ WORD_LEN("dissimilarity index "), BLOCK_OP_NONE,        true }
};
/* clang-format on */

#undef WORD_LEN

/* Git emits either a binary payload or a summary that the files differ */
static const char *const binary_words[] = { "GIT binary patch",
					    "Binary files " };

bool line_is_binary_word(const char *base, size_t len)
{
	for (size_t i = 0; i < ARRAY_SIZE(binary_words); i++) {
		size_t at = strlen(binary_words[i]);

		if (len >= at && !memcmp(base, binary_words[i], at))
			return true;
	}

	return false;
}

/* The offset where the lowercase-hex run starting at `at` ends */
static size_t hex_run(const char *base, size_t len, size_t at)
{
	for (; at < len && ((base[at] >= '0' && base[at] <= '9') ||
			    (base[at] >= 'a' && base[at] <= 'f'));
	     at++)
		;

	return at;
}

/*
 * Index lines can establish that metadata is complete, so require the full
 * grammar: two 4-64 digit lowercase object IDs, an optional six-digit octal
 * mode, and an optional CR. Combined-diff object lists are unsupported.
 */
bool scan_index_line(const char *base, size_t len, struct iomem_slice *pre,
		     struct iomem_slice *post)
{
	size_t at = sizeof("index ") - 1, run;
	struct iomem_slice a, b;

	if (len < at || memcmp(base, "index ", at))
		return false;

	run = hex_run(base, len, at);
	if (run - at < 4 || run - at > 64)
		return false;

	a.base = base + at;
	a.len = run - at;

	at = run;
	if (len - at < 2 || base[at] != '.' || base[at + 1] != '.')
		return false;

	run = hex_run(base, len, at + 2);
	if (run - (at + 2) < 4 || run - (at + 2) > 64)
		return false;

	b.base = base + at + 2;
	b.len = run - (at + 2);

	at = run;
	if (at < len && base[at] == ' ') {
		size_t digits;

		at++;
		for (digits = 0; at < len && base[at] >= '0' && base[at] <= '7';
		     digits++)
			at++;

		if (digits != 6)
			return false;
	}

	if (at < len && base[at] == '\r')
		at++;

	if (at != len)
		return false;

	*pre = a;
	*post = b;
	return true;
}

/*
 * A complete similarity field has one or two digits, or exactly 100, followed
 * by '%' and an optional CR. The caller skips the keyword and its space.
 */
static bool similarity_is_strict(const char *base, size_t len, size_t at)
{
	size_t digits = at;

	for (; digits < len && base[digits] >= '0' && base[digits] <= '9';
	     digits++)
		;

	if (digits == at || digits - at > 3)
		return false;

	if (digits - at == 3 && memcmp(base + at, "100", 3))
		return false;

	at = digits;
	if (at >= len || base[at] != '%')
		return false;

	at++;
	if (at < len && base[at] == '\r')
		at++;

	return at == len;
}

enum block_line_kind {
	BLOCK_LINE_END,
	BLOCK_LINE_MODE,
	BLOCK_LINE_OPERATION,
	BLOCK_LINE_SIMILARITY,
	BLOCK_LINE_INDEX,
	BLOCK_LINE_BINARY
};

struct block_line {
	struct iomem_slice index[2];
	struct iomem_slice tail;
	enum block_line_kind kind;
	enum block_op operation;
	bool strict_similarity;
};

/*
 * Both metadata consumers end their run on the same grammar. Recognition is
 * weaker than capture: invalid mode fields and empty operation halves still
 * belong to the run, but neither supplies a usable value.
 */
static struct block_line classify_block_line(const char *base, size_t len)
{
	struct block_line line = {};

	if (line_is_mode_word(base, len)) {
		line.kind = BLOCK_LINE_MODE;
		return line;
	}

	for (size_t i = 0; i < ARRAY_SIZE(operation_words); i++) {
		size_t at = operation_words[i].len;

		if (len < at || memcmp(base, operation_words[i].word, at))
			continue;

		if (operation_words[i].similarity) {
			line.kind = BLOCK_LINE_SIMILARITY;
			line.strict_similarity =
				similarity_is_strict(base, len, at);
		} else {
			size_t end = len - (len && base[len - 1] == '\r');

			line.kind = BLOCK_LINE_OPERATION;
			line.operation = operation_words[i].op;
			if (end > at)
				line.tail = (typeof(line.tail)){ base + at,
								 end - at };
		}
		return line;
	}

	if (scan_index_line(base, len, &line.index[0], &line.index[1]))
		line.kind = BLOCK_LINE_INDEX;
	else if (line_is_binary_word(base, len))
		line.kind = BLOCK_LINE_BINARY;

	return line;
}

void block_latch_opener(struct block_latch *bl, bool named)
{
	*bl = (typeof(*bl)){ .named = named, .run_open = named };
}

/*
 * Track metadata before a file's ---/+++ headers. A mode or binary line marks
 * an operation anywhere in the named block, but completeness requires evidence
 * in the uninterrupted metadata immediately after its opener. A rename or copy
 * needs adjacent matching from/to lines so a lone prose fragment cannot count.
 */
bool block_latch_line(struct block_latch *bl, const char *base, size_t len)
{
	struct block_line line;
	bool first;

	if (!bl->named)
		return false;

	line = classify_block_line(base, len);
	switch (line.kind) {
	case BLOCK_LINE_MODE:
		if (bl->run_open) {
			if (!memcmp(base, "old mode", sizeof("old mode") - 1))
				bl->saw_old_mode = true;
			else if (!memcmp(base, "new mode",
					 sizeof("new mode") - 1))
				bl->saw_new_mode = true;

			if (bl->saw_old_mode && bl->saw_new_mode)
				bl->complete = true;
		}

		bl->prev_op = BLOCK_OP_NONE;
		goto arm;

	case BLOCK_LINE_OPERATION:
		if (!bl->run_open)
			return false;

		if (!line.tail.len) {
			bl->prev_op = BLOCK_OP_NONE;
			return false;
		}

		/* A same-kind pair arms the block; a lone half only tracks */
		if ((bl->prev_op == BLOCK_OP_RENAME_FROM &&
		     line.operation == BLOCK_OP_RENAME_TO) ||
		    (bl->prev_op == BLOCK_OP_COPY_FROM &&
		     line.operation == BLOCK_OP_COPY_TO)) {
			bl->complete = true;
			bl->prev_op = line.operation;
			goto arm;
		}

		bl->prev_op = line.operation;
		return false;

	case BLOCK_LINE_SIMILARITY:
		if (bl->run_open) {
			bl->complete |= line.strict_similarity;
			bl->prev_op = BLOCK_OP_NONE;
		}
		return false;

	case BLOCK_LINE_INDEX:
		if (bl->run_open)
			bl->complete = true;

		bl->prev_op = BLOCK_OP_NONE;
		return false;

	case BLOCK_LINE_BINARY:
		/* A binary payload still needs its index line for completion */
		bl->prev_op = BLOCK_OP_NONE;
		goto arm;

	case BLOCK_LINE_END:
		bl->run_open = false;
		bl->prev_op = BLOCK_OP_NONE;
		return false;
	}

arm:
	first = !bl->armed;
	bl->armed = true;
	return first;
}

/*
 * Capture authored fields as slices borrowed from the patch buffer. The
 * classifier fixes the run boundary independently of which fields parse, so a
 * recognized line with an unusable field still lets the story continue.
 */
static bool block_story_line(struct block_story *bs, const char *base,
			     size_t len)
{
	struct block_line line = classify_block_line(base, len);

	switch (line.kind) {
	case BLOCK_LINE_MODE:
		scan_mode_line(base, len, &bs->mode);
		break;

	case BLOCK_LINE_OPERATION:
		if (!line.tail.len)
			break;

		switch (line.operation) {
		case BLOCK_OP_RENAME_FROM:
			bs->rename_from = line.tail;
			break;
		case BLOCK_OP_RENAME_TO:
			bs->rename_to = line.tail;
			break;
		case BLOCK_OP_COPY_FROM:
			bs->copy_from = line.tail;
			break;
		case BLOCK_OP_COPY_TO:
			bs->copy_to = line.tail;
			break;
		case BLOCK_OP_NONE:
			break;
		}

		break;

	case BLOCK_LINE_INDEX:
		bs->index_pre = line.index[0];
		bs->index_post = line.index[1];
		bs->has_index = true;
		break;

	case BLOCK_LINE_BINARY:
		bs->has_binary = true;
		break;

	case BLOCK_LINE_SIMILARITY:
		break;

	case BLOCK_LINE_END:
		return false;
	}

	return true;
}

void block_story_read(struct block_story *bs, const struct iomem_buf *buf,
		      size_t opener, const char *operand)
{
	struct iomem_cursor cur;
	struct iomem_line line;

	*bs = (typeof(*bs)){};
	iomem_cursor_init(&cur, buf);
	iomem_cursor_seek(&cur, opener, operand);

	/* Metadata must immediately follow the opener */
	if (!iomem_cursor_next(&cur, &line))
		return;

	while (iomem_cursor_next(&cur, &line)) {
		if (!block_story_line(bs, line.base, line.len))
			return;
	}
}
