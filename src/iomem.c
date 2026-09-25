// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ctrl IQ, Inc.
 *
 * Read files or stdin into memory, expose independent line cursors, and collect
 * generated patches in an append-only writer.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <iomem.h>
#include <util.h>

/*
 * Streams have no size hint from fstat. Start them at 128 KiB and grow
 * geometrically; regular files can reserve their reported size immediately.
 */
#define INTAKE_BLOCK ((size_t)128 << 10)

/*
 * Reserve the sentinel separately from payload capacity. main()'s allocation
 * limit bounds growth before the capacity can overflow.
 */
static void intake_grow(struct iomem_buf *buf)
{
	size_t cap = buf->cap ? buf->cap * 2 : INTAKE_BLOCK;

	buf->base = xrealloc(buf->base, cap + 1);
	buf->cap = cap;
}

/*
 * Retry interruptions, but fail on EAGAIN: retrying a nonblocking descriptor
 * without waiting for readiness would spin.
 */
static size_t read_retry_eintr(int fd, char *dest, size_t want,
			       const char *name)
{
	for (;;) {
		ssize_t nread;

		nread = read(fd, dest, want);
		if (nread >= 0)
			return nread;

		if (errno != EINTR)
			edie(errno, "%s", name);
	}
}

static bool is_descriptor_path(const char *name)
{
	static const char *const directories[] = { "/dev/fd", "/proc/self/fd" };
	char *parent __free(free) = xstrdup(name);
	struct stat directory, descriptors;
	char *resolved __free(free) = NULL;
	char *slash = strrchr(parent, '/');

	if (!strcmp(name, "/dev/stdin") || !strcmp(name, "/dev/stdout") ||
	    !strcmp(name, "/dev/stderr"))
		return true;

	/* Resolve directory aliases without following the fd's target */
	if (slash)
		slash[1] = '\0';
	else
		strcpy(parent, ".");
	if (stat(parent, &directory))
		return false;

	for (size_t i = 0; i < ARRAY_SIZE(directories); i++) {
		if (!stat(directories[i], &descriptors) &&
		    directory.st_dev == descriptors.st_dev &&
		    directory.st_ino == descriptors.st_ino)
			return true;
	}

	/* Thread and process fd directories have their own directory inodes */
	resolved = realpath(parent, NULL);
	return resolved &&
	       (!fnmatch("/proc/[0-9]*/fd", resolved, FNM_PATHNAME) ||
		!fnmatch("/proc/[0-9]*/task/[0-9]*/fd", resolved,
			 FNM_PATHNAME));
}

bool iomem_acquire(struct iomem_buf *buf, const char *name)
{
	bool is_stdin = !strcmp(name, "-");
	bool named_file;
	struct stat st;
	int fd;

	*buf = (typeof(*buf)){};

	if (is_stdin) {
		fd = STDIN_FILENO;
	} else {
		fd = open(name, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			edie(errno, "%s", name);
	}

	if (fstat(fd, &st))
		edie(errno, "%s", name);

	if (S_ISDIR(st.st_mode))
		edie(EISDIR, "%s", name);

	/* Descriptor numbers are not filenames, even for regular files */
	named_file = S_ISREG(st.st_mode) && !is_stdin &&
		     !is_descriptor_path(name);

	/*
	 * A regular file normally needs one allocation. Treat its size as a
	 * hint, since the file can grow or shrink while it is being read.
	 */
	if (S_ISREG(st.st_mode) && st.st_size > 0) {
		buf->cap = st.st_size;
		buf->base = xmalloc(buf->cap + 1);
	}

	for (;;) {
		size_t nread;
		char probe;

		if (buf->len < buf->cap) {
			nread = read_retry_eintr(fd, buf->base + buf->len,
						 buf->cap - buf->len, name);
			if (!nread)
				break;

			buf->len += nread;
			continue;
		}

		/*
		 * Probe for another byte before growing a full buffer. A
		 * zero-length read would falsely indicate EOF; growing first
		 * could hit the memory limit even when no more bytes exist.
		 */
		if (!read_retry_eintr(fd, &probe, 1, name))
			break;

		intake_grow(buf);
		buf->base[buf->len++] = probe;
	}

	if (!is_stdin)
		close(fd);

	/*
	 * Even an empty operand needs storage for the sentinel. Keep its
	 * payload capacity at zero.
	 */
	if (!buf->base)
		buf->base = xmalloc(1);

	buf->base[buf->len] = '\0';
	return named_file;
}

void iomem_buf_free(struct iomem_buf *buf)
{
	free(buf->base);
	*buf = (typeof(*buf)){};
}

void iomem_cursor_init(struct iomem_cursor *cur, const struct iomem_buf *buf)
{
	cur->buf = buf;
	cur->pos = 0;
}

bool iomem_cursor_next(struct iomem_cursor *cur, struct iomem_line *line)
{
	const struct iomem_buf *buf = cur->buf;
	const char *start, *end;
	size_t left;

	if (cur->pos >= buf->len)
		return false;

	/*
	 * Search by length so embedded NULs remain part of the line. Only LF
	 * ends it.
	 */
	start = buf->base + cur->pos;
	left = buf->len - cur->pos;
	end = memchr(start, '\n', left);
	line->base = start;
	line->offset = cur->pos;
	line->has_lf = end != NULL;
	line->len = end ? (size_t)(end - start) : left;
	cur->pos += line->len + line->has_lf;
	return true;
}

void iomem_cursor_seek(struct iomem_cursor *cur, size_t off, const char *name)
{
	if (off > cur->buf->len)
		die("operand intake bug: seeking %s to offset %zu past its %zu byte length",
		    name, off, cur->buf->len);

	cur->pos = off;
}

void iomem_writer_open(struct iomem_writer *w)
{
	w->base = NULL;
	w->len = 0;
	w->fp = xopen_memstream(&w->base, &w->len);
}

void iomem_writer_write(struct iomem_writer *w, const void *bytes, size_t len)
{
	if (len && fwrite(bytes, 1, len, w->fp) != len)
		edie(errno, "writing an in-memory image");
}

void iomem_writer_printf(struct iomem_writer *w, const char *fmt, ...)
{
	va_list args;
	int ret;

	va_start(args, fmt);
	ret = vfprintf(w->fp, fmt, args);
	va_end(args);

	if (ret < 0)
		edie(errno, "writing an in-memory image");
}

/*
 * The flush is what makes the reported length and its sentinel final, and
 * checking ferror() afterward catches a write that failed earlier and latched:
 * glibc drops the buffer and sets the error flag rather than replaying the
 * write, so errno is only worth reporting when the flush call itself is what
 * failed.
 */
void iomem_writer_publish(struct iomem_writer *w, struct iomem_buf *buf)
{
	bool flush_failed = fflush(w->fp) != 0;
	int saved_errno = flush_failed ? errno : 0;

	if (flush_failed || ferror(w->fp))
		edie(saved_errno, "writing an in-memory image");

	if (fclose(w->fp))
		edie(errno, "closing an in-memory image");

	/*
	 * Capacity is the payload length: the sentinel byte glibc planted at
	 * base[len] sits beyond it, same as an intake buffer's.
	 */
	*buf = (typeof(*buf)){ .base = w->base, .len = w->len, .cap = w->len };
	*w = (typeof(*w)){};
}
