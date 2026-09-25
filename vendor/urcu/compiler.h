// SPDX-FileCopyrightText: 2009 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * Reduced to the helper needed by list.h for diffofdiffs on 2026-09-23.
 * The remaining compiler helpers and generated configuration aren't needed.
 */

#ifndef _URCU_COMPILER_H
#define _URCU_COMPILER_H

#include <stddef.h>

/*
 * caa_container_of - Get the address of an object containing a field.
 *
 * @ptr: pointer to the field.
 * @type: type of the object.
 * @member: name of the field within the object.
 */
#define caa_container_of(ptr, type, member)				\
	__extension__							\
	({								\
		const __typeof__(((type *) NULL)->member) * __ptr = (ptr); \
		(type *)((char *)__ptr - offsetof(type, member));	\
	})

#endif /* _URCU_COMPILER_H */
