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


def expanded_terminal_source(source, tab_width):
    """Expand escaped source independently, counting Unicode display columns."""
    result = []
    column = 0
    for char in display_text(source).decode('utf-8', 'surrogateescape'):
        if char == '\t':
            spaces = tab_width - column % tab_width
            result.append(' ' * spaces)
            column += spaces
        else:
            result.append(char)
            column = 0 if char == '\n' else column + display_width(char)
    return ''.join(result).encode('utf-8', 'surrogateescape')


def check_terminal_source(reference, output, tab_width=8):
    """Audit presentation against rows already checked against original patches.

    The raw-byte oracle remains strict about tabs. This separate display check
    requires the same ordered rows, roles, coordinates, and pairings, with only
    source-origin tab expansion allowed to change their rendered characters.
    """
    known_paths = {path for row in reference['rows'] for path in row.paths}
    printed = parse_review(output, known_paths)
    assert b'\t' not in output, 'TTY output contains an unexpanded tab'
    assert len(printed) == len(reference['rows']), 'TTY changed the selected rows'
    fields = ('section', 'paths', 'group', 'leg', 'sign', 'pos', 'shared', 'pair')
    for before, after in zip(reference['rows'], printed):
        assert all(getattr(before, key) == getattr(after, key) for key in fields), (
            'TTY changed row identity or correspondence', before, after)
        expanded = expanded_terminal_source(before.source, tab_width)
        assert matches_source(after, expanded), (
            'TTY changed expanded source or final newline', after, expanded)
    return printed


def column_text(output):
    """Recover wrapped cell text for assertions about file notes."""
    physical = re.sub(r'\x1b\[[0-9;]*m', '', output.decode()).splitlines()
    width = len(next(line for line in physical if re.fullmatch(r'\u2501+|-+', line)))
    gutter = next(m.end() for line in physical
                  if (m := re.match(r'^ (?: *old +new| +new)[\u2502|]', line)))
    cell_width = (width - 1) // 2
    result = ['', '']
    for line in physical:
        left, rest = split_columns(line, cell_width)
        if not rest or rest[0] not in ('\u2503', '|'):
            continue
        right = rest[1:]
        for leg, cell in enumerate((left, right)):
            if len(cell) >= gutter and cell[gutter-1] in ('\u2502', '|'):
                result[leg] += cell[gutter:] + ' ' * (cell_width - display_width(cell))
    return '\n'.join(result).encode()


def oracle_negative_cases():
    """Prove that independent source checks reject plausible bad reports."""
    global CHECKS
    before = [b'head\n', b'old\n']
    a = patch(before, [b'head\n', b'apple\n'])
    b = patch(before, [b'head\n', b'pear\n'])
    _, output, _ = run_case('oracle-control', a, b, expected(a, b))
    row = next(line for line in output.splitlines(keepends=True) if b'apple' in line)
    _, no_lf, _ = run_case('oracle-newline-control', patch(before, [b'head\n', b'apple']), b)
    wrong_number = re.sub(rb'([+] +)(2)(\xe2\x94\x82apple)', rb'\g<1>9\3', output, count=1)
    bad_reports = {
        'fabricated-source': output.replace(b'apple', b'xxxxx'),
        'wrong-owner': output.replace(b'apple', b'pear '),
        'wrong-coordinate': wrong_number,
        'wrong-file': output.replace(b'f.c', b'x.c'),
        'missing-edit': output.replace(row, b'', 1),
        'duplicated-edit': output.replace(row, row + row, 1),
        'changed-final-newline': no_lf,
        'invented-context': output.replace(b'head', b'xxxx', 1),
        'falsely-shared': output.replace(row, b' ' + row[1:], 1)
    }
    for name, bad in bad_reports.items():
        assert bad != output, name
        try:
            check_review([a, b], bad)
        except AssertionError:
            CHECKS += 1
        else:
            raise AssertionError(('oracle accepted a deliberately bad report', name))

    prefix = [f'prefix{i}\n'.encode() for i in range(2)]
    suffix = [f'suffix{i}\n'.encode() for i in range(3)]
    before = [prefix + [side, b'old\n'] + suffix
              for side in (b'left context\n', b'right context\n')]
    after = [prefix + [side, b'new\n'] + suffix
             for side in (b'left context\n', b'right context\n')]
    patches = [patch(before[leg], after[leg]) for leg in (0, 1)]
    _, _, review = run_case('oracle-relationships', *patches)
    rows = [r for r in review['rows'] if r.section == 'context']
    sources = [{'f.c': [dict(enumerate(before[leg])), dict(enumerate(after[leg]))]}
               for leg in (0, 1)]
    parsed = [parse_patch(p) for p in patches]
    check_relationships(rows, sources)
    check_context_windows(parsed, rows, sources, 3)
    bad_rows = {
        'false-paired-equality': [replace(r, sign=' ') if r.sign in '+-' else r for r in rows],
        'interior-source-hole': [r for r in rows if r.pos[1] != 1],
        'missing-leading-margin': [r for r in rows if r.pos[1] != 0],
        'source-order': rows[::-1],
    }
    for name, bad in bad_rows.items():
        try:
            check_relationships(bad, sources)
            check_context_windows(parsed, bad, sources, 3)
        except AssertionError:
            CHECKS += 1
        else:
            raise AssertionError(('relationship oracle accepted bad rows', name))


    before = [f'row {i}\n'.encode() for i in range(101)]
    after = before.copy()
    after[50] = b'changed\n'
    inputs = [b'', patch(before, after)]
    sources = [{'f.c': [{}, {}]}, {'f.c': [dict(enumerate(before)), dict(enumerate(after))]}]
    excessive = [PrintedRow('context', ('f.c', 'f.c'), 0, 1, '+', text,
                            (-1, i), pair=i, source=text) for i, text in enumerate(after)]
    check_relationships(excessive, sources)
    try:
        check_context_windows([parse_patch(p) for p in inputs], excessive, sources, 3)
    except AssertionError:
        CHECKS += 1
    else:
        raise AssertionError('scope oracle accepted an unrelated whole file')


def neutral_move_oracle_cases():
    """No-op rewrites cannot create or distinguish a transferred edit."""
    global CHECKS
    headers = [b'--- a/left.c\n+++ b/left.c\n',
               b'--- a/right.c\n+++ b/right.c\n']
    neutral = b'@@ -1,3 +1,3 @@\n-keep\n+keep\n anchor\n end\n'
    with_edit = neutral.replace(b'+1,3', b'+1,4').replace(b' end\n', b'+added\n end\n')
    retained = with_edit.replace(b'-keep\n+keep\n', b' keep\n')
    for bodies, wanted in [([neutral, neutral], [{}, {}]),
                           ([with_edit, retained], [{(0, 0): (0, 0)}, {(0, 0): (0, 0)}])]:
        for order in (bodies, bodies[::-1]):
            parsed = [parse_patch(header + body) for header, body in zip(headers, order)]
            assert moved_hunks(parsed) == wanted, (order, moved_hunks(parsed))
            CHECKS += 1


def context_margin_cases():
    """Independently pin the exact context around additions and deletions."""
    global CHECKS
    before = [b'before\n', b'obsolete\n', b'after\n']
    surrounding = [b'before\n', b'b3\n', b'b2\n', b'b1\n', b'left\n',
                   b'a1\n', b'a2\n', b'a3\n', b'after\n']
    for role in ('add', 'delete'):
        inputs = []
        for word in (b'left\n', b'right\n'):
            content = surrounding.copy()
            content[4] = word
            inputs.append(patch(before, content) if role == 'add' else
                          patch(content, [b'before\n', b'after\n']))
        for width in (0, 1, 2, 3, 4, 8):
            paths, output, report = run_case(
                f'context-{role}-U{width}', *inputs, expected(*inputs), width=width)
            for leg, word in enumerate((b'left\n', b'right\n')):
                rows = [r for r in report['rows'] if r.section == 'delta' and r.leg == leg]
                prefix = [b'b3\n', b'b2\n', b'b1\n'][-width:] if width else []
                suffix = [b'a1\n', b'a2\n', b'a3\n'][:width]
                if width >= 4:
                    prefix.insert(0, b'obsolete\n' if role == 'add' else b'before\n')
                    suffix.append(b'after\n')
                if width >= 5 and role == 'add':
                    prefix.insert(0, b'before\n')
                assert [r.text for r in rows] == prefix + [word] + suffix, (
                    'wrong context extent', role, width, leg, rows)
                assert [r.text for r in rows if not r.shared and r.sign in '+-'] == [word]
                assert all(r.shared for r in rows if r.text[:1] in (b'a', b'b')
                           and r.text not in (b'after\n', b'before\n'))
            if width == 3:
                default = subprocess.check_output([BINARY, '--color=never', *paths], env=ENV)
                assert default == output, 'default differs from -U3'
            plain = subprocess.check_output([BINARY, f'-U{width}', *paths], env=ENV)
            for word in prefix + suffix:
                assert word.rstrip(b'\n') in plain, ('plain view lost context', word)
            CHECKS += 1


def wrapping_cases():
    """Wrapped source keeps its coordinates, bytes, and final newline."""
    global CHECKS
    old = [b'head\n', b'old\n', b'tail\n']
    code = ('\t\tcall_with_a_long_identifier(' + 'argument_' * 14 + ');\n').encode()
    unicode = ('\t/* ' + '\u754c\u00e9' * 30 + ' */\n').encode()
    literal = b'\t> literal continuation-looking source\n'
    path = 'long_' * 16 + 'name.c'
    a = patch(old, [old[0], code, unicode, literal, b'left'], path)
    b = patch(old, [old[0], code, unicode, literal, b'right'], path)
    paths, output, report = run_case('wrapped-source-identity', a, b, expected(a, b), columns=39)
    assert any(r.shared and b'call_with' in r.text for r in report['rows'])
    assert max(display_width(line) for line in output.decode().splitlines()) == 97
    assert all(row.columns == 39 for row in report['rows'])
    assert any(len(row.fragments) > 1 for row in report['rows'])
    bad = output.replace(b'argument_', b'xxxxxxxxx', 1)
    assert bad != output
    try:
        check_review([a, b], bad)
    except AssertionError:
        CHECKS += 1
    else:
        raise AssertionError('oracle accepted changed continuation text')
    expanded = output.replace(b'\t', b' ' * 8, 1)
    assert expanded != output
    try:
        check_review([a, b], expanded)
    except AssertionError:
        CHECKS += 1
    else:
        raise AssertionError('oracle accepted a source tab replaced by spaces')
    for columns in (2, 8, 40, 100, 200):
        _, _, report = run_case(f'wrapped-width-{columns}', a, b, expected(a, b), columns=columns)
        assert all(r.columns <= max(columns, 8) for r in report['rows'])
    for columns in (60, 80, 120):
        output = terminal(['--color=never', *paths], columns=columns)
        assert all(display_width(line) <= max(columns, 35) for line in output.decode().splitlines())
        check_terminal_source(report, output)
        CHECKS += 1
    a = patch(old, [b'\x80' * 100 + b'\n', b'left\n'])
    b = patch(old, [b'\x80' * 100 + b'\n', b'right\n'])
    run_case('wrapped-non-utf8-source', a, b, expected(a, b), columns=23)
    a = patch(old, [b'control \x1b \r \b end\n', b'left\n'])
    b = patch(old, [b'control \x1b \r \b end\n', b'right\n'])
    run_case('nonprinting-source', a, b, expected(a, b))

    # A separator at the width limit must not push a complete word down
    a = patch([b'old\n'], [b'left\n'])
    b = patch([b'old\n'], [b'right\n'])
    _, narrow, _ = run_case('wrapped-description-whole-word', a, b,
                            expected(a, b), columns=2)
    assert b'it, an old number means\n' in narrow


