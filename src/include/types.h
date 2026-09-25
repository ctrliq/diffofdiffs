/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 Ctrl IQ, Inc.
 *
 * The kernel's short integer names. Each is a pure rename of its <stdint.h>
 * counterpart, so no width, alignment, or format string moves; only these
 * spellings appear outside this header.
 */
#ifndef TYPES_H
#define TYPES_H

#include <stdint.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t s64;

#endif
