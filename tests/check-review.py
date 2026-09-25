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
    context_margin_cases()
    repeated_edit_cases()
    wrapping_cases()
    compact_tab_gutter_cases()
    terminal_layout_cases()
    sparse_context_cases()
    text_interface_cases()
    CHECKS += check_reindentation_cases(BINARY, WORK, ENV)
    print(f'PASS {CHECKS} original-source, ownership, layout, and terminal checks ({BINARY.name})')

if __name__ == '__main__':
    main()