def check_number_field_spacing(output, report, digits):
    gutter = 2 * digits + 3
    starts = {row.source_start for row in report['rows']}
    checked = 0
    for line in output.decode().expandtabs(8).splitlines():
        for start in starts:
            cell = line[start-gutter:start]
            fields = re.fullmatch(r' *([ +\\-])( *[0-9]*) ( *[0-9]*)[\u2502|]', cell)
            if not fields:
                continue
            role, old, new = fields.groups()
            old, new = old.strip(), new.strip()
            if not old and not new and role == ' ':
                continue
            expected = role + f'{old:>{digits}} {new:>{digits}}'
            assert cell[:-1] == expected, ('spread number fields', cell, expected)
            checked += 1
    assert checked, 'no source gutters were checked'


def compact_tab_gutter_cases():
    """Keep literal tabs from adding any space to the number gutters."""
    before = [b'\told;\n']
    inputs = [patch(before, [b'\t' + b'x' * offset + b'\t' + word + b';\n'
                             for offset in range(8)])
              for word in (b'left', b'right')]
    for start, digits in ((1, 3), (997, 4), (9997, 5), (99997, 6),
                           (100000, 6), (1000000, 7), (9999997, 8)):
        sparse = [item.replace(b'@@ -1 +1,8 @@',
                               f'@@ -{start} +{start},8 @@'.encode())
                  for item in inputs]
        header = f' {"old":>{digits}} {"new":>{digits}}\u2502'
        for columns in (2, 8, 9, 10, 11, 12, 13, 14, 15, 39, 80):
            _, output, report = run_case(f'compact-tabs-{start}-{columns}',
                                         *sparse, expected(*sparse), columns=columns)
            assert any(line.startswith(header) for line in output.decode().splitlines()), (
                'tabs widened the number gutter', start, columns)
            check_number_field_spacing(output, report, digits)
            assert min(row.source_start for row in report['rows']) == 2 * digits + 3, (
                'extra padding before source', start, columns)

    inputs = [patch(before, [b'\t' + word + b';'])
              for word in (b'left', b'right')]
    _, output, report = run_case('compact-tabs-no-final-newline', *inputs,
                                 expected(*inputs), columns=39)
    check_number_field_spacing(output, report, 3)

    inputs = [patch([b'#define\tOLD\n'], [b'#define\t' + word + b'\n'])
              for word in (b'LEFT', b'RIGHT')]
    _, _, report = run_case('compact-tabs-natural-width', *inputs,
                            expected(*inputs), columns=80)
    assert all(len(row.fragments) == 1 for row in report['rows']), (
        'source tabs caused avoidable wrapping below the width limit')

    indentation = (b'\t', b' ' * 8, b'   \t', b'\t   ', b' \t \t')
    inputs = [patch(before, [prefix + f'{word}{i}();\n'.encode()
                             for i, prefix in enumerate(indentation)])
              for word in ('left', 'right')]
    _, output, report = run_case('mixed-indentation-stops', *inputs,
                                 expected(*inputs), columns=80)
    physical = output.decode().expandtabs(8).splitlines()
    for leg, word in enumerate(('left', 'right')):
        start = next(row.source_start for row in report['rows'] if row.leg == leg)
        for i, prefix in enumerate(indentation):
            body = f'{word}{i}();'
            row = next(line for line in physical if body in line)
            columns = len((' ' * start + prefix.decode()).expandtabs(8)) - start
            assert row.index(body) == start + columns, (
                'mixed tab and space indentation changed width', leg, i)


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


def terminal_layout_cases():
    """Keep tab stops source-relative while selecting width from the destination."""
    global CHECKS
    before = [b'\told_value();\n']
    after = []
    for leg, word in enumerate((b'left', b'right')):
        after.append([
            b'\t\tcall_' + word + b'();\n',
            b'  \t \tcall_' + word + b'();\n',
            b'\tvalue =\targument_' + word + b';\n',
            '\t界é\u0301\t'.encode() + word + b';\n',
            b'\t\xff\t' + word + b'\x1b;\n',
            (b' \t ' if leg == 0 else b' ' * 9) + b'changed_tabs();\n',
            b'\t' + word + b' ' * 3 + b'\t\n',
            b'\t\t\t\n',
            b'\t' + word + b'_without_final_newline'
        ])
    original = [patch(before, rows) for rows in after]
    for start, digits in ((1, 3), (99997, 6)):
        sparse = [item.replace(b'@@ -1 +1,9 @@',
                               f'@@ -{start} +{start},9 @@'.encode())
                  for item in original]
        paths, raw, reference = run_case(f'terminal-tabs-{start}', *sparse,
                                         expected(*sparse))
        for tab_width, cap in product((1, 4, 8, 16), (2, 7, 31, 80)):
            args = ['--color=never', f'--tab-width={tab_width}',
                    f'--max-column-width={cap}', *paths]
            output = terminal(args, columns=223)
            rows = check_terminal_source(reference, output, tab_width)
            assert all(row.columns <= cap for row in rows)
            check_number_field_spacing(output, {'rows': rows}, digits)
            assert min(row.source_start for row in rows) == 2 * digits + 3
            if cap == 80:
                # The right pane has no layout padding to mask lost source spaces
                trailing = expanded_terminal_source(after[1][6], tab_width)
                assert '\u2502'.encode() + trailing in output
            CHECKS += 1

        # Expanding either the source or the gutter origin gives different bytes
        ordinary = terminal(['--color=never', '--tab-width=8', *paths], columns=223)
        selected = terminal(['--color=never', '--tab-width=8', *paths], columns=223,
                            destination_tty=True)
        assert selected == ordinary, '-o TTY ignored its own terminal dimensions'
        assert terminal(['--color=never', *paths], columns=223) == ordinary
        CHECKS += 2

        destination = Path(paths[0]).parent / 'redirected.txt'
        assert terminal(['--color=never', '--tab-width=16', '-o', destination,
                         *paths], columns=40) == b''
        assert destination.read_bytes() == raw, 'stdout TTY changed redirected bytes'
        assert b'\t' in raw
        CHECKS += 1

    long = [patch(before, [b'\t' + word + b'x' * 400 + b'\n'])
            for word in (b'left', b'right')]
    paths, raw, reference = run_case('terminal-width-default', *long, expected(*long))
    for columns, cap, source_width in ((500, None, 240), (80, None, 30),
                                       (0, None, 100), (500, 41, 41),
                                       (80, 100, 30), (1, None, 2)):
        args = ['--color=never', *paths]
        if cap is not None:
            args += [f'--max-column-width={cap}']
        output = terminal(args, columns=columns)
        rows = check_terminal_source(reference, output)
        assert {row.columns for row in rows} == {source_width}, (
            'wrong source width for TTY', columns, cap, rows)
        CHECKS += 1

    # Redirected width doesn't depend on COLUMNS or the visual tab setting
    for tab_width in (1, 4, 8, 16):
        output = subprocess.check_output([BINARY, '--color=never',
                                          f'--tab-width={tab_width}', *paths],
                                         env=dict(ENV, COLUMNS='500'))
        assert output == raw
        CHECKS += 1

    small = [patch([b'x\n'], [b'\t' + word + b'\n']) for word in (b'a', b'b')]
    paths, _, reference = run_case('terminal-natural-width', *small, expected(*small))
    output = terminal(['--color=never', *paths], columns=500)
    assert {row.columns for row in check_terminal_source(reference, output)} == {9}
    CHECKS += 1

    for option in ('-w', '--tab-width=0', '--tab-width=-1', '--tab-width=-0',
                   '--tab-width=', '--tab-width=1x', '--tab-width=2147483648'):
        result = subprocess.run([BINARY, option, *paths], env=ENV, capture_output=True)
        assert result.returncode and not result.stdout, (option, result)
        CHECKS += 1


