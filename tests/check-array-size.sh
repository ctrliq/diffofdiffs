#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2026 Ctrl IQ, Inc.
set -eu

# A compile failure must come from the contract, even with warnings disabled
arena=$(mktemp -d)
trap 'rm -rf "$arena"' EXIT
compiler=("$@")
for expression in 'ARRAY_SIZE(pointer)' 'sizeof(ARRAY_SIZE(pointer))'; do
	cat > "$arena/pointer.c" <<EOF
#include <util.h>
size_t count(int *pointer)
{
	return $expression;
}
EOF
	if "${compiler[@]}" -w -fsyntax-only "$arena/pointer.c" \
		> "$arena/diagnostic" 2>&1; then
		echo "ARRAY_SIZE accepted a pointer: $expression" >&2
		exit 1
	fi
	if ! grep -q 'ARRAY_SIZE requires an array' "$arena/diagnostic"; then
		cat "$arena/diagnostic" >&2
		exit 1
	fi
done

echo 'array-size checks: passed'
