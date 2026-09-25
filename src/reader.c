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

void check_hunk_chop(unsigned long old_left, unsigned long new_left)
{
	if (new_left > 3)
		die("unexpected end of file in patch");

	if (old_left != new_left)
		die("malformed patch: hunk short by %lu old and %lu new lines",
		    old_left, new_left);
}

struct hunk_progress {
	unsigned long old_left;
	unsigned long new_left;
	unsigned long header_line;
	int markers;
	bool active;
	bool changed;
};

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
