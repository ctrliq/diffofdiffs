/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 Ctrl IQ, Inc.
 *
 * Read files or stdin into memory, expose independent line cursors, and collect
 * generated patches in an append-only writer.
 */
#ifndef IOMEM_H
#define IOMEM_H

#include <stddef.h>
#include <stdio.h>

#include <util.h>

/*
 * DOC: operand buffers and their views
 *
 * Read every operand to EOF before parsing it. Files, pipes, and other input
 * streams share this path; no seeking or format detection is needed here.
 *
 * Embedded NULs are payload. Buffer and line lengths are explicit; the trailing
 * NUL sentinel is for callers that need a terminated string, and never decides
 * where a buffer ends.
 *
 * Cursors borrow the buffer and hold independent positions, so a second pass
 * cannot disturb a first pass's progress.
 *
 * Callers already retain each operand's display name. Pass it when needed for
 * diagnostics instead of storing a second copy in the buffer.
 */

/*
 * One operand's bytes. base owns the allocation and is never NULL after a
 * successful intake, len is the exact payload length with embedded NULs
 * included, cap is the usable payload capacity, and the allocation is always at
 * least len + 1 bytes so base[len] can hold the NUL sentinel.
 */
struct iomem_buf {
	char *base;
	size_t len;
	size_t cap;
};

/*
 * One line of an operand, pointing into the buffer rather than copying it. len
 * excludes the terminating LF, has_lf is false only for an unterminated final
 * line, and offset is the line's zero-based byte position, which is the value
 * to hand back to iomem_cursor_seek().
 */
struct iomem_line {
	const char *base;
	size_t len;
	bool has_lf;
	size_t offset;
};

/*
 * A read position over one buffer. Cursors are cheap and independent: make as
 * many as a read needs, and copy one by value to fork a second pass off the
 * position the first reached. Compaction may rewrite bytes behind a cursor;
 * unread bytes and the recorded length must stay stable until the walk ends.
 */
struct iomem_cursor {
	const struct iomem_buf *buf;
	size_t pos;
};

/*
 * A borrowed span of an operand buffer: a pointer into the bytes plus a length,
 * owning nothing. A slice is valid exactly as long as its buffer is, so no
 * consumer may outlive the operand it was cut from. A NULL base is the empty
 * slice, which is how a parser says a field was never written.
 */
struct iomem_slice {
	const char *base;
	size_t len;
};

/*
 * An append-only memory stream. Callers write through fp without seeking or
 * reading base and len, which are settled by iomem_writer_publish(). Seeking
 * would make the final length depend on the cursor rather than all bytes
 * written.
 */
struct iomem_writer {
	FILE *fp;
	char *base;
	size_t len;
};

/*
 * Read name to EOF into *buf. "-" borrows standard input, leaving it open. The
 * caller must allow at most one stdin operand because a second read would
 * resume from the first read's EOF. Directories and open/read failures are
 * fatal; EAGAIN is not retried. Return whether name identifies a regular file
 * through a filesystem path, rather than stdin or a descriptor alias.
 */
bool iomem_acquire(struct iomem_buf *buf, const char *name);

void iomem_buf_free(struct iomem_buf *buf);

void iomem_cursor_init(struct iomem_cursor *cur, const struct iomem_buf *buf);

/*
 * Fills line with the line at the cursor and steps past it, returning false at
 * EOF without touching line.
 */
bool iomem_cursor_next(struct iomem_cursor *cur, struct iomem_line *line);

/*
 * Moves the cursor to an absolute offset. An offset of exactly the length is
 * legal and lands at EOF; anything past it is a caller bug and dies.
 */
void iomem_cursor_seek(struct iomem_cursor *cur, size_t off, const char *name);

/* Opens a writer. Write through w->fp, then publish. */
void iomem_writer_open(struct iomem_writer *w);

void iomem_writer_write(struct iomem_writer *w, const void *bytes, size_t len);

void iomem_writer_printf(struct iomem_writer *w, const char *fmt, ...)
	__printf(2, 3);

/*
 * Finalizes the writer and fills *buf with its bytes: len is the count the
 * stream reported and base[len] == 0. Ownership transfers to buf, and the
 * writer is spent afterward.
 */
void iomem_writer_publish(struct iomem_writer *w, struct iomem_buf *buf);

#endif /* IOMEM_H */
