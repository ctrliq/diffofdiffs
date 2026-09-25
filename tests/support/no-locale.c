// SPDX-License-Identifier: GPL-2.0-only
/* Simulate a system where no requested locale can be installed */
#include <locale.h>

#include <util.h>

char *setlocale(int category __unused, const char *locale __unused)
{
	return NULL;
}
