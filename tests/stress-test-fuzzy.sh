#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# tests/stress-test-fuzzy.sh - compare every commit in a range against
# itself through diffofdiffs and report any non-empty output as a bug.
#
# Usage: tests/stress-test-fuzzy.sh <git-tree> <commit-range>
#
# Output: only prints SHAs that produce non-empty diffofdiffs output,
# along with the first line of that output for triage.
#
# Standalone oracle, not a registered test: run it by hand against a git
# tree of interest. It is invisible to `make check`, which only discovers
# tests/*/spec files.

set -uo pipefail

if [ $# -ne 2 ]; then
	echo "Usage: $0 <git-tree> <commit-range>" >&2
	exit 1
fi

GIT_TREE="$1"
COMMIT_RANGE="$2"
DIFFOFDIFFS="${DIFFOFDIFFS:-diffofdiffs}"
JOBS="${JOBS:-$(nproc)}"
test_commit () {
	local sha="$1"
	local patch out

	if ! patch=$(git -C "$GIT_TREE" format-patch -1 --stdout "$sha" 2>&1); then
		echo "FAIL $sha format-patch: $patch"
		return
	fi

	local rc=0
	out=$("$DIFFOFDIFFS" <(echo "$patch") <(echo "$patch") 2>&1) || rc=$?

	if [ "$rc" -ne 0 ]; then
		echo "FAIL $sha exit $rc: $(echo "$out" | head -1)"
	elif [ -n "$out" ]; then
		echo "FAIL $sha $(echo "$out" | head -1)"
	fi
}

export -f test_commit
export GIT_TREE DIFFOFDIFFS

git -C "$GIT_TREE" rev-list "$COMMIT_RANGE" | xargs -P "$JOBS" -I{} bash -c 'test_commit "$@"' _ {}
