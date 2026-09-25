// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ctrl IQ, Inc.
 *
 * Allocation wrappers, the fatal and debug diagnostics, the program-identity
 * global, and the standard-output write-error check shared by the program.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <error.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <util.h>

const char *progname = "(unset)";
bool debug = false;

/*
 * Limit memory growth relative to the process's size at startup.
 * AddressSanitizer reserves terabytes of address space before main(), so an
 * absolute limit would prevent it from running. A relative limit gives normal
 * and sanitizer builds the same allowance for additional allocations.
 */
#define MEM_CEILING ((size_t)768 << 20) /* bytes past the entry footprint */

/*
 * Read the current page count from a 1-based field in /proc/self/statm. Use
 * open/read because allocating a stdio buffer would increase the measurement.
 *
 * Leave the resource limit unchanged when this procfs file isn't available.
 * Other access or parsing failures mustn't silently disable the memory bound.
 */
static void mem_limit_install(int resource, int field)
{
	char buf[128], *at, *end;
	unsigned long pages;
	struct rlimit rl;
	rlim_t want;
	ssize_t got;
	int fd;

	fd = open("/proc/self/statm", O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		if (errno == ENOENT || errno == ENOTDIR)
			return;

		edie(errno, "/proc/self/statm");
	}
	got = read(fd, buf, sizeof(buf) - 1);
	if (got <= 0)
		edie(errno, "/proc/self/statm");

	close(fd);
	buf[got] = '\0';

	at = buf;
	for (int i = 1; i < field; i++) {
		at = strchr(at, ' ');
		if (!at)
			die("/proc/self/statm: too few fields");

		at++;
	}

	if (*at < '0' || *at > '9')
		die("/proc/self/statm: no page count");

	errno = 0;
	pages = strtoul(at, &end, 10);
	if (errno)
		edie(errno, "/proc/self/statm");

	if (*end != ' ')
		die("/proc/self/statm: bad page count");

	/* Keep overflow checking available without C23's stdckdint.h */
	if (__builtin_mul_overflow(pages, sysconf(_SC_PAGESIZE), &want) ||
	    __builtin_add_overflow(want, MEM_CEILING, &want))
		die("/proc/self/statm: page count overflow");

	/*
	 * Preserve any tighter existing limit, and never request a soft limit
	 * above the hard limit. Leave the hard limit itself unchanged.
	 */
	if (getrlimit(resource, &rl))
		edie(errno, "getrlimit");

	if (rl.rlim_max != RLIM_INFINITY && want > rl.rlim_max)
		want = rl.rlim_max;
	if (rl.rlim_cur == RLIM_INFINITY || want < rl.rlim_cur) {
		rl.rlim_cur = want;
		if (setrlimit(resource, &rl))
			edie(errno, "setrlimit");
	}
}

void mem_limit_init(void)
{
	/*
	 * statm field 1 measures total mapped memory in pages. RLIMIT_AS covers
	 * large allocations made through mmap even on kernels whose RLIMIT_DATA
	 * only limits the traditional heap.
	 */
	mem_limit_install(RLIMIT_AS, 1);
}

void mem_limit_init_tree(void)
{
	/*
	 * Tree mode maps pack and index files through libgit2. Their read-only
	 * mappings can be large without consuming the comparison's memory
	 * allowance. RLIMIT_DATA limits heap growth and, since Linux 4.7,
	 * writable private mappings. statm field 6 supplies the corresponding
	 * initial size in pages: data plus stack.
	 */
	mem_limit_install(RLIMIT_DATA, 6);
}

/*
 * error(3)'s default prefix is whatever argv[0] the caller used to invoke this
 * process; set_progname() below points error_print_progname at this instead, so
 * diagnostics name the tool consistently regardless of the invocation path.
 */
static void print_progname(void)
{
	fprintf(stderr, "%s: ", progname);
}

void set_progname(const char *name)
{
	progname = name;
	error_print_progname = print_progname;
}

