// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ctrl IQ, Inc.
 *
 * A preload for attributes of the tool's own process image that no wrapper
 * process can set on its behalf. The dumpable flag is cleared so a test that
 * kills the tool by design doesn't feed the machine's core handler on every
 * suite run; RLIMIT_CORE=0 can't do that job under a piped core_pattern, and
 * the flag doesn't survive execve from a wrapper. A signal number named in
 * TOOL_SETUP_SIG_DFL is restored to its default disposition so a sig: spec
 * holds under any invoking environment; an ignored disposition survives the
 * whole exec chain, and POSIX forbids a non-interactive shell from restoring
 * it. Every other test keeps real crashes dumpable, and a run with the variable
 * absent or bound empty keeps its inherited dispositions untouched.
 */

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/prctl.h>

static int parse_sig_dfl(const char *s)
{
	const char *p;
	char *end;
	long sig;

	/*
	 * The runner is the only writer and always hands over a bare decimal
	 * from kill -l, so anything else is inherited garbage and parses to 0
	 * (no restore). The bound runs on the long: narrowed to int first, an
	 * out-of-range decimal can wrap back into range (4294967321 lands on
	 * signal 25).
	 */
	if (!*s)
		return 0;

	for (p = s; *p; p++) {
		if (*p < '0' || *p > '9')
			return 0;
	}
	errno = 0;
	sig = strtol(s, &end, 10);
	if (errno != 0 || *end != '\0')
		return 0;

	if (sig < 1 || sig >= NSIG)
		return 0;

	return (int)sig;
}

__attribute__((constructor)) static void tool_setup(void)
{
	const char *sig_dfl = getenv("TOOL_SETUP_SIG_DFL");

	prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
	if (sig_dfl) {
		int sig;

		sig = parse_sig_dfl(sig_dfl);
		if (sig)
			signal(sig, SIG_DFL);
	}
}
