/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 Ctrl IQ, Inc.
 *
 * Allocation wrappers, the fatal and debug diagnostics, the program-identity
 * global, and the standard-output write-error check shared by the program.
 */
#ifndef UTIL_H
#define UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

#define __unused __attribute__((__unused__))

/*
 * Marks a printf-style function so the compiler checks its format string
 * against the arguments that follow it. fmt is the 1-based parameter index of
 * the format string; first is the 1-based index of the first variadic argument.
 */
#define __printf(fmt, first) __attribute__((__format__(__printf__, fmt, first)))

/*
 * Element count of a declared array. When the operand is a pointer rather than
 * an array, __must_be_array()'s bitfield width goes negative and the build
 * dies, so a decayed pointer can never slip through the division silently.
 */
#define __same_type(a, b) __builtin_types_compatible_p(typeof(a), typeof(b))
#define __must_be_array(a) \
	((int)(sizeof(struct { int : (-!!(__same_type((a), &(a)[0]))); })))
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]) + __must_be_array(arr))

/*
 * Marks a function whose return value must be used, so dropping the result
 * becomes a compile-time diagnostic instead of a silent bug.
 */
#define __must_check __attribute__((__warn_unused_result__))

/*
 * __free(name) calls __free_name() when a local leaves scope. DEFINE_FREE()
 * supplies the local's value as _T to the cleanup expression. no_free_ptr()
 * retrieves the value and clears the local, transferring ownership without
 * freeing it at scope exit. Its result must be used to avoid losing the only
 * reference to the allocation.
 */
#define __cleanup(func) __attribute__((__cleanup__(func)))

#define DEFINE_FREE(_name, _type, _free) \
	static __always_inline void __free_##_name(void *p) \
	{ \
		_type _T = *(_type *)p; \
		_free; \
	}

#define __free(_name) __cleanup(__free_##_name)

#define __get_and_null(p, nullvalue) \
	({ \
		__auto_type __ptr = &(p); \
		__auto_type __val = *__ptr; \
		*__ptr = nullvalue; \
		__val; \
	})

static __always_inline __must_check const volatile void *
__must_check_fn(const volatile void *val)
{
	return val;
}

#define no_free_ptr(p) \
	((typeof(p))__must_check_fn( \
		(const volatile void *)__get_and_null(p, NULL)))

DEFINE_FREE(free, void *, free(_T))

/* Application diagnostic prefix supplied by set_progname() */
extern const char *progname;

/*
 * Installs the process-wide memory bound: the limit becomes the entry footprint
 * plus a fixed ceiling, and only ever moves down. Called at the top of each
 * tool main(), before anything allocates, so the footprint read reflects entry
 * state alone. The patch-only form binds RLIMIT_AS; the tree form binds
 * RLIMIT_DATA instead, since a tree run's read-only pack and index windows
 * scale with the repository rather than with the run's own work.
 */
void mem_limit_init(void);
void mem_limit_init_tree(void);

/*
 * Records name as progname and points error(3) at it, so every diagnostic
 * error() emits afterward names the tool instead of whatever argv[0] the caller
 * happened to use. The name is kept, not copied, so it must have static
 * lifetime; keeping set_progname() allocation-free lets callers install the
 * prefix ahead of mem_limit_init()'s footprint read.
 */
void set_progname(const char *name);

/*
 * Print a fatal diagnostic prefixed by progname and exit with EXIT_FAILURE.
 * edie() appends the error description when errnum is nonzero.
 */
_Noreturn void die(const char *fmt, ...) __printf(1, 2);
_Noreturn void edie(int errnum, const char *fmt, ...) __printf(2, 3);

/*
 * Set by --debug; gates diagnostics whose format carries no stability promise.
 */
extern bool debug;

/*
 * Prints a debug diagnostic to stderr when debug is set and does nothing
 * otherwise. Unlike the fatal wrappers above, the output takes no progname
 * prefix.
 */
void pr_dbg(const char *fmt, ...) __printf(1, 2);

/* Allocation failure below is fatal: none of these ever return NULL */
void *xmalloc(size_t size);
void *xzalloc(size_t size);
void *xrealloc(void *ptr, size_t size);
char *xstrdup(const char *s);

/* Copies n readable bytes, including NULs, and adds a trailing NUL */
char *memdup(const char *s, size_t n);

/*
 * Check the element-count multiplication before allocating, so an overflowing
 * product cannot produce a buffer smaller than the requested array.
 */
void *xmalloc_array(size_t nmemb, size_t size);
void *xzalloc_array(size_t nmemb, size_t size);
void *xrealloc_array(void *ptr, size_t nmemb, size_t size);

/*
 * Formats into a freshly allocated buffer, stores it in *strp, and returns the
 * formatted length, same as vasprintf(3). Allocation failure is fatal, same as
 * the other x* wrappers above.
 */
int xasprintf(char **strp, const char *fmt, ...) __printf(2, 3);

/* Open a memory output stream, reporting allocation failure normally */
FILE *xopen_memstream(char **bufp, size_t *sizep);

/*
 * Call before a successful exit to check for lost output. A pipe whose reader
 * closed early is a normal end to a pipeline; other write failures exit
 * nonzero, including a full nonblocking pipe with a reader still attached.
 */
void check_output(FILE *out, const char *name);

#endif /* UTIL_H */