def tree_cases():
    global CHECKS
    generator = ROOT / 'tests/support/gitread-unit'
    derive = ROOT / 'tests/support/derive-patch'
    cases = json.loads((ROOT / 'tests/context-cases.json').read_text())
    cases += json.loads((ROOT / 'tests/correspondence-trees.json').read_text())
    background = [f'prefix{i}\n' for i in range(50)]
    tail = [f'suffix{i}\n' for i in range(50)]
    cases.append(dict(name='one-sided-context',
                      before=['', ''.join(background + ['old\n'] + tail)],
                      after=['', ''.join(background + ['new\n'] + tail)]))
    prefix = '/* before0 */\n/* before1 */\n/* before2 */\n'
    body = ('{\n\tstruct env *env;\n\tstruct item *head;\n\tint ret;\n\n'
            '\treturn head;\n}\n')
    before = [prefix + signature + body for signature in (
        'static struct item *\ntarget(int arg)\n',
        'static struct item *target(long arg)\n')]
    cases.append(dict(name='split-function-declaration', before=before,
                      after=[s.replace('*head;', '*head, *next;') for s in before]))
    prefix = [f'prefix{i}\n' for i in range(20)] + ['anchor\n']
    suffix = ['boundary\n'] + [f'suffix{i}\n' for i in range(5)]
    before = [prefix + ['join\n', 'left tail\n'] + suffix,
              prefix + [f'inherited{i}\n' for i in range(10)] +
              ['join\n', 'right tail\n'] + suffix]
    after = [side.copy() for side in before]
    after[0][5] = after[1][5] = 'shared edit\n'
    after[1][25] = 'changed inherited row\n'
    cases.append(dict(name='gap-context-margin',
                      before=[''.join(side) for side in before],
                      after=[''.join(side) for side in after]))
    prefix = ('static int previous(void)\n{\n#ifdef WHICH\n\tif (a) {\n'
              '#else\n\tif (b) {\n#endif\n\t\treturn 1;\n\t}\n'
              '\treturn 0;\n}\nEXPORT_SYMBOL_GPL(previous);\n\n')
    before = prefix + 'static int\ntarget(int value)\n{\n\treturn value;\n}\n'
    for name, old, replacements in (
            ('signature', 'target(int value)', ('target(long value)', 'target(unsigned value)')),
            ('return-type', 'static int\ntarget', ('static short\ntarget', 'static long\ntarget'))):
        cases.append(dict(name='label-' + name, before=[before, before],
                          after=[before.replace(old, replacement) for replacement in replacements],
                          context=False,
                          labels=list(replacements) if name == 'signature' else ['target(int value)'] * 2))
    repeated = '    repeated();\n' * 9
    guard = '    if (unlisted) {\n        first();\n        second();\n        third();\n        return;\n    }\n'
    before = ['start\nleft\n' + repeated + 'finish\n',
              'start\n' + repeated + 'right\nfinish\n']
    after = [before[0].replace('left\n', 'left\n' + guard),
             before[1].replace('right\n', 'right\n' + guard)]
    cases.append(dict(name='edit-before-background', before=before, after=after,
                      shared_positions=[(2 + i, 11 + i) for i in range(6)]))
    cases.append(dict(cases[-1], name='edit-before-background-files', moved=True))
    before = 'site_one\nretained_one\nend_one\n' + ''.join(
        f'between_{i}\n' for i in range(20)) + 'site_two\nretained_two\nend_two\n'
    cases.append(dict(name='different-insertion-sites', before=[before, before],
                      after=[before.replace('site_one\n', 'site_one\nidentical addition\n'),
                             before.replace('site_two\n', 'site_two\nidentical addition\n')],
                      context=False, distinct_positions=[1, 24]))
    for case in cases:
        directory = WORK / ('tree-' + case['name'])
        directory.mkdir(exist_ok=True)
        shutil.rmtree(directory / 'repo', ignore_errors=True)
        for leg in (0, 1):
            (directory / f'p{leg + 1}-source').write_bytes(case['before'][leg].encode())
            (directory / f'p{leg + 1}-result').write_bytes(case['after'][leg].encode())
        family = 'recipe-moved' if case.get('moved') else 'recipe-c'
        paths = ['f.c', 'g.c' if case.get('moved') else 'f.c']
        subprocess.run([generator, 'build-store', family, directory / 'repo'],
                       env=ENV, cwd=directory, check=True, capture_output=True)
        patches = [subprocess.check_output([derive, directory / 'repo', revision], env=ENV)
                   for revision in ('bp', 'up')]
        sources = [{paths[leg]: [dict(enumerate(lines(case['before'][leg].encode()))),
                            dict(enumerate(lines(case['after'][leg].encode())))]}
                   for leg in (0, 1)]
        for leg in (0, 1):
            for path in paths:
                sources[leg].setdefault(path, [{}, {}])
        delta_rows = None
        for width in (0, 1, 3, 8, 12):
            result = subprocess.run([BINARY, '--color=never', f'-U{width}',
                                     f'--git-tree={directory}/repo', 'bp', 'up'],
                                    env=ENV, capture_output=True, timeout=20)
            (directory / f'review-U{width}.txt').write_bytes(result.stdout)
            assert result.returncode == 0, (case['name'], result.stderr)
            diagnostic = b"diffofdiffs: bp doesn't contain a patch\n" if case['name'] == 'one-sided-context' else b''
            assert result.stderr == diagnostic, (case['name'], result.stderr)
            report = check_review(patches, result.stdout, sources, context=width)
            if result.stdout:
                check_report_titles(result.stdout, ['Commit 1: bp', 'Commit 2: up'])
            delta = sorted((row.leg, row.sign, row.pos[row.sign == '+'])
                           for row in report['rows'] if row.section == 'delta' and row.sign in '+-' and not row.shared)
            if delta_rows is None:
                delta_rows = delta
            assert delta_rows == delta, ('display width changed the edits', case['name'], width)
            context = [row for row in report['rows'] if row.section == 'context' and row.sign in '+-']
            boundary = case['name'] in ('beginning-of-file', 'end-of-file')
            assert bool(context) == case.get('context', not boundary or width >= 12), (
                'surrounding source extent', case['name'], width, context)
            if case['name'] == 'moved-neighborhood' and width == 3:
                assert sum(b'prefix' in row.text for row in context) == 2
                assert not any(b'shared' in row.text for row in report['rows']
                               if row.section == 'delta' and row.sign in '+-' and not row.shared)
            if case['name'] == 'long-context-difference' and width == 3:
                assert not any(b'new_tail19' in row.text for row in context)
                assert not any(row.text.strip() == b'end' for row in context)
                assert {r.pos[1] for r in context if r.leg == 0} == {1, 2}
                assert {r.pos[1] for r in context if r.leg == 1} == set(range(1, 8))
            if case['name'] == 'one-sided-context':
                extent = max(3, width) + width
                assert {r.pos[1] for r in context if r.leg == 1} == set(range(50 - extent, 51 + extent)), 'wrong bounded source margin'
                assert not any(r.leg == 0 for r in context), 'invented counterpart source'
            if case['name'] == 'repeated-text':
                assert any(b'init' in row.text for row in context)
            if case['name'] == 'split-function-declaration' and width == 3:
                assert {r.pos[1] for r in context if r.leg == 0} >= {3, 4}, 'return type was clipped'
                assert any(r.leg == 1 and r.pos[1] == 3 for r in context)
            if case['name'] == 'gap-context-margin' and width == 3:
                for leg, wanted in enumerate((range(18, 24), range(19, 32))):
                    positions = {r.pos[1] for r in report['rows']
                                 if r.section == 'context' and r.leg == leg}
                    assert positions == set(wanted), ('wrong gap source margin', leg, positions)
            if 'labels' in case and width == 0:
                for label in case['labels']:
                    assert ('@@ ' + label).encode() in column_text(result.stdout), ('wrong declaration label', case['name'], result.stdout)
                assert b'@@ EXPORT_SYMBOL' not in result.stdout
            if 'shared_positions' in case:
                assert not delta, ('shared edit was moved into background', case['name'], delta)
                if width == 3:
                    pairs = {}
                    for row in report['rows']:
                        if row.section == 'context':
                            pairs.setdefault(row.pair, {})[row.leg] = row
                    equal = {(p[0].pos[1], p[1].pos[1]) for p in pairs.values()
                             if len(p) == 2 and p[0].sign == p[1].sign == ' '}
                    assert set(case['shared_positions']) <= equal, 'shared block lost its original location'
            if 'distinct_positions' in case:
                assert delta == [(leg, '+', pos) for leg, pos in enumerate(case['distinct_positions'])], 'identical text at different sites is not a shared edit'
            if 'expected_delta_rows' in case:
                assert delta == [tuple(row) for row in case['expected_delta_rows']], (case['name'], delta)
            if width == 3 and 'equal_context' in case:
                pairs = {}
                for row in report['rows']:
                    if row.section == 'context':
                        pairs.setdefault((row.paths, row.pair), {})[row.leg] = row
                equal = {(p[0].pos[1], p[1].pos[1]) for p in pairs.values()
                         if len(p) == 2 and p[0].sign == p[1].sign == ' '}
                assert set(map(tuple, case['equal_context'])) <= equal, (case['name'], equal)
                forbidden = set(map(tuple, case.get('unpaired_context', [])))
                assert not any((leg, pair[leg]) in forbidden for pair in equal for leg in (0, 1)), 'unrelated functions share a context row'
            if 'expected_delta_text' in case:
                expected_edits = Counter((1, sign, text.encode() + b'\n')
                                         for sign in '-+' for text in case['expected_delta_text'])
                actual_edits = Counter((r.leg, r.sign, r.source) for r in report['rows']
                                       if r.section == 'delta' and r.sign in '+-' and not r.shared)
                assert actual_edits == expected_edits, 'a shared removal block was split into exclusive edits'
            if any(k in case for k in ('shared_positions', 'distinct_positions', 'expected_delta_text', 'expected_delta_rows')):
                reverse = subprocess.run([BINARY, '--color=never', f'-U{width}',
                                          f'--git-tree={directory}/repo', 'up', 'bp'],
                                         env=ENV, capture_output=True, timeout=20)
                assert reverse.returncode == 0 and not reverse.stderr, reverse.stderr
                (directory / f'reverse-U{width}.txt').write_bytes(reverse.stdout)
                reversed_report = check_review(patches[::-1], reverse.stdout,
                                               sources[::-1], context=width)
                assert paired_cells(report['rows']) == paired_cells(
                    reversed_report['rows'], reverse=True), 'operand order changed native correspondence'
                CHECKS += 1
            CHECKS += 1


def create_git_store(name):
    """Build a disposable object store with deterministic commit identities."""
    directory = WORK / name
    directory.mkdir(exist_ok=True)
    repo = directory / 'repo'
    shutil.rmtree(repo, ignore_errors=True)
    env = dict(ENV, GIT_CONFIG_NOSYSTEM='1', GIT_CONFIG_GLOBAL=os.devnull,
               GIT_DEFAULT_HASH='sha1', GIT_AUTHOR_NAME='Example',
               GIT_AUTHOR_EMAIL='example@example.com',
               GIT_COMMITTER_NAME='Example', GIT_COMMITTER_EMAIL='example@example.com',
               GIT_AUTHOR_DATE='2000-01-01T00:00:00Z',
               GIT_COMMITTER_DATE='2000-01-01T00:00:00Z')
    subprocess.run(['git', 'init', '--bare', '--quiet', repo], env=env,
                   check=True, capture_output=True)

    def git(*args, data=None):
        return subprocess.check_output(['git', '-C', repo, *args], input=data, env=env)

    return repo, git


def gitlink_cases():
    """Gitlink IDs describe submodule commits, not blobs in the parent store."""
    global CHECKS
    repo, git = create_git_store('gitlinks')

    def commit(oid, parent=None):
        entry = f'160000 commit {oid}\tmodule\n'.encode() if oid else b''
        tree = git('mktree', '--missing', data=entry).strip().decode()
        parents = ['-p', parent] if parent else []
        return git('commit-tree', tree, *parents, data=b'Update submodule\n').strip().decode()

    def source(oid):
        return [f'Subproject commit {oid}\n'.encode()] if oid else []

    for name, old, changed in (('modify', '1' * 40, ['2' * 40, '3' * 40]),
                               ('create', None, ['2' * 40, '3' * 40]),
                               ('delete', '1' * 40, [None, None])):
        base = commit(old)
        revisions = [commit(oid, base) for oid in changed]
        patches = [git('diff', '--binary', '--submodule=short', base, rev) for rev in revisions]
        sources = [{'module': [dict(enumerate(source(old))),
                               dict(enumerate(source(oid)))]} for oid in changed]
        for reverse in (False, True):
            order = [1, 0] if reverse else [0, 1]
            for width in (0, 3):
                output = subprocess.check_output(
                    [BINARY, '--color=never', f'-U{width}', f'--git-tree={repo}',
                     *(revisions[leg] for leg in order)], env=ENV)
                report = check_review([patches[leg] for leg in order], output,
                                      [sources[leg] for leg in order], context=width)
                delta = [Counter((row.sign, row.text) for row in side)
                         for side in report['edits']]
                wanted = [Counter(('+', line) for line in source(changed[leg]))
                          for leg in order]
                assert delta == wanted, (name, reverse, width, delta, wanted)
                CHECKS += 1


def directory_transition_cases():
    """A directory at a former file path contributes no file source there."""
    global CHECKS
    repo, git = create_git_store('directory-transitions')
    paths = ['node', 'node/item']

    def commit(directory, text, parent=None):
        blob = git('hash-object', '-w', '--stdin', data=text).strip().decode()
        if directory:
            child = git('mktree', data=f'100644 blob {blob}\titem\n'.encode()).strip().decode()
            entry = f'040000 tree {child}\tnode\n'
        else:
            entry = f'100644 blob {blob}\tnode\n'
        tree = git('mktree', data=entry.encode()).strip().decode()
        parents = ['-p', parent] if parent else []
        return git('commit-tree', tree, *parents, data=b'Change path type\n').strip().decode()

    before = b'head\nold\nend\n'
    changed = [b'head\nleft\nend\n', b'head\nright\nend\n']
    for old_directory in (False, True):
        base = commit(old_directory, before)
        for new_directories in ([not old_directory, old_directory],
                                [not old_directory, not old_directory]):
            revisions = [commit(directory, text, base)
                         for directory, text in zip(new_directories, changed)]
            patches = [git('diff', '--no-renames', base, rev) for rev in revisions]
            sources = [{path: [dict(enumerate(lines(before))) if i == old_directory else {},
                               dict(enumerate(lines(changed[leg]))) if i == directory else {}]
                        for i, path in enumerate(paths)}
                       for leg, directory in enumerate(new_directories)]
            for order in ([0, 1], [1, 0]):
                for width in (0, 3):
                    output = subprocess.check_output(
                        [BINARY, '--color=never', f'-U{width}', f'--git-tree={repo}',
                         *(revisions[leg] for leg in order)], env=ENV)
                    inputs = [patches[leg] for leg in order]
                    report = check_review(inputs, output, [sources[leg] for leg in order], context=width)
                    delta = [Counter((row.sign, row.text) for row in side) for side in report['edits']]
                    assert delta == expected(*inputs), (old_directory, new_directories, order, width, delta)
                    CHECKS += 1


