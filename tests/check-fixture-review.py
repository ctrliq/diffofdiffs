#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check review meaning in a fixture after its normal I/O and status checks."""

import json
import os
from pathlib import Path
import subprocess
import sys

from review_oracle import check_review, lines, parse_patch
from tree_oracle import check_quotations, derive_inputs, tree_sources


def check(binary, arena, fixture):
    spec = {}
    for line in (fixture / 'spec').read_text().splitlines():
        if line and not line.startswith('#'):
            key, value = line.split(':', 1)
            spec[key] = value.strip()
    # These test process handling, not a complete successful comparison
    if (spec.get('bin', 'DIFFOFDIFFS') != 'DIFFOFDIFFS' or
            'stdout-limit' in spec or 'rlimit-fsize' in spec):
        return
    args = spec.get('args', '').split()
    if len(args) < 2 or '--help' in args or '-h' in args or '--version' in args:
        return
    env = dict(os.environ, ASAN_OPTIONS='verify_asan_link_order=1', LSAN_OPTIONS='',
               UBSAN_OPTIONS='halt_on_error=1', LD_PRELOAD='', TMPDIR=str(arena))
    repository = next((a.split('=', 1)[1] for a in args if a.startswith('--git-tree=')), None)
    if '--git-tree' in args:
        repository = args[args.index('--git-tree') + 1]
    stdin = (arena / spec['stdin']).read_bytes() if spec.get('stdin') else None
    if repository is not None:
        repository = (arena / repository).resolve()
        derive = Path(__file__).parent / 'support/derive-patch'
        patches = derive_inputs(repository, args[-2:], derive, env)
        sources = tree_sources(repository, args[-2:], patches)
    else:
        operands = [arena / a for a in args[-2:] if a != '-']
        if not all(p.is_file() or p == Path('/dev/null') for p in operands):
            return
        patches = [stdin if a == '-' else (arena / a).read_bytes() for a in args[-2:]]
        sources = None
        if (fixture / 'p1-source').exists():
            parsed = [parse_patch(p) for p in patches]
            actual = []
            for leg in (0, 1):
                views = [dict(enumerate(lines((fixture / f'p{leg + 1}-{stage}').read_bytes())))
                         for stage in ('source', 'result')]
                actual.append({file.path: views for file in parsed[leg]})
            check_quotations(parsed, actual)
    run = subprocess.run([str(binary), '--color=never', *args], input=stdin,
                         cwd=arena, env=env, capture_output=True, timeout=120)
    (arena / 'review.txt').write_bytes(run.stdout)
    assert run.returncode == 0, (fixture.name, run.returncode, run.stderr)
    assert b'Sanitizer' not in run.stderr and b'runtime error:' not in run.stderr
    report = check_review(patches, run.stdout, sources)
    (arena / 'review-check.json').write_text(json.dumps(
        {'shown': report['shown'], 'shared': report['shared']}) + '\n')


if __name__ == '__main__':
    check(*(Path(p).resolve() for p in sys.argv[1:]))
