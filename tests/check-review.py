#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check review meaning against original inputs and independently chosen edits."""

from collections import Counter
from contextlib import ExitStack
from dataclasses import replace
from itertools import product
import difflib
import fcntl
import json
import os
from pathlib import Path
import pty
import random
import re
import runpy
import shutil
import struct
import subprocess
import sys
import tty
import termios

from review_oracle import PrintedRow, check_context_windows, check_relationships, check_review, display_text, display_width, lines, matches_source, moved_hunks, paired_cells, parse_patch, parse_review, split_columns
from reindent_cases import check_reindentation_cases

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / 'diffofdiffs'
WORK = ROOT / 'build/tests' / (BINARY.name + '-review')
WORK.mkdir(parents=True, exist_ok=True)
ENV = dict(os.environ, TMPDIR=str(WORK),
           ASAN_OPTIONS='verify_asan_link_order=1',
           LSAN_OPTIONS='', UBSAN_OPTIONS='halt_on_error=1', LD_PRELOAD='')
CHECKS = 0

def patch(before, after, path='f.c'):
    """Write the final-newline markers difflib itself does not provide."""
    result = bytearray()
    text = difflib.diff_bytes(difflib.unified_diff, before, after,
                             ('a/' + path).encode(), ('b/' + path).encode(), n=3)
    for line in text:
        result.extend(line)
        if not line.endswith(b'\n'):
            result.extend(b'\n\\ No newline at end of file\n')
    return bytes(result)

def run_case(name, a, b, expected=None, width=3, columns=100):
    global CHECKS
    directory = WORK / name
    directory.mkdir(exist_ok=True)
    paths = [directory / 'patch1', directory / 'patch2']
    paths[0].write_bytes(a)
    paths[1].write_bytes(b)
    result = subprocess.run([BINARY, '--color=never', f'-U{width}', f'--max-column-width={columns}', *paths],
                            env=ENV, capture_output=True, timeout=20)
    (directory / 'review.txt').write_bytes(result.stdout)
    (directory / 'stderr').write_bytes(result.stderr)
    assert result.returncode == 0, (name, result.returncode, result.stderr)
    assert b'Sanitizer' not in result.stderr and b'runtime error:' not in result.stderr, (name, result.stderr)
    assert b'\x1b' not in result.stdout, (name, 'unexpected color on redirected output')
    report = check_review([a, b], result.stdout, context=width)
    if expected is not None:
        actual = [Counter(), Counter()]
        for leg in (0, 1):
            for row in report['edits'][leg]:
                actual[leg][row.sign, row.text] += 1
        assert actual == expected, (name, actual, expected)
    if not result.stdout:
        html = subprocess.run([BINARY, '--html', f'-U{width}',
                               f'--max-column-width={columns}', *paths],
                              env=ENV, capture_output=True, timeout=20)
        assert (html.returncode, html.stdout, html.stderr) == (
            0, b'', result.stderr), (name, html.returncode, len(html.stdout), html.stderr)
        CHECKS += 1
    CHECKS += 1
    return paths, result.stdout, report

def expected(a, b):
    inventories = [Counter((row.sign, row.text)
                            for file in parse_patch(p) for row in file.rows
                            if row.sign in '+-' and not row.neutral)
                   for p in (a, b)]
    return [inventories[0] - inventories[1], inventories[1] - inventories[0]]

def terminal(args, columns=0, destination_tty=False):
    master, slave = pty.openpty()
    tty.setraw(slave)
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 24, columns, 0, 0))
    command = [BINARY, *args]
    if destination_tty:
        command += ['-o', os.ttyname(slave)]
    process = subprocess.Popen(command, stdout=subprocess.PIPE if destination_tty else slave,
                               stderr=subprocess.PIPE, env=ENV,
                               pass_fds=(slave,) if destination_tty else ())
    os.close(slave)
    chunks = []
    try:
        while True:
            try:
                part = os.read(master, 65536)
            except OSError:
                break
            if not part:
                break
            chunks.append(part)
    finally:
        os.close(master)
    standard, error = process.communicate(timeout=10)
    assert process.returncode == 0, error
    assert not standard, ('TTY destination also wrote to stdout', standard)
    return b''.join(chunks)