def repeated_edit_cases():
    """Equal interior runs remain shared without hiding distinct edits."""
    suffix = b' xxxxxxxxxxxxxxxxxxxxxxxx\n'
    cases = {
        'native-result-p1-interior': [Counter({('-', b'extra\n'): 1, ('+', b'p1_extra\n'): 1}), Counter()],
        'fuzzy150b': [
            Counter({('-', b'q three' + suffix): 1, ('+', b'z sep1' + suffix): 1,
                     ('+', b'x same' + suffix): 1, ('+', b'y sep1' + suffix): 1}),
            Counter({('-', b'zz absent' + suffix): 1, ('+', b'y head' + suffix): 1,
                     ('+', b'y sep1' + suffix): 1, ('+', b'y sep2' + suffix): 1})]
    }
    for name, changes in cases.items():
        inputs = [(ROOT / 'tests' / name / f'patch{leg}').read_bytes() for leg in (1, 2)]
        for reverse in (False, True):
            for width in (0, 3):
                patches = inputs[::-1] if reverse else inputs
                wanted = changes[::-1] if reverse else changes
                run_case(f'original-{name}-{reverse}-U{width}', *patches, wanted, width=width)


def sparse_context_cases():
    cases = json.loads((ROOT / 'tests/context-quotes.json').read_text())
    for edge in ('head', 'tail'):
        for reverse in (False, True):
            for width in (3, 8):
                patches = [('--- a/f.c\n+++ b/f.c\n' + cases[key]).encode()
                           for key in ('broad', edge)]
                if reverse:
                    patches.reverse()
                run_case(f'quoted-{edge}-{reverse}-U{width}', *patches,
                         expected(*patches), width=width)

    for edge in ('head', 'tail'):
        patches = []
        for leg, word in enumerate(('left context', 'right context')):
            body = [' anchor', ' ' + word, '+shared', ' following', ' tail']
            if leg:
                body.insert(0, ' uncertain edge')
            if edge == 'tail':
                body.reverse()
            start = 10 + 10 * leg
            old = sum(line[0] != '+' for line in body)
            new = sum(line[0] != '-' for line in body)
            patches.append(('--- a/f.c\n+++ b/f.c\n' +
                            f'@@ -{start},{old} +{start},{new} @@\n' +
                            '\n'.join(body) + '\n').encode())
        for reverse in (False, True):
            inputs = patches[::-1] if reverse else patches
            for width in (0, 1, 3, 8):
                _, output, report = run_case(
                    f'outer-anchor-{edge}-{reverse}-U{width}', *inputs,
                    [Counter(), Counter()], width=width)
                wanted = [b'left context\n', b'right context\n']
                if reverse:
                    wanted.reverse()
                actual = [(row.leg, row.text) for row in report['rows']
                          if row.section == 'context' and row.sign in '+-']
                assert actual == list(enumerate(wanted)), 'a margin promoted an unanchored edge to a context difference'
                assert b'uncertain edge' not in output, 'context crossed its outer anchor into unknown source'


def group_members(report, reverse=False):
    groups = {}
    for row in report['rows']:
        groups.setdefault((row.section, row.paths, row.group), []).append(
            (row.leg ^ reverse, row.paths[row.leg], row.sign, row.pos,
             row.shared, row.source))
    return Counter(tuple(sorted(group)) for group in groups.values())


def unknown_pairing_cases():
    # Missing source between hunks cannot justify positional row pairing
    for count in (1, 4, 40):
        for gaps in (1, 2):
            a = (f'--- a/f.c\n+++ b/f.c\n@@ -1,{count + 1} +1,{count + 2} @@\n'
                 '-old anchor\n+anchor\n+left edit\n' +
                 ''.join(f' left context {i}\n' for i in range(count)) +
                 '@@ -100 +101 @@\n-old tail\n+tail\n')
            if gaps == 1:
                a = a.replace(f'@@ -1,{count + 1} +1,{count + 2} @@',
                              f'@@ -1,{count + 2} +1,{count + 3} @@')
                a = a.replace('@@ -100 +101 @@\n', '')
            b = ('--- a/f.c\n+++ b/f.c\n@@ -1 +1 @@\n-old anchor\n+anchor\n' +
                 f'@@ -{100 - count},{count + 1} +{100 - count},{count + 2} @@\n' +
                 '+right edit\n' +
                 ''.join(f' right context {i}\n' for i in range(count)) +
                 '-old tail\n+tail\n')
            patches = [a.encode(), b.encode()]
            for width in (0, 3, 80):
                reports = []
                groupings = []
                for reverse in (False, True):
                    inputs = patches[::-1] if reverse else patches
                    _, _, report = run_case(
                        f'unknown-pair-{count}-{gaps}-{width}-{reverse}',
                        *inputs, expected(*inputs), width=width)
                    reports.append(paired_cells(report['rows'], reverse=reverse))
                    pairs = {}
                    for row in report['rows']:
                        pairs.setdefault((row.section, row.pair), []).append(row)
                    groupings.append(group_members(report, reverse))
                    for rows in pairs.values():
                        if len(rows) == 2:
                            assert all(row.text in (b'anchor\n', b'tail\n',
                                                    b'old anchor\n', b'old tail\n')
                                       for row in rows), (
                                count, gaps, width, reverse,
                                'paired real rows across unknown source', rows)
                assert reports[0] == reports[1], 'unknown pairing depended on order'
                assert groupings[0] == groupings[1], 'unknown gaps changed group membership on reversal'

    # Shared removals anchor the quoted rows on either side of the gap
    a = (b'--- a/f.c\n+++ b/f.c\n@@ -10,2 +10,2 @@\n-old\n+left edit\n'
         b' left prefix\n@@ -100,3 +100,3 @@\n left excess\n left suffix\n'
         b'-old tail\n+left tail\n')
    b = (b'--- a/f.c\n+++ b/f.c\n@@ -10,2 +10,2 @@\n-old\n+right edit\n'
         b' right prefix\n@@ -101,2 +101,2 @@\n right suffix\n'
         b'-old tail\n+right tail\n')
    for reverse in (False, True):
        inputs = (b, a) if reverse else (a, b)
        _, _, report = run_case(f'unknown-anchored-{reverse}', *inputs,
                                 expected(*inputs), width=100)
        rows = {(row.leg, row.text): row.pair for row in report['rows']
                if row.section == 'delta'}
        for left, right in ((b'left edit\n', b'right edit\n'),
                            (b'left prefix\n', b'right prefix\n'),
                            (b'left suffix\n', b'right suffix\n')):
            assert rows[reverse, left] == rows[not reverse, right], (
                reverse, left, right, 'lost pairing beside a shared anchor')
        excess = rows[reverse, b'left excess\n']
        assert all(pair != excess for (leg, _), pair in rows.items()
                   if leg != reverse), 'excess prefix displaced the following anchor'

    # A later gap doesn't change the location of an anchored replacement
    a = (b'--- a/f.c\n+++ b/f.c\n@@ -1,4 +1,4 @@\n head\n'
         b'-old\n+left edit\n between\n-old tail\n+tail\n')
    b = (b'--- a/f.c\n+++ b/f.c\n@@ -1,2 +1,2 @@\n head\n'
         b'-old\n+right edit\n@@ -100 +100 @@\n-old tail\n+tail\n')
    for reverse in (False, True):
        inputs = (b, a) if reverse else (a, b)
        _, _, report = run_case(f'unknown-replacement-{reverse}', *inputs,
                                 expected(*inputs), width=100)
        rows = {row.text: row.pair for row in report['rows']
                if row.section == 'delta'}
        assert rows[b'left edit\n'] == rows[b'right edit\n'], (
            reverse, 'split replacements of the same quoted source')

    # Extra additions don't displace a similar replacement beside a shared removal
    a = a.replace(b'@@ -1,4 +1,4 @@', b'@@ -1,4 +1,5 @@')
    a = a.replace(b'+left edit\n', b'+new unrelated line\n+left_value\n')
    b = b.replace(b'+right edit\n', b'+left_valuz\n')
    for reverse in (False, True):
        inputs = (b, a) if reverse else (a, b)
        _, _, report = run_case(f'unknown-extra-replacement-{reverse}', *inputs,
                                 expected(*inputs), width=100)
        rows = {row.text: row.pair for row in report['rows']
                if row.section == 'delta'}
        assert rows[b'left_value\n'] == rows[b'left_valuz\n'], (
            reverse, 'extra addition displaced the similar replacement')
        assert rows[b'new unrelated line\n'] != rows[b'left_valuz\n']

    # Two isolated quotations have no common anchor for display pairing
    a = b'--- a/f.c\n+++ b/f.c\n@@ -10 +10 @@\n-left old\n+left new\n'
    b = b'--- a/f.c\n+++ b/f.c\n@@ -80 +80 @@\n-right old\n+right new\n'
    for reverse in (False, True):
        inputs = (b, a) if reverse else (a, b)
        _, _, report = run_case(f'unknown-islands-{reverse}', *inputs,
                                 expected(*inputs))
        counts = Counter((row.section, row.pair) for row in report['rows'])
        assert all(count == 1 for count in counts.values()), (
            reverse, 'paired isolated quotations without source evidence')

    # Distinct statements in distant hunks must keep their original locations
    body = ('{\n\tint ret;\n\n\tret = setup(d);\n\tif (ret)\n'
            '\t\treturn ret;\n\n\tret = start(d);\n\tif (ret)\n'
            '\t\treturn ret;\n\n\treturn 0;\n}\n\n')
    functions = [f'static int {name}(struct dev *d)\n' + body
                 for name in ('alpha', 'beta')]
    original = ''.join(functions)
    logged = [text.replace('\tret = start(d);\n',
                           '\tret = start(d);\n\tlog(d);\n')
              for text in functions]
    results = [
        logged[0].replace('\treturn 0;\n', '\treturn ret;\n') +
        functions[1].replace('\treturn 0;\n', '\tcleanup(d);\n\n\treturn 0;\n'),
        logged[0] + logged[1].replace('\treturn 0;\n', '\tcleanup(d);\n\treturn 0;\n'),
    ]
    boundaries = [text.splitlines().index('static int beta(struct dev *d)')
                  for text in [original, *results]]
    for quoted in (0, 1, 3, 8):
        patches = [''.join(difflib.unified_diff(
            original.splitlines(keepends=True), result.splitlines(keepends=True),
            fromfile='a/f.c', tofile='b/f.c', n=quoted)).encode()
                   for result in results]
        for width in (0, 3, 12):
            reports = []
            for reverse in (False, True):
                inputs = patches[::-1] if reverse else patches
                _, _, report = run_case(f'unknown-functions-{quoted}-{width}-{reverse}',
                                         *inputs, width=width)
                reports.append(paired_cells(report['rows'], reverse=reverse))
                pairs = {}
                for row in report['rows']:
                    leg = 1 - row.leg if reverse else row.leg
                    stage = leg + 1 if row.pos[1] >= 0 else 0
                    pos = row.pos[1] if stage else row.pos[0]
                    pairs.setdefault((row.section, row.pair), []).append(
                        (pos >= boundaries[stage], bool(row.text.strip())))

                # Sparse quotations cannot identify an otherwise equal blank
                assert all(len({scope for scope, _ in rows}) == 1 or
                           not any(nonblank for _, nonblank in rows)
                           for rows in pairs.values()), (
                    quoted, width, reverse, 'paired different function occurrences')
            assert reports[0] == reports[1], 'hunk pairing depended on order'


