// SPDX-License-Identifier: Apache-2.0
/* Simulate a system where no requested locale can be installed */
#include <locale.h>

#include <util.h>

char *setlocale(int category __unused, const char *locale __unused)
{
	return NULL;
}