def main():
    global CHECKS
    head, tail, old, new, extra = (x + b'\n' for x in (b'head', b'tail', b'old', b'new', b'extra'))
    a = patch([head, tail, old], [head, extra, tail, new])
    b = patch([head, extra, tail, old], [head, extra, tail, new])
    run_case('addition-versus-retained', a, b, expected(a, b))
    a = patch([head, extra, old, tail], [head, new, tail])
    b = patch([head, old, tail], [head, new, tail])
    run_case('removal-survives-equal-results', a, b, expected(a, b))
    a = patch([head, old, tail], [head, b'apple\n', tail])
    b = patch([head, old, tail], [head, b'pear\n', tail])
    paths, _, _ = run_case('different-additions', a, b, expected(a, b))
    run_case('identical', a, a, [Counter(), Counter()])
    run_case('unchanged-context-differs',
             patch([b'left\n', old, tail], [b'left\n', new, tail]),
             patch([b'right\n', old, tail], [b'right\n', new, tail]),
             [Counter(), Counter()])
    a = b'--- a/f.c\n+++ b/f.c\n@@ -1,3 +1,3 @@\n head\n-old\n+old\n tail\n'
    b = patch([head, old, tail], [head, new, tail])
    run_case('adjacent-no-effect', a, b)
    context_inputs = [patch([label, old, tail], [label, new, tail])
                      for label in (b'left context\n', b'right context\n')]
    for width in (0, 1, 3):
        _, _, report = run_case(f'context-kept-U{width}', *context_inputs, width=width)
        differences = [(r.leg, r.pos[1], r.sign, r.text) for r in report['rows']
                       if r.section == 'context' and r.sign in '+-']
        assert differences == [(0, 0, '-', b'left context\n'),
                               (1, 0, '+', b'right context\n')], 'margin hid a context difference'
    for spelling, source in [('tab', b'\tcontext\n'), ('blank', b'\n')]:
        a = patch([source, old, tail], [source, b'first\n', tail])
        b = patch([source, old, tail], [source, b'second\n', tail])
        a = a.replace(b' ' + source, source)
        b = b.replace(b' ' + source, source)
        run_case('signless-' + spelling, a, b, expected(a, b))
    for left_lf in (False, True):
        for right_lf in (False, True):
            a = patch([head, b'old'], [head, b'left' + (b'\n' if left_lf else b'')])
            b = patch([head, b'old'], [head, b'right' + (b'\n' if right_lf else b'')])
            run_case(f'final-lf-{left_lf}-{right_lf}', a, b, expected(a, b))
    a = patch([old], [b'--- literal\n', b'```\n'])
    b = patch([old], [b'+++ literal\n', b'````\n'])
    _, escaped, _ = run_case('header-like-source-and-fences', a, b, expected(a, b))
    assert b'--- literal' in escaped and b'+++ literal' in escaped
    a = b'--- a/f.c\n+++ b/f.c\n@@ -999999999,1 +999999999,1 @@\n-old\n+left\n'
    b = a.replace(b'left', b'right')
    _, sparse, _ = run_case('large-sparse-coordinates', a, b, expected(a, b))
    assert len(sparse) < 8192
    a = b'--- quoted/thing.c\n' + patch([old], [new], 'x.c')
    b = patch([old], [new], 'y.c')
    _, _, report = run_case('stray-header-keeps-real-pair', a, b)
    assert report['shown'] == [0, 0]
    a = b'--- quoted/thing.c\n' + patch([old], [new, extra], 'x.c')
    _, _, report = run_case('stray-header-keeps-exclusive-edit', a, b)
    assert report['shown'] == [3, 2]
    a = patch([old], [new], 'x.c') + patch([old], [new], 'y.c')
    b = patch([old], [new], 'z.c')
    _, _, report = run_case('duplicate-moved-change-stays-visible', a, b)
    assert report['shown'] == [4, 2]
    a = (b'--- a/f.c\n+++ b/f.c\n@@ -1,2 +1,2 @@\n one\n-old\n+first\n'
         b'@@ -1,2 +1,2 @@\n other\n-old\n+second\n')
    run_case('contradictory-self-comparison', a, a, [Counter(), Counter()])
    run_case('contradictory-quotations', a, patch([b'one\n', old], [b'one\n', new]))

    # Deliberately unique source rows give an independent location oracle.
    # Common signed rows must be omitted, while each exclusive edit is shown.
    rng = random.Random(866299)
    for case in range(100):
        base = [f'row {i:03d};\n'.encode() for i in range(60)]
        before = [base.copy(), base.copy()]
        after = [base.copy(), base.copy()]
        shared = rng.sample(range(5, 55), 4)
        exclusive = rng.sample([i for i in range(5, 55) if i not in shared], 2)
        for leg in (0, 1):
            for i in shared:
                after[leg][i] = f'shared edit {i:03d};\n'.encode()
            after[leg][exclusive[leg]] = f'exclusive {leg} at {exclusive[leg]:03d};\n'.encode()
            before[leg][0] = after[leg][0] = f'background {leg};\n'.encode()
        a, b = [patch(before[leg], after[leg]) for leg in (0, 1)]
        run_case(f'located-edits-{case:03d}', a, b, expected(a, b))

    plain = subprocess.check_output([BINARY, *paths], env=ENV)
    assert b'\x1b' not in plain
    auto = terminal(paths)
    assert b'\x1b[32m' in auto
    assert re.sub(rb'\x1b\[[0-9;]*m', b'', auto) == plain
    assert terminal(['--color=never', *paths]) == plain
    forced = subprocess.check_output([BINARY, '--color=always', *paths], env=ENV)
    assert b'\x1b' in forced and re.sub(rb'\x1b\[[0-9;]*m', b'', forced) == plain

    # Shared deletions leave no red lines, but word differences can still use red
    line_colors = terminal(['--highlight=none', *paths])
    assert b'\x1b[32m' in line_colors and b'\x1b[31m' not in line_colors
    CHECKS += 5
    CHECKS += check_reindentation_cases(BINARY, WORK, ENV)
    print(f'PASS {CHECKS} original-source, ownership, layout, and terminal checks ({BINARY.name})')

if __name__ == '__main__':
    main()