def file_pairing_cases():
    before = [b'head\n', b'old\n', b'tail\n']

    def edit(path, text):
        return patch(before, [before[0], text + b'\n', before[2]], path)

    def git_edit(path, text):
        return f'diff --git a/{path} b/{path}\n'.encode() + edit(path, text)

    def mode(path):
        return (f'diff --git a/{path} b/{path}\n'
                'old mode 100644\nnew mode 100755\n').encode()

    x = edit('p/x/foo.c', b'left x')
    y = edit('p/y/foo.c', b'left y')
    z = edit('q/r/y/foo.c', b'right y')
    exact = edit('y/lib/foo.c', b'exact')
    git = git_edit('lib/y.c', b'git')
    bare = (edit('lib/y.c', b'bare') + edit('q/lib/y.c', b'prefixed'))
    bare = bare.replace(b'--- a/', b'--- ').replace(b'+++ b/', b'+++ ')
    cases = [
        ('longest-suffix', x + y, z, (b'left y\n', b'right y\n')),
        ('longest-suffix-reordered', y + x, z, (b'left y\n', b'right y\n')),
        ('exact-before-suffix', edit('x/lib/foo.c', b'other') + exact,
         exact, None),
        ('exact-before-metadata-suffix', mode('x/lib/foo.c') +
         git_edit('y/lib/foo.c', b'exact'), git_edit('y/lib/foo.c', b'exact'), None),
        ('repeated-records', exact + edit('y/lib/foo.c', b'second'),
         exact, None),
        ('repeated-records-both', exact + edit('y/lib/foo.c', b'second'),
         exact + edit('y/lib/foo.c', b'changed'), (b'second\n', b'changed\n')),
        ('single-basename', edit('foo.c', b'left'),
         edit('src/foo.c', b'right'), (b'left\n', b'right\n')),
        ('metadata-losing-path', mode('p/x/foo.c') + git_edit('p/y/foo.c', b'left y'),
         git_edit('q/r/y/foo.c', b'right y'),
         (b'left y\n', b'right y\n')),
        ('full-name-before-stripped', git, bare, (b'git\n', b'bare\n')),
    ]
    for name, a, b, changed_pair in cases:
        reports = []
        for reverse in (False, True):
            inputs = (b, a) if reverse else (a, b)
            _, _, report = run_case(f'file-pair-{name}-{reverse}', *inputs,
                                     expected(*inputs))
            reports.append(paired_cells(report['rows'], reverse=reverse))
            if changed_pair:
                texts = changed_pair[::-1] if reverse else changed_pair
                rows = [next(row for row in report['rows']
                             if row.leg == leg and row.section == 'delta'
                             and row.text == texts[leg] and row.sign == '+')
                        for leg in (0, 1)]
                assert (rows[0].paths, rows[0].pair) == (
                    rows[1].paths, rows[1].pair), name
        assert reports[0] == reports[1], (name, 'operand order changed pairing')

    # An exact metadata path takes priority over another file's content
    a = mode('x/lib/foo.c') + git_edit('y/lib/foo.c', b'left y')
    b = git_edit('x/lib/foo.c', b'right x')
    inventories = [Counter((row.sign, row.text) for file in parse_patch(p)
                           for row in file.rows if row.sign in '+-')
                   for p in (a, b)]
    for pinned in (False, True):
        prefix = git_edit('z.c', b'pin') if pinned else b''
        reports = []
        for reverse in (False, True):
            inputs = (b, a) if reverse else (a, b)
            inputs = tuple(prefix + p for p in inputs)
            _, _, report = run_case(f'file-pair-metadata-exact-{pinned}-{reverse}',
                                     *inputs,
                                     inventories[::-1] if reverse else inventories)
            reports.append(paired_cells(report['rows'], reverse=reverse))
            rows = [row for row in report['rows'] if row.section == 'delta'
                    and row.sign == '+' and row.text == b'left y\n']
            assert len(rows) == 1 and all(
                row.leg == rows[0].leg for row in report['rows']
                if row.paths == rows[0].paths), (
                pinned, reverse, 'suffix content displaced exact metadata')
        assert reports[0] == reports[1], 'metadata path depended on order'

    # Neither basename candidate has evidence that distinguishes it
    a = edit('x/b/foo.c', b'left x') + edit('y/b/foo.c', b'left y')
    b = edit('z/b/foo.c', b'right')
    inventories = [Counter((row.sign, row.text) for file in parse_patch(p)
                           for row in file.rows if row.sign in '+-')
                   for p in (a, b)]
    reports = []
    for reverse in (False, True):
        inputs = (b, a) if reverse else (a, b)
        _, _, report = run_case(f'file-pair-tied-{reverse}', *inputs,
                                 inventories[::-1] if reverse else inventories)
        reports.append(paired_cells(report['rows'], reverse=reverse))
    assert reports[0] == reports[1], 'tied path candidates depended on order'


def metadata_pairing_cases():
    # Metadata for a path must leave its content counterpart available
    for name in ('rename-defer-content', 'mode-defer-depth0', 'mode-defer-depth1'):
        inputs = [(ROOT / 'tests' / name / p).read_bytes()
                  for p in ('patch1', 'patch2')]
        reports = []
        for reverse in (False, True):
            a, b = inputs[::-1] if reverse else inputs
            _, output, report = run_case(f'{name}-order-{reverse}', a, b,
                                         expected(a, b))
            edits = report['edits']
            assert [len(side) for side in edits] == [1, 1], name
            printed = [[row for row in report['rows'] if row.leg == leg
                        and row.section == 'delta' and row.sign in '+-'
                        and not row.shared] for leg in (0, 1)]
            left, right = printed[0][0], printed[1][0]
            assert (left.section, left.paths, left.pair) == (
                right.section, right.paths, right.pair), name
            assert left.sign == right.sign == '+', name
            reports.append(paired_cells(report['rows'], reverse=reverse))
            notes = column_text(output)
            assert b'Old mode:' in notes or b'Rename:' in notes, name
        assert reports[0] == reports[1], (name, 'operand order changed pairing')


def metadata_cases():
    mode = b'diff --git a/f b/f\nold mode 100644\nnew mode 100755\n'
    rename = (b'diff --git a/a b/c b/d b/e\nsimilarity index 100%\n'
              b'rename from a b/c\nrename to d b/e\n')
    copy = rename.replace(b'rename ', b'copy ')

    # A stray header cannot hide the following operation, and CRLF must parse
    # metadata using the same paths and record boundaries as its LF equivalent.
    for name, plain, alternate in (
            ('stray-mode', mode, b'--- stray\n' + mode),
            ('stray-rename', rename, b'--- stray\n' + rename),
            ('stray-copy', copy, b'--- stray\n' + copy),
            ('crlf-rename', rename, rename.replace(b'\n', b'\r\n')),
            ('crlf-copy', copy, copy.replace(b'\n', b'\r\n')),
            ('crlf-prefaced-rename', rename,
             b'Patch description\r\n\r\n' + rename.replace(b'\n', b'\r\n'))):
        for reverse in (False, True):
            inputs = [plain, alternate]
            if reverse:
                inputs.reverse()
            paths, output, _ = run_case(f'metadata-{name}-{reverse}', *inputs)
            assert not output, (name, output)
            assert not (paths[0].parent / 'stderr').read_bytes(), name

    directory = ROOT / 'tests/ident47-irrdelete'
    opaque, quoted = [(directory / name).read_bytes() for name in ('patch1', 'patch2')]
    for suffix, a, b in (('forward', opaque, quoted), ('reverse', quoted, opaque)):
        _, output, _ = run_case('whole-file-deletion-' + suffix, a, b,
                                [Counter(), Counter()])
        assert not output
    full_id = b'f0f23074642919bb50ab5b6e4ec489706127f061'
    run_case('whole-file-deletion-abbreviation', opaque,
             quoted.replace(b'f0f2307', full_id), [Counter(), Counter()])
    different = opaque.replace(b'f0f2307', b'abcdef0')
    _, output, _ = run_case('whole-file-deletion-different-source', different,
                            quoted, [Counter(), Counter({('-', b'l1\n'): 1,
                            ('-', b'l2\n'): 1, ('-', b'l3\n'): 1})])
    notes = column_text(output)
    assert b'This patch deletes' in notes and b'quoting its source' in notes
    different = quoted.replace(b'-l2\n', b'-another\n')
    run_case('whole-file-deletion-both-quoted', quoted, different,
             expected(quoted, different))
    names = (
        'rename-source-annotated', 'rename-source-mirror',
        'rename-source-depth1', 'rename-source-prefixed-depth1',
        'rename-source-hunked', 'copy-source-annotated',
        'source-literal-rename-positive',
        'rename-source-basename-negative', 'rename-source-crossblock-negative',
        'source-literal-rename-negative', 'source-literal-copy-negative')
    for name in names:
        inputs = [(ROOT / 'tests' / name / p).read_bytes() for p in ('patch1', 'patch2')]
        _, output, _ = run_case('metadata-' + name, *inputs)
        assert (b'The other patch ' in column_text(output)) == ('negative' not in name), name

    def binary(oid, before='1111', mode='100644'):
        return (f'diff --git a/image b/image\nindex {before}..{oid} {mode}\n'
                'Binary files a/image and b/image differ\n').encode()

    for name, a, b, visible in (
        ('same-binary', binary('2222'), binary('2222'), False),
        ('abbreviated-binary', binary('abcd'), binary('abcdef01'), False),
        ('abbreviated-sha256-binary', binary('abcd'), binary('abcd' * 16), False),
        ('abbreviated-sha256-binary-reverse', binary('abcd' * 16), binary('abcd'), False),
        ('binary-parent-differs', binary('2222'), binary('2222', '3333'), False),
        ('different-binary', binary('2222'), binary('3333'), True),
        ('binary-null-width', binary('0000'), binary('0' * 64), False),
        ('binary-null-versus-prefix', binary('0000'), binary('00001'), True),
        ('binary-hash-families', binary('a' * 40), binary('a' * 64), True)):
        _, output, _ = run_case(name, a, b)
        assert bool(output) == visible, name
        if visible:
            assert b'Result blob:' in column_text(output)

    # Both the omitted deletion source and the unequal binary data need notices
    header = (b'diff --git a/x.bin b/x.bin\ndeleted file mode 100644\n'
              b'old mode 100644\nnew mode 100755\n')
    binary_forms = [b'Binary files a/x.bin and /dev/null differ\n',
                    b'GIT binary patch\nliteral 0\nHcmV?d00001\n\n']
    for reverse in (False, True):
        inputs = [header + data for data in binary_forms[::(-1 if reverse else 1)]]
        _, output, _ = run_case(f'binary-deletion-notices-{reverse}', *inputs)
        notes = column_text(output)
        assert notes.count(b'This patch deletes the whole file') == 2
        assert notes.count(b'The quoted binary patch data differs.') == 1


