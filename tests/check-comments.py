#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Require block-comment stars to align one space after the opening slash."""

from pathlib import Path
import re
import sys

failed = False
for argument in sys.argv[1:]:
    path = Path(argument)
    indent = None
    for number, line in enumerate(path.read_text().splitlines(), 1):
        if indent is None:
            opening = re.fullmatch(r'([ \t]*)/\*', line)
            if opening:
                indent = opening[1]
        else:
            if not line.startswith(indent + ' *'):
                print(f'{path}:{number}: block-comment asterisk is misaligned')
                failed = True
            if '*/' in line:
                indent = None
sys.exit(failed)