/* Write diagnostics directly, without a fixed-size formatting buffer */
static void print_fatal(int errnum, const char *fmt, va_list args)
{
	fflush(stdout);
	print_progname();
	vfprintf(stderr, fmt, args);
	if (errnum)
		fprintf(stderr, ": %s", strerror(errnum));
	fputc('\n', stderr);
}

_Noreturn void die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	print_fatal(0, fmt, ap);
	va_end(ap);
	exit(EXIT_FAILURE);
}

_Noreturn void edie(int errnum, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	print_fatal(errnum, fmt, ap);
	va_end(ap);
	exit(EXIT_FAILURE);
}

void pr_dbg(const char *fmt, ...)
{
	va_list ap;

	if (!debug)
		return;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

void *xmalloc(size_t size)
{
	void *ptr;

	ptr = malloc(size);
	if (!ptr)
		error(EXIT_FAILURE, errno, "malloc");

	return ptr;
}

void *xzalloc(size_t size)
{
	void *ptr;

	ptr = calloc(1, size);
	if (!ptr)
		error(EXIT_FAILURE, errno, "calloc");

	return ptr;
}

void *xrealloc(void *ptr, size_t size)
{
	void *new_ptr;

	new_ptr = realloc(ptr, size);
	if (!new_ptr)
		error(EXIT_FAILURE, errno, "realloc");

	return new_ptr;
}

static size_t array_bytes(size_t nmemb, size_t size)
{
	size_t nbytes;

	if (__builtin_mul_overflow(nmemb, size, &nbytes))
		die("size overflow: %zu members of %zu bytes", nmemb, size);

	return nbytes;
}

void *xmalloc_array(size_t nmemb, size_t size)
{
	return xmalloc(array_bytes(nmemb, size));
}

void *xzalloc_array(size_t nmemb, size_t size)
{
	return xzalloc(array_bytes(nmemb, size));
}

void *xrealloc_array(void *ptr, size_t nmemb, size_t size)
{
	return xrealloc(ptr, array_bytes(nmemb, size));
}

char *xstrdup(const char *s)
{
	size_t len = strlen(s) + 1;
	char *dup = xmalloc(len);

	memcpy(dup, s, len);
	return dup;
}

char *memdup(const char *s, size_t n)
{
	char *dup = xmalloc(n + 1);

	memcpy(dup, s, n);
	dup[n] = '\0';
	return dup;
}

int xasprintf(char **strp, const char *fmt, ...)
{
	va_list args;
	int len;

	va_start(args, fmt);
	len = vasprintf(strp, fmt, args);
	va_end(args);

	if (len < 0)
		error(EXIT_FAILURE, errno, "vasprintf");

	return len;
}

FILE *xopen_memstream(char **bufp, size_t *sizep)
{
	FILE *fp;

	fp = open_memstream(bufp, sizep);
	if (!fp)
		error(EXIT_FAILURE, errno, "open_memstream");

	return fp;
}

/*
 * A full nonblocking pipe is an error; only a departed reader ends a pipeline.
 */
static bool pipe_reader_closed(int fd)
{
	struct pollfd pipe = { .fd = fd, .events = POLLOUT };
	struct stat st;

	if (fstat(fd, &st) || !S_ISFIFO(st.st_mode))
		return false;

	return poll(&pipe, 1, 0) == 1 && (pipe.revents & POLLERR);
}

/*
 * glibc remembers a failed write in the stream's error flag but doesn't retry
 * the discarded output on fflush(). Check both results, and trust errno only
 * when the flush itself failed; otherwise it may describe an unrelated error.
 *
 * A pipe reader that stopped early is a routine end to a pipeline. Query the
 * endpoint's state to distinguish that from a full nonblocking pipe without
 * depending on an errno that another operation may have overwritten.
 */
void check_output(FILE *out, const char *name)
{
	bool flush_failed = fflush(out) != 0;
	int saved_errno = flush_failed ? errno : 0;

	if (!flush_failed && !ferror(out))
		return;

	if (pipe_reader_closed(fileno(out)))
		return;

	edie(saved_errno, "write error on %s", name);
}