def text_interface_cases():
    """Check the new options and the distinction between source and padding."""
    global CHECKS
    for role in ('addition', 'removal'):
        before = [b'head\n', b'tail\n']
        left = [before[0], b'\n', before[1]]
        right = [before[0], b'word\n', before[1]]
        inputs = [patch(before, x) if role == 'addition' else patch(x, before)
                  for x in (left, right)]
        paths, plain, report = run_case('blank-color-' + role, *inputs)
        colored = subprocess.check_output([BINARY, '--color=always', *paths], env=ENV)
        foreground = b'\x1b[32m' if role == 'addition' else b'\x1b[31m'
        cell = colored.split(foreground, 1)[1].split(b'\x1b[0m', 1)[0]
        assert cell.endswith('\u2502'.encode()) and len(cell.decode()) == 9, 'blank change colors beyond its gutter'
        assert re.sub(rb'\x1b\[[0-9;]*m', b'', colored) == plain
        labeled = subprocess.check_output([BINARY, '--backport-labels', *paths], env=ENV)
        assert b'Backport' in labeled and b'Upstream' in labeled
        labeled_report = check_review(inputs, labeled)
        assert labeled_report['rows'] == report['rows'], 'labels changed the comparison'
        CHECKS += 3
    for option in ('--max-column-width=0', '--max-column-width=1',
                   '--max-column-width=-1', '--max-column-width=abc',
                   '--max-column-width=2147483648', '--github-comment',
                   '--fuzz=0', '--fuzz'):
        failed = subprocess.run([BINARY, option, *paths], env=ENV, capture_output=True)
        assert failed.returncode != 0 and not failed.stdout, ('invalid option accepted', option)
        if option.startswith('--fuzz'):
            assert b'unrecognized option' in failed.stderr
        CHECKS += 1
    env_columns = dict(ENV, COLUMNS='30')
    assert subprocess.check_output([BINARY, *paths], env=env_columns) == plain, 'redirected report used terminal width'
    CHECKS += 1

    # The embedded plain report belongs to the HTML file, not its output terminal
    html = subprocess.check_output([BINARY, '--html', *paths], env=ENV)
    assert terminal(['--html', *paths], columns=40) == html
    CHECKS += 1


def hunk_header_cases():
    """A missing closing delimiter does not permit junk in a coordinate."""
    global CHECKS
    inputs = [patch([b'old\n'], [line]) for line in (b'left\n', b'right\n')]
    paths, _, _ = run_case('hunk-header-tail', *inputs)
    header = b'@@ -1 +1 @@'
    for tail in (b'@@ -1 +1', b'@@ -1 +1 @', b'@@ -1 +1 @@ heading'):
        paths[0].write_bytes(inputs[0].replace(header, tail))
        output = subprocess.check_output([BINARY, '--color=never', *paths], env=ENV)
        check_review(inputs, output)
        CHECKS += 1
    for malformed in (b'@@ -1 +1junk @@', b'@@ -1,1 +1,1junk @@',
                      b'@@ -1 +1,1  junk @@'):
        paths[0].write_bytes(inputs[0].replace(header, malformed))
        for order in (paths, paths[::-1]):
            result = subprocess.run([BINARY, *order], env=ENV, capture_output=True)
            assert result.returncode != 0 and b'malformed patch' in result.stderr, result.stderr
            CHECKS += 1

    # A stray file header cannot consume the next hunk's header during indexing
    hunk = b'@@ -10 +10 @@\n-old again\n+new again\n'
    for order in (paths, paths[::-1]):
        paths[0].write_bytes(inputs[0] + hunk)
        baseline = subprocess.check_output([BINARY, *order], env=ENV)
        paths[0].write_bytes(inputs[0] + b'--- a/other\n' + hunk + b'+++ b/other\n')
        output = subprocess.check_output([BINARY, *order], env=ENV)
        assert output == baseline, 'stray header changed the following hunk'
        CHECKS += 1

    # A later +++ line cannot rescue an unsupported context-diff header across
    # a hunk; validation must require adjacent file headers, as indexing does.
    for trailing in (b'', b'+++ b/other\n'):
        paths[0].write_bytes(inputs[0] + b'*** a/other\n--- a/other\n' + hunk + trailing)
        for order in (paths, paths[::-1]):
            result = subprocess.run([BINARY, *order], env=ENV, capture_output=True)
            assert result.returncode != 0 and not result.stdout, result.stdout
            assert b'malformed patch' in result.stderr and b'--- a/other' in result.stderr, result.stderr
            CHECKS += 1


def output_file_cases():
    """The destination changes neither report content nor input handling."""
    global CHECKS
    before = [b'head\n', b'old\n', b'tail\n']
    paths, _, _ = run_case('output-file',
                           patch(before, [before[0], b'left\n', before[-1]]),
                           patch(before, [before[0], b'right\n', before[-1]]))
    destination = paths[0].parent / 'saved report'
    for options in ([], ['--color=always'], ['--html']):
        expected = subprocess.check_output([BINARY, *options, *paths], env=ENV)
        for output in (['-o', destination], [f'--output={destination}']):
            result = subprocess.run([BINARY, *options, *output, *paths],
                                    env=ENV, capture_output=True, check=True)
            assert result.stdout == b'' and result.stderr == b''
            assert destination.read_bytes() == expected
            CHECKS += 1

        # A terminal on stdout must not color or narrow a report saved elsewhere
        assert terminal([*options, '-o', destination, *paths], columns=40) == b''
        assert destination.read_bytes() == expected
        CHECKS += 1

        destination.write_bytes(b'keep this file\n')
        actual = subprocess.check_output([BINARY, *options, '-o', destination,
                                          '-o', '-', *paths], env=ENV)
        assert actual == expected and destination.read_bytes() == b'keep this file\n'
        CHECKS += 1

    # Parse and read failures must leave an existing destination intact
    invalid = paths[0].parent / 'invalid.patch'
    invalid.write_bytes(b'not a patch\n')
    for options, inputs in product(
            ([], ['--html']),
            ([paths[0], invalid], [paths[0], invalid.with_suffix('.missing')])):
        result = subprocess.run([BINARY, *options, '-o', destination, *inputs],
                                env=ENV, capture_output=True)
        assert result.returncode != 0 and result.stdout == b''
        assert destination.read_bytes() == b'keep this file\n'
        CHECKS += 1

    missing = destination.parent / 'missing-directory' / 'report'
    result = subprocess.run([BINARY, '-o', missing, *paths], env=ENV, capture_output=True)
    assert result.returncode != 0 and os.fsencode(missing) in result.stderr
    assert result.stdout == b''
    CHECKS += 1

    if Path('/dev/full').exists():
        result = subprocess.run([BINARY, '-o', '/dev/full', *paths],
                                env=ENV, capture_output=True)
        assert result.returncode != 0 and b'write error on /dev/full' in result.stderr
        assert result.stdout == b''
        CHECKS += 1

    # Both inputs are consumed before a destination that aliases one is opened
    expected = subprocess.check_output([BINARY, *paths], env=ENV)
    subprocess.run([BINARY, '-o', paths[0], *paths], env=ENV, check=True,
                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert paths[0].read_bytes() == expected
    CHECKS += 1


def empty_output_cases():
    """Empty comparisons emit no bytes but still open their destination."""
    global CHECKS
    shared = patch([b'old\n'], [b'new\n'])
    paths, output, _ = run_case('empty-output', shared, shared,
                                [Counter(), Counter()])
    assert output == b''
    repo, git = create_git_store('empty-output-tree')
    tree = git('mktree', data=b'').strip().decode()
    root = git('commit-tree', tree, data=b'Empty root\n').strip().decode()
    empty = git('commit-tree', tree, '-p', root,
                data=b'No file changes\n').strip().decode()
    blob = git('hash-object', '-w', '--stdin', data=b'source\n').strip().decode()
    tree = git('mktree', data=f'100644 blob {blob}\tf.c\n'.encode()).strip().decode()
    changed = git('commit-tree', tree, '-p', root,
                  data=b'Add source\n').strip().decode()
    cases = (
        ('patch', paths),
        ('empty-tree', [f'--git-tree={repo}', root, empty]),
        ('identical-tree', [f'--git-tree={repo}', changed, changed]))
    for name, args in cases:
        destination = paths[0].parent / f'{name}-report'
        for options in ([], ['--html']):
            for output_args in ([], ['-o', '-']):
                result = subprocess.run([BINARY, *options, *output_args, *args],
                                        env=ENV, capture_output=True)
                assert (result.returncode, result.stdout, result.stderr) == (0, b'', b'')
                CHECKS += 1

            # A successful empty report must also replace a stale saved report
            for previous in (None, b'previous report\n'):
                if previous is None:
                    destination.unlink(missing_ok=True)
                else:
                    destination.write_bytes(previous)
                result = subprocess.run([BINARY, *options, '-o', destination, *args],
                                        env=ENV, capture_output=True)
                assert (result.returncode, result.stdout, result.stderr) == (0, b'', b'')
                assert destination.is_file() and destination.read_bytes() == b''
                CHECKS += 1

            missing = destination.parent / 'missing-directory' / name
            result = subprocess.run([BINARY, *options, '-o', missing, *args],
                                    env=ENV, capture_output=True)
            assert result.returncode == 1 and result.stdout == b''
            assert b'cannot open' in result.stderr and os.fsencode(missing) in result.stderr
            CHECKS += 1
            if Path('/dev/full').exists():
                result = subprocess.run([BINARY, *options, '-o', '/dev/full', *args],
                                        env=ENV, capture_output=True)
                assert (result.returncode, result.stdout, result.stderr) == (0, b'', b'')
                CHECKS += 1

    # Shared edits may leave context differences, while file actions need no rows
    context = [patch([label, b'old\n'], [label, b'new\n'])
               for label in (b'left context\n', b'right context\n')]
    modes = [b'diff --git a/f.c b/f.c\nold mode 100644\nnew mode ' + mode + b'\n'
             for mode in (b'100755', b'100600')]
    for section, inputs in (('context', context), ('delta', modes)):
        paths, output, _ = run_case(f'nonempty-{section}', *inputs)
        assert output
        result = subprocess.run([BINARY, '--html', *paths], env=ENV, capture_output=True)
        assert result.returncode == 0 and result.stderr == b''
        assert result.stdout.startswith(b'<!doctype html>')
        assert f'<section id="{section}">'.encode() in result.stdout
        CHECKS += 1


def output_failure_cases():
    """Failures must retain their full diagnostic and report lost output."""
    global CHECKS
    missing = WORK / ('a' * 180) / ('b' * 180) / 'missing.patch'
    result = subprocess.run([BINARY, missing, missing], env=ENV, capture_output=True)
    assert result.returncode != 0 and os.fsencode(missing) in result.stderr, result.stderr
    CHECKS += 1

    before = [b'old\n']
    paths, _, _ = run_case('full-output-pipe',
                           patch(before, [b'left\n']), patch(before, [b'right\n']))
    read_fd, write_fd = os.pipe()
    try:
        os.set_blocking(write_fd, False)
        while True:
            try:
                os.write(write_fd, b'x' * 4096)
            except BlockingIOError:
                break

        # Keep the reader open without draining it: EAGAIN is not a closed reader
        result = subprocess.run([BINARY, *paths], stdout=write_fd,
                                stderr=subprocess.PIPE, env=ENV, timeout=10)
        assert result.returncode != 0 and b'write error' in result.stderr, result.stderr
        CHECKS += 1
    finally:
        os.close(write_fd)
        os.close(read_fd)


def framing_cases():
    """Exercise the compact grid independently of metadata length and color."""
    global CHECKS
    before = [b'head\n', b'shared old\n', b'old\n', b'tail\n']
    after = [[before[0], b'shared new\n', word, before[-1]]
             for word in (b'left\n', b'right\n')]
    path = 'long_file_name_' * 12 + '.c'
    label = b'long_function(' + b'argument, ' * 20 + b'final)'
    inputs = [patch(before, side, path).replace(b'@@\n', b'@@ ' + label + b'\n')
              for side in after]
    paths, plain, report = run_case('source-only-width', *inputs, expected(*inputs))
    assert all(row.columns == 10 for row in report['rows']), 'metadata widened source columns'
    physical = plain.decode().splitlines()
    assert max(map(display_width, physical)) == 39
    assert not any(line.endswith(' ') for line in physical)
    assert any(line.startswith('*    DELTA DIFFERENCES') for line in physical)
    assert not any(line.startswith('*') and ' - ' in line for line in physical), 'narrow banner retained subtitle'
    assert any(row.shared and row.sign == '+' for row in report['rows'])
    assert any(row.shared and row.sign == '-' for row in report['rows'])
    colored = subprocess.check_output([BINARY, '--color=always', *paths], env=ENV)
    for line in colored.splitlines():
        if b'shared old' in line or b'shared new' in line:
            assert b'\x1b' not in line, 'shared edit has color'
    assert re.sub(rb'\x1b\[[0-9;]*m', b'', colored) == plain
    CHECKS += 4

    unavailable_locale = dict(ENV, LD_PRELOAD=str(ROOT / 'tests/support/no-locale.so'),
                              ASAN_OPTIONS='verify_asan_link_order=0')
    ascii_output = subprocess.check_output([BINARY, '--color=never', *paths], env=unavailable_locale)
    glyphs = str.maketrans({'\u2502': '|', '\u2503': '|', '\u2500': '-',
                          '\u2542': '+', '\u253c': '+', '\u2501': '-', '\u2550': '='})
    assert ascii_output == plain.decode().translate(glyphs).encode(), 'ASCII fallback changed the layout'
    check_review(inputs, ascii_output)
    check_report_titles(ascii_output, ['Patch 1: patch1', 'Patch 2: patch2'])
    CHECKS += 1

    sparse = [item.replace(b'@@ -1,4 +1,4 @@', b'@@ -1000,4 +1000,4 @@') for item in inputs]
    assert sparse != inputs
    _, output, report = run_case('four-digit-gutters', *sparse, expected(*sparse))
    assert any(line.startswith('  old  new\u2502') for line in output.decode().splitlines())
    assert max(map(display_width, output.decode().splitlines())) == 43
    check_report_titles(output, ['Patch 1: patch1', 'Patch 2: patch2'])
    CHECKS += 1

    before = [f'line {number}\n'.encode() for number in range(20)]
    after = [list(before), list(before)]
    for leg, word in enumerate((b'left', b'right')):
        for position in (1, 18):
            after[leg][position] = word + b'\n'
    inputs = [patch(before, side) for side in after]
    _, output, report = run_case('group-and-file-rules', *inputs, expected(*inputs))
    width = report['rows'][0].columns
    rule = ' ' * 8 + '\u2502' + '\u2500' * width + '\u2542' + '\u2500' * 8 + '\u253c' + '\u2500' * width
    physical = output.decode().splitlines()
    groups = len({row.group for row in report['rows']})
    files = sum(check_report_titles(
        output, ['Patch 1: patch1', 'Patch 2: patch2']).values())
    assert groups > files and physical.count(rule) == groups - files
    assert any('@@' in line for line in physical), 'missing empty-label headings'
    CHECKS += 1

    a = b'diff --git a/mode-only b/mode-only\nold mode 100644\nnew mode 100755\n'
    b = a.replace(b'100755', b'100600')
    _, output, _ = run_case('metadata-only-minimum-grid', a, b)
    assert max(map(display_width, output.decode().splitlines())) == 23
    assert b'New mode: 100755' in column_text(output)
    CHECKS += 1


def check_report_titles(output, labels):
    """Require one complete title block above provenance and file captions."""
    plain = re.sub(rb'\x1b\[[0-9;]*m', b'', output).decode()
    physical = plain.splitlines()
    assert physical and re.fullmatch(r'\u2501+|-+', physical[0]), 'missing top title border'
    rule = physical[0]
    assert len(rule) % 2 == 1, 'unequal title panes'
    rules = [i for i, line in enumerate(physical)
             if re.fullmatch(r'\u2501+|-+', line)]
    assert all(physical[i] == rule for i in rules), 'unequal report borders'
    width = (len(rule) - 1) // 2
    bar, middle = ('\u2502', '\u2503') if rule[0] == '\u2501' else ('|', '|')
    remaining = list(labels)
    titles = []
    while any(remaining):
        cells = []
        for leg in (0, 1):
            piece, remaining[leg] = split_columns(remaining[leg], width)
            piece = piece.rstrip(' ')
            padding = (width - display_width(piece)) // 2 if piece else 0
            cells.append(' ' * padding + piece)
        titles.append(cells[0] + ' ' * (width - display_width(cells[0])) +
                      middle + cells[1])
    end = len(titles) + 2
    assert physical[:end] == [rule, *titles, rule], (labels, width, physical[:end])
    assert physical[end] == '', 'title block lacks its following blank line'
    provenance = ' '.join('\n'.join(physical[end+1:]).split('\n\n', 1)[0].splitlines())
    assert provenance in (
        'Tree mode: comparisons use complete files before and after each commit.',
        'Patch mode: comparisons use only the source lines quoted in the patches.')
    assert len(rules) > 2, 'missing file headings'
    sections = Counter()
    section = None
    for i in range(end + 1, len(physical)):
        line = physical[i]
        if line.startswith('*') and 'DIFFERENCES' in line:
            section = 'delta' if 'DELTA DIFFERENCES' in line else 'context'
        if i not in rules:
            continue
        assert section is not None, 'file heading precedes its section'
        left, rest = split_columns(physical[i+1], width)
        assert rest.startswith(middle), 'file caption lacks its middle separator'
        pattern = (r'^ +old +new' if section == 'delta' else r'^ +new') + re.escape(bar)
        assert all(re.match(pattern, cell) for cell in (left, rest[1:])), 'file border does not lead directly to numbered paths'
        sections[section] += 1
    for row, count in Counter(titles).items():
        assert physical.count(row) == count, 'title repeated below its initial block'
    return sections


def open_title_operand(resources, path, kind, descriptor_directory):
    """Keep stream inputs alive until the report has consumed them."""
    if kind == 'file':
        return str(path), path.name, {}
    if kind == 'symlink':
        link = path.with_name(path.name + '.link')
        link.unlink(missing_ok=True)
        link.symlink_to(path)
        return str(link), link.name, {}
    if kind == 'stdin-pipe':
        return '-', None, {'input': path.read_bytes()}
    if kind in ('stdin-file', 'dev-stdin'):
        source = resources.enter_context(path.open('rb'))
        name = '-' if kind == 'stdin-file' else '/dev/stdin'
        return name, None, {'stdin': source}
    if kind == 'fifo':
        fifo = path.with_name(path.name + '.fifo')
        fifo.unlink(missing_ok=True)
        os.mkfifo(fifo)
        writer = subprocess.Popen([
            sys.executable, '-c',
            'import pathlib, sys; pathlib.Path(sys.argv[1]).write_bytes(pathlib.Path(sys.argv[2]).read_bytes())',
            str(fifo), str(path)], env=ENV)

        def finish_writer():
            if writer.poll() is None:
                writer.kill()
            writer.wait(timeout=10)

        resources.callback(finish_writer)
        return str(fifo), None, {}

    if kind == 'fd-pipe':
        descriptor, writer = os.pipe()
        resources.callback(os.close, descriptor)
        with os.fdopen(writer, 'wb') as output:
            output.write(path.read_bytes())
    else:
        descriptor = resources.enter_context(path.open('rb')).fileno()

    directory = descriptor_directory
    if kind == 'fd-directory-link':
        directory = path.parent / 'descriptors'
        directory.unlink(missing_ok=True)
        directory.symlink_to(descriptor_directory, target_is_directory=True)
    elif kind == 'fd-thread':
        directory = Path('/proc/thread-self/fd')
    elif kind == 'fd-parent':
        directory = Path(f'/proc/{os.getpid()}/fd')

    options = {'pass_fds': (descriptor,)}
    name = str(directory / str(descriptor))
    if kind == 'fd-cwd':
        name = str(descriptor)
        options['cwd'] = directory
    return name, None, options


def stream_title_cases():
    """Name real files, but leave stream and descriptor titles role-only."""
    global CHECKS
    inputs = [patch([b'head\n', b'old\n', b'tail\n'],
                    [b'head\n', word + b'\n', b'tail\n'])
              for word in (b'left', b'right')]
    directory = WORK / 'stream-titles'
    directory.mkdir(exist_ok=True)
    paths = [directory / '63', directory / 'right.patch']
    for path, data in zip(paths, inputs):
        path.write_bytes(data)

    descriptors = next((Path(name) for name in ('/dev/fd', '/proc/self/fd')
                        if Path(name).is_dir()), None)
    kinds = ['file', 'symlink', 'stdin-pipe', 'stdin-file', 'fifo']
    if Path('/dev/stdin').is_symlink():
        kinds.append('dev-stdin')
    if descriptors is not None:
        kinds += ['fd-pipe', 'fd-file', 'fd-directory-link', 'fd-cwd']
    if Path('/proc/thread-self/fd').is_dir():
        kinds.append('fd-thread')
    if Path(f'/proc/{os.getpid()}/fd').is_dir():
        kinds.append('fd-parent')
    scenarios = [(kind, 'file') for kind in kinds]
    if descriptors is not None:
        scenarios.append(('fd-pipe', 'fd-pipe'))

    for kinds, reverse, backport in product(scenarios, (False, True), (False, True)):
        order = (1, 0) if reverse else (0, 1)
        with ExitStack() as resources:
            operands = [open_title_operand(resources, paths[leg], kinds[leg], descriptors)
                        for leg in order]
            options = {'pass_fds': ()}
            for _, _, settings in operands:
                for key, value in settings.items():
                    if key == 'pass_fds':
                        options[key] += value
                    else:
                        assert key not in options
                        options[key] = value
            command = [BINARY, '--color=never', '--max-column-width=80']
            if backport:
                command.append('--backport-labels')
            result = subprocess.run(command + [operand[0] for operand in operands],
                                    capture_output=True, env=ENV, timeout=20, **options)
        assert result.returncode == 0 and not result.stderr, (kinds, result.stderr)
        roles = ['Backport patch', 'Upstream patch'] if backport else ['Patch 1', 'Patch 2']
        labels = [role + (': ' + operand[1] if operand[1] is not None else '')
                  for role, operand in zip(roles, operands)]
        check_report_titles(result.stdout, labels)
        check_review([inputs[leg] for leg in order], result.stdout, context=3)
        CHECKS += 1


def title_cases():
    """Keep one title block across sections, files, and one-sided entries."""
    global CHECKS
    source = b'source_' + b'x' * 72 + b'\n'
    inputs = [b''.join(patch(
        [b'head\n', b'background_' + word + b'\n', source, b'tail\n'],
        [b'head\n', b'background_' + word + b'\n', word + source, b'tail\n'], path)
        for path in ('f.c', 'g.c')) for word in (b'left_', b'right_')]
    metadata = [b''.join(
        f'diff --git a/{path} b/{path}\nold mode 100644\nnew mode {mode}\n'.encode()
        for path in ('f.c', 'g.c')) for mode in ('100755', '100600')]
    one_sided = patch([source], [b'only_' + source])
    shared = patch([b'old\n'], [b'new\n'], 'shared.c')
    cases = [
        ('operand-titles', inputs, Counter(delta=2, context=2)),
        ('metadata-titles', metadata, Counter(delta=2)),
        ('left-only-titles', [shared + one_sided, shared], Counter(delta=1)),
        ('right-only-titles', [shared, shared + one_sided], Counter(delta=1))
    ]
    name_pairs = [
        ['cafe\u0301-fix.patch', '\u4fee\u6b63.patch'],
        ['cafe\u0301-' + 'long-' * 18 + 'fix.patch', '\u4fee\u6b63-' * 25 + 'fix.patch']
    ]
    for name, inputs, sections in cases:
        paths, _, original = run_case(name, *inputs, expected(*inputs))
        for names, columns, backport in product(name_pairs, (8, 28, 80),
                                               (False, True)):
            paths = [path.with_name(name) for path, name in zip(paths, names)]
            for path, data in zip(paths, inputs):
                path.write_bytes(data)
            roles = ('Backport patch', 'Upstream patch') if backport else ('Patch 1', 'Patch 2')
            labels = [f'{role}: {name}' for role, name in zip(roles, names)]
            for color in ('never', 'always'):
                args = [f'--color={color}', f'--max-column-width={columns}']
                if backport:
                    args.append('--backport-labels')
                output = subprocess.check_output([BINARY, *args, *paths], env=ENV)
                report = check_review(inputs, output)
                assert paired_cells(report['rows']) == paired_cells(original['rows'])
                assert check_report_titles(output, labels) == sections
                plain = re.sub(rb'\x1b\[[0-9;]*m', b'', output).decode()
                if color == 'never':
                    assert b'\x1b' not in output
                    uncolored = plain
                else:
                    assert b'\x1b[1m' in output and plain == uncolored
                assert not any(line.endswith(' ') for line in plain.splitlines())
                assert all(row.columns <= columns for row in report['rows'])
                CHECKS += 1
    paths, output, _ = run_case('empty-titles', shared, shared,
                                [Counter(), Counter()])
    assert output == b'', 'empty report gained titles'
    assert subprocess.check_output([BINARY, '--color=always', *paths], env=ENV) == b''
    CHECKS += 1


def grouping_cases():
    """Added context must not move an insertion to a different source location."""
    first = (ROOT / 'tests/source-priority-shared-insertion/patch1').read_bytes()
    separate = (ROOT / 'tests/source-priority-shared-insertion/patch2').read_bytes()
    before = ([f'prefix{i}\n' for i in range(10)] +
              ['alpha\n', 'beta\n', 'gamma\n', 'gamma\n', 'delta\n', 'epsilon\n'] +
              [f'filler{i}\n' for i in range(20)] +
              ['zeta\n', 'eta\n', 'gamma\n', 'gamma\n', 'delta\n', 'theta\n'])
    after = before[:13] + ['ROW_A\n'] + before[13:39] + ['INSERTED_BY_P1\n'] + before[39:]
    joined = ''.join(difflib.unified_diff(before, after, fromfile='a/f', tofile='b/f', n=100)).encode()
    expected_rows = [
        [('+', (13, 13), b'INSERTED_BY_P1\n')],
        [('+', (13, 13), b'ROW_A\n'), ('+', (39, 40), b'INSERTED_BY_P1\n')]]
    for name, second in (('separate', separate), ('joined', joined)):
        for reverse in (False, True):
            inputs = [first, second][::(-1 if reverse else 1)]
            for width in (0, 3, 20):
                _, _, report = run_case(f'grouping-{name}-{reverse}-U{width}', *inputs, width=width)
                actual = [[(row.sign, row.pos, row.text) for row in side] for side in report['edits']]
                assert actual == expected_rows[::(-1 if reverse else 1)], ('hunk grouping moved an edit', name, actual)


def correspondence_cases():
    for case in json.loads((ROOT / 'tests/correspondence-quotes.json').read_text()):
        patches = [p.encode() for p in case['patches']]
        widths = (0, 3, 12) if 'required_edits' in case or 'expected_edits' in case else (3,)
        for width in widths:
            reports = [run_case(f"{case['name']}{suffix}-U{width}", *operands, width=width)[2]
                       for suffix, operands in (('-forward', patches), ('-reverse', patches[::-1]))]
            identities = [[Counter((row.sign, row.pos, row.text) for row in rows)
                           for rows in report['edits']] for report in reports]
            assert identities[0] == identities[1][::-1], case['name']
            edits = {(leg, row.sign, row.pos[row.sign == '+'])
                     for leg, rows in enumerate(reports[0]['edits']) for row in rows}
            assert set(map(tuple, case.get('required_edits', []))) <= edits, (case['name'], edits)
            if 'expected_edits' in case:
                assert edits == set(map(tuple, case['expected_edits'])), (case['name'], edits)


def unknown_island_cases():
    # Reversal cannot move an unpaired quotation across a report separator
    for name in ('crossed-pair', 'source-conflicting-positive',
                 'stretch-tail-172', 'partial-native-unreported-gap',
                 'fuzzy5', 'fuzzy158'):
        fixture = ROOT / 'tests' / name
        args = next(line[5:].split() for line in
                    (fixture / 'spec').read_text().splitlines()
                    if line.startswith('args:'))
        patches = [(fixture / path).read_bytes() for path in args[-2:]]
        for width in (0, 3, 12):
            reports = []
            for reverse in (False, True):
                inputs = patches[::-1] if reverse else patches
                _, _, report = run_case(
                    f'unknown-island-{name}-{width}-{reverse}', *inputs,
                    width=width)
                reports.append(group_members(report, reverse))
            assert reports[0] == reports[1], (name, width,
                                             'quotation changed groups')


def known_suffix_cases():
    # A quoted suffix pairs similar rows before falling back to its anchor
    a = (b'--- a/n.c\n+++ b/n.c\n@@ -20,5 +20,6 @@\n n_p1_a\n n_p1_b\n'
         b'-limit(old_cap);\n+limit(new_cap);\n+n_extra_line\n'
         b' n_shared_x\n n_shared_y\n')
    b = (b'--- a/n.c\n+++ b/n.c\n@@ -120,5 +120,5 @@\n n_p2_a\n n_p2_b\n'
         b'-\tlimit(old_cap);\n+\tlimit(new_cap);\n n_shared_x\n n_shared_y\n')
    for reverse in (False, True):
        inputs = (b, a) if reverse else (a, b)
        _, _, report = run_case(f'known-suffix-similar-{reverse}', *inputs,
                                 width=5)
        rows = {(row.leg, row.text): row.pair for row in report['rows']
                if row.section == 'delta'}
        for left, right in ((b'n_p1_a\n', b'n_p2_a\n'),
                            (b'n_p1_b\n', b'n_p2_b\n'),
                            (b'limit(old_cap);\n', b'\tlimit(old_cap);\n'),
                            (b'limit(new_cap);\n', b'\tlimit(new_cap);\n')):
            assert rows[reverse, left] == rows[not reverse, right], (
                reverse, left, right, 'suffix similarity lost to position')
        extra = rows[reverse, b'n_extra_line\n']
        assert all(pair != extra for (leg, _), pair in rows.items()
                   if leg != reverse), 'unmatched suffix row gained a partner'

    # Without similar rows, the suffix still attaches to its following anchor
    a = (b'--- a/f.c\n+++ b/f.c\n@@ -10,3 +10,3 @@\n left one\n left two\n'
         b'-shared old\n+left new\n')
    b = (b'--- a/f.c\n+++ b/f.c\n@@ -9,4 +9,4 @@\n right zero\n right one\n'
         b' right two\n-shared old\n+right new\n')
    for reverse in (False, True):
        inputs = (b, a) if reverse else (a, b)
        _, _, report = run_case(f'known-suffix-backward-{reverse}', *inputs,
                                 width=5)
        rows = {(row.leg, row.text): row.pair for row in report['rows']
                if row.section == 'delta'}
        for left, right in ((b'left one\n', b'right one\n'),
                            (b'left two\n', b'right two\n')):
            assert rows[reverse, left] == rows[not reverse, right], (
                reverse, left, right, 'suffix fallback left its anchor')
        zero = rows[not reverse, b'right zero\n']
        assert all(pair != zero for (leg, _), pair in rows.items()
                   if leg == reverse), 'excess suffix row gained a partner'


def corpus_cases():
    global CHECKS
    directory = WORK / 'corpus-assertions'
    if directory.exists():
        shutil.rmtree(directory)
    (directory / 'patches').mkdir(parents=True)
    case = dict(name='replacement', bp='left', up='right')
    for leg, value in (('bp', b'left\n'), ('up', b'right\n')):
        (directory / 'patches' / f'{leg}-{case[leg]}.patch').write_bytes(
            patch([b'old\n'], [value]))
    (directory / 'repro.json').write_text(json.dumps([case]))
    wanted = dict(case, edits=[[0, 'f.c', '+', 1], [1, 'f.c', '+', 1]])
    assertions = directory / 'locations.json'
    for name, edits in (('correct', wanted['edits']),
                        ('wrong-line', [[0, 'f.c', '+', 2], [1, 'f.c', '+', 1]]),
                        ('optional', None)):
        assertions.write_text(json.dumps([dict(wanted, edits=edits)]))
        output = directory / name
        command = [sys.executable, ROOT / 'tests/check-corpus.py', '--mode=patch',
                   '--reverse', '--jobs=1', '--binary', BINARY,
                   '--corpus', directory, '--output', output]
        if edits is not None:
            command += ['--expectations', assertions]
        result = subprocess.run(command, env=ENV, capture_output=True, timeout=60)
        summary = json.loads((output / 'summary.json').read_text())
        assert result.returncode == (1 if name == 'wrong-line' else 0), result.stderr
        assert summary['passed'] == (name != 'wrong-line'), summary
        assert json.loads((output / 'expectations.json').read_text()) == (
            [dict(wanted, edits=edits)] if edits is not None else [])
        CHECKS += 1

    # Paired coordinates alone do not verify excerpt signs or completeness
    check = runpy.run_path(str(ROOT / 'tests/check-corpus.py'))['check_expectations']
    rows = [PrintedRow('context', ('f.c', 'f.c'), 0, leg, ' ', b'anchor\n',
                       (4, 4), pair=0) for leg in (0, 1)]
    wanted = dict(case, edits=[], equal_context=[[['f.c', 'f.c'], 5, 5]],
                  context_rows=[[0, 'f.c', ' ', 5]],
                  context_exact=[dict(leg=0, path='f.c', sign=' ', between=[4, 6], lines=[5])])
    check(case, dict(rows=rows), 'tree', [wanted])
    corruptions = [
        [replace(rows[0], pos=(5, 5)), rows[1]],
        [rows[0], replace(rows[1], pair=1)],
        [replace(rows[0], sign='-'), rows[1]],
        rows + [replace(rows[0], pos=(5, 5), pair=1)],
        [replace(rows[0], paths=('other.c', 'f.c')), rows[1]],
    ]
    for broken in corruptions:
        try:
            check(case, dict(rows=broken), 'tree', [wanted])
        except AssertionError:
            CHECKS += 1
        else:
            raise AssertionError(('corpus assertion accepted corrupt context', broken))


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
    oracle_negative_cases()
    neutral_move_oracle_cases()
    context_margin_cases()
    repeated_edit_cases()
    wrapping_cases()
    compact_tab_gutter_cases()
    terminal_layout_cases()
    sparse_context_cases()
    unknown_pairing_cases()
    unknown_island_cases()
    known_suffix_cases()
    file_pairing_cases()
    metadata_pairing_cases()
    metadata_cases()
    correspondence_cases()
    grouping_cases()
    text_interface_cases()
    hunk_header_cases()
    output_file_cases()
    empty_output_cases()
    output_failure_cases()
    framing_cases()
    title_cases()
    stream_title_cases()
    tree_cases()
    gitlink_cases()
    directory_transition_cases()
    corpus_cases()
    CHECKS += check_reindentation_cases(BINARY, WORK, ENV)
    print(f'PASS {CHECKS} original-source, ownership, layout, and terminal checks ({BINARY.name})')


if __name__ == '__main__':
    main()
