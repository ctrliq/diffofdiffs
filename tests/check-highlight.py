#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check native ranges and renderer source preservation independently."""
import difflib
import fcntl
import os
import pty
import re
import struct
import subprocess
import sys
import tempfile
import termios
import tty
from contextlib import ExitStack
from html.parser import HTMLParser
from itertools import product, zip_longest
from pathlib import Path

from review_oracle import check_review, display_text


ENV = dict(os.environ, LC_ALL='C.UTF-8', ASAN_OPTIONS='allocator_may_return_null=1:detect_leaks=1')
ANSI = re.compile(rb'\x1b\[[0-9;]*m')
SPANS = {'word-change': 'words', 'character-change': 'characters',
         'indent-change': 'indentation', 'whitespace-tab': 'tabs',
         'whitespace-space': 'spaces'}
CHECKS = 0


class Report(HTMLParser):
    def __init__(self, data):
        super().__init__(convert_charrefs=True)
        self.stack = []
        self.cells = []
        self.current = None
        self.plain = ''
        self.scripts = 0
        self.root = None
        self.styles = []
        self.resources = []
        self.indents = []
        self.subjects = []
        self.identities = []
        self.title = ''
        self.feed(data)
        assert not self.stack

    def handle_starttag(self, tag, attributes):
        attrs = dict(attributes)
        classes = attrs.get('class', '').split()
        if tag in ('meta', 'input', 'br'):
            return

        if 'src' in attrs or 'href' in attrs:
            self.resources.append(attrs)
        if tag == 'html':
            assert self.root is None
            self.root = attrs
        if tag == 'style':
            self.styles.append('')
        if tag == 'script':
            self.scripts += 1
        if 'data-indent-shift' in attrs:
            self.indents.append(attrs['data-indent-shift'])
        if 'operand-subject' in classes:
            self.subjects.append('')
        if tag == 'div' and self.stack and 'operands' in self.stack[-1][1].get('class', '').split():
            self.identities.append('')
        if 'source-side' in classes:
            assert self.current is None
            self.current = dict(gap='gap' in classes, source='', sign='', old='', new='',
                                newline=attrs.get('data-newline'), depth=len(self.stack),
                                **{key: [] for key in SPANS.values()})
            self.cells.append(self.current)
        if self.current is not None:
            for name, key in SPANS.items():
                if name in classes:
                    self.current[key].append('')
        self.stack.append((tag, attrs))

    def handle_endtag(self, tag):
        opened, _ = self.stack.pop()
        assert opened == tag, (opened, tag)
        if self.current is not None and len(self.stack) == self.current['depth']:
            self.current = None

    def handle_data(self, data):
        if any(tag == 'style' for tag, _ in self.stack):
            self.styles[-1] += data
        if any(tag == 'title' for tag, _ in self.stack):
            self.title += data
        if any('operand-subject' in attrs.get('class', '').split() for _, attrs in self.stack):
            self.subjects[-1] += data
        if len(self.stack) > 1 and 'operands' in self.stack[-2][1].get('class', '').split():
            self.identities[-1] += data
        if any(attrs.get('id') == 'plain-report' for _, attrs in self.stack):
            self.plain += data
        if self.current is None:
            return

        classes = {name for _, attrs in self.stack for name in attrs.get('class', '').split()}
        for name, key in [('source-text', 'source'), ('sign', 'sign'), ('old', 'old'), ('new', 'new')]:
            if name in classes:
                self.current[key] += data
        for name, key in SPANS.items():
            if name in classes:
                self.current[key][-1] += data


def ranges(directory, left, right):
    global CHECKS
    paths = [directory / 'left', directory / 'right']
    for path, data in zip(paths, (left, right)):
        path.write_bytes(data)
    output = subprocess.check_output([DRIVER, *map(str, paths)], env=ENV, timeout=10).decode()
    result = {'words': [[], []], 'characters': [[], []]}
    displayed = [display_text(text) for text in (left, right)]
    for line in output.splitlines():
        name, *values = line.split()
        values = list(map(int, values))
        if name == 'indent':
            result['indent'] = values
        else:
            leg, start, end = values
            result[name][leg].append(displayed[leg][start:end].decode())
    CHECKS += 1
    return result


def native_cases(directory):
    cases = [
        ('hrtimer_init(timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);',
         'hrtimer_init(timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_SOFT);',
         [['HRTIMER_MODE_REL'], ['HRTIMER_MODE_REL_SOFT']], [[], ['_SOFT']]),
        ('nfs_commit_inode(inode, flags);', 'nfs_commit_inode(inode, &cinfo, flags);',
         [[], ['&cinfo, ']], [[], ['&cinfo, ']]),
        ('struct page *page;', 'struct page page;', [['*'], []], [['*'], []]),
        ('fn(old_count, old_ptr);', 'fn(new_count, new_ptr);',
         [['old_count', 'old_ptr'], ['new_count', 'new_ptr']], [['old', 'old'], ['new', 'new']]),
        ('call(café, δelta);', 'call(cafés, δelta2);',
         [['café', 'δelta'], ['cafés', 'δelta2']], [[], ['s', '2']]),
        ('call(文件, count);', 'call(文档, count);', [['文件'], ['文档']], [['件'], ['档']]),
        ('return value;', 'return value;  ', [[], ['  ']], [[], ['  ']]),
        ('call(left, right);', 'call(left,right);', [[' '], []], [[' '], []]),
        ('call(left,\tright);', 'call(left,      right);',
         [['\t'], ['      ']], [['\t'], ['      ']]),
        ('return value;\t', 'return value;   ',
         [['\t'], ['   ']], [['\t'], ['   ']]),
        ('\tcall(文件,\tcount);', '\tcall(文件,      count);',
         [['\t'], ['      ']], [['\t'], ['      ']]),
        ('abc123', 'xyz789', [[], []], [[], []]),
        ('', 'abc', [[], []], [[], []]),
        ('value;', 'value;', [[], []], [[], []]),
        ('x' * 9000, 'y' * 9000, [[], []], [[], []]),
    ]
    for left, right, words, characters in cases:
        result = ranges(directory, left.encode(), right.encode())
        assert result['words'] == words, (left[:80], result)
        assert result['characters'] == characters, (left[:80], result)
        assert result['indent'][:2] == [0, 0], (left[:80], result)
        reversed_result = ranges(directory, right.encode(), left.encode())
        assert reversed_result['words'] == words[::-1], (left[:80], reversed_result)
        assert reversed_result['characters'] == characters[::-1], (left[:80], reversed_result)

    for left, right, indent in [
        ('\tif (oom) {', '\t\tif (oom) {', [1, 1, 8, 16]),
        ('\treturn value;', '        return value;', [1, 1, 8, 8]),
        ('\treturn old_value;', '        return new_value;', [1, 0, 8, 8]),
        ('\t return value;', ' \treturn value;', [1, 1, 9, 8]),
        ('\t\treturn value;', '\t        return value;', [1, 1, 16, 16]),
        ('\treturn value;', '\t\treturn value; ', [1, 0, 8, 16]),
        ('\treturn "a b";', '\t\treturn "a  b";', [1, 0, 8, 16]),
        ('\tabc123', '  xyz789', [0, 0, 8, 2]),
        ('\treturn value;\n', '\t\treturn value;\n', [1, 1, 8, 16]),
        ('\treturn value;\n', '\t\treturn value;', [1, 0, 8, 16]),
        ('\treturn value;', '\t\treturn value;\n', [1, 0, 8, 16]),
        ('\t', '  ', [1, 0, 8, 2]),
        ('\t\n', '  \n', [1, 0, 8, 2]),
    ]:
        result = ranges(directory, left.encode(), right.encode())
        assert result['indent'] == indent, (left, right, result)


def patch(before, after, path='file.c'):
    output = ''.join(difflib.unified_diff(before, after, fromfile='a/' + path,
                                        tofile='b/' + path, n=20))
    return output.encode()


def renderer_cases(directory):
    global CHECKS
    before = ['void sample(void)\n', '{\n', '\told_value();\n', '}\n']
    sources = [
        ['void sample(void)\n', '{\n', '\tfn(old_count, old_ptr);\n',
         '\tcall(文件, count);\n', '\treturn value;\n', '\ttrailing();\n', '}\n'],
        ['void sample(void)\n', '{\n', '\tfn(new_count, new_ptr);\n',
         '\tcall(文档, count);\n', '\t\treturn value;\n', '\ttrailing();  \n', '}\n'],
    ]
    paths = [directory / 'left.patch', directory / 'right.patch']
    for path, after in zip(paths, sources):
        path.write_bytes(patch(before, after))
    arguments = [TOOL, *map(str, paths)]
    defaults = Report(subprocess.check_output([*arguments, '--html'], env=ENV).decode()).root
    assert defaults['data-theme'] == 'dark'
    assert defaults['data-highlight'] == 'words'
    CHECKS += 1
    for columns in (2, 23, 80):
        plain = subprocess.check_output([*arguments, '--color=never', f'--max-column-width={columns}'], env=ENV)
        for theme in ('dark', 'light'):
            for mode in ('words', 'characters', 'none'):
                options = [f'--theme={theme}', f'--highlight={mode}', f'--max-column-width={columns}']
                ansi = subprocess.check_output([*arguments, '--color=always', *options], env=ENV)
                assert ANSI.sub(b'', ansi) == plain, (columns, theme, mode)
                assert b'\x1b[32m' in ansi
                assert b'\x1b[38;' not in ansi and b'\x1b[48;' not in ansi
                assert not re.search(rb'\x1b\[(?:7|4[0-8]|10[0-7])m', ansi)
                for code in (b'\x1b[91m', b'\x1b[92m'):
                    assert (code in ansi) == (mode != 'none'), (columns, theme, mode, code)
                underlined = re.findall(rb'\x1b\[4m(.*?)\x1b\[(?:0|24)m', ansi, re.S)
                assert len(underlined) == ansi.count(b'\x1b[4m')
                assert all(span and not span.strip(b' ') for span in underlined), underlined
                assert not underlined, 'isolated trailing spaces gained underlines'
                if theme == 'light':
                    dark = subprocess.check_output(
                        [*arguments, '--color=always', *options, '--theme=dark'], env=ENV)
                    assert ansi == dark, 'HTML theme changed terminal colors'
                ansi.decode('utf-8')
                html = subprocess.check_output([*arguments, '--html', *options], env=ENV).decode()
                parsed = Report(html)
                attributes = {'theme': theme, 'highlight': mode, 'layout': 'review',
                              'shared': 'true', 'gaps': 'true', 'wrap': 'true',
                              'text-size': 'medium'}
                for key, value in attributes.items():
                    assert parsed.root.get(f'data-{key}') == value, (key, value)
                assert parsed.plain == plain.decode(), 'copy payload differs from plain output'
                assert not parsed.resources and parsed.scripts == 1
                assert parsed.styles
                css = ''.join(parsed.styles)
                urls = [url for _, url in re.findall(r'url\(\s*([\'"]?)(.*?)\1\s*\)',
                                                    css, re.DOTALL)]
                imports = re.findall(r'@import\s+[\'"]([^\'"]+)', css)
                assert all(url.startswith(('data:', '#')) for url in urls + imports)
                assert len(parsed.cells) % 2 == 0
                for side in (0, 1):
                    cells = [cell for cell in parsed.cells[side::2] if not cell['gap']]
                    for source in sources[side][2:-1]:
                        assert any(cell['source'] == source.rstrip('\n') for cell in cells), source
                    assert any(cell['words'] == ['old_count', 'old_ptr'] if not side else
                               cell['words'] == ['new_count', 'new_ptr'] for cell in cells)
                assert '8' in parsed.indents
                CHECKS += 1

    markup = '</script><script>globalThis.pwned=1</script>&"\x1b\r'
    for path, text in zip(paths, [markup + 'left', markup + 'right']):
        path.write_bytes(patch(before, [*before[:2], '\t' + text + '\n', before[-1]], 'a&"<b>.c'))
    html = subprocess.check_output([*arguments, '--html'], env=ENV).decode()
    parsed = Report(html)
    assert parsed.scripts == 1 and not parsed.resources
    assert b'\x1b' not in html.encode() and '\x00' not in html and '\r' not in html
    assert any('</script><script>globalThis.pwned=1</script>&"\\x1b\\x0d' in cell['source']
               for cell in parsed.cells)
    assert parsed.plain == subprocess.check_output([*arguments, '--color=never'], env=ENV).decode()
    assert '&lt;b&gt;' in html
    CHECKS += 1

    for path, word in zip(paths, ('left', 'right')):
        path.write_text('--- a/file.c\n+++ b/file.c\n@@ -1 +1 @@\n-old\n+' +
                        word + '\n\\ No newline at end of file\n')
    html = subprocess.check_output([*arguments, '--html'], env=ENV).decode()
    parsed = Report(html)
    assert any(cell['newline'] == 'no' and cell['source'] == 'left' for cell in parsed.cells)
    assert any(cell['newline'] == 'no' and cell['source'] == 'right' for cell in parsed.cells)
    assert 'No newline at end of file' in html
    CHECKS += 1

    for missing_newline in (0, 1):
        for side, path in enumerate(paths):
            text = '--- a/file.c\n+++ b/file.c\n@@ -1 +1 @@\n-old\n+'
            text += '\t' * (side + 1) + 'return value;\n'
            if side == missing_newline:
                text += '\\ No newline at end of file\n'
            path.write_text(text)
        html = subprocess.check_output([*arguments, '--html'], env=ENV).decode()
        parsed = Report(html)
        assert not parsed.indents, 'newline changes must not be labeled indentation only'
        assert any(cell['newline'] == 'no' for cell in parsed.cells)
        assert 'No newline at end of file' in html
        CHECKS += 1

    paths[0].write_bytes(patch(before, [*before[:2], '\n', '\tnew_value();\n', before[-1]]))
    paths[1].write_bytes(patch(before, [*before[:2], '\tnew_value();\n', before[-1]]))
    parsed = Report(subprocess.check_output([*arguments, '--html'], env=ENV).decode())
    assert any(cell['gap'] for cell in parsed.cells)
    assert any(not cell['gap'] and not cell['source'] and cell['new'] for cell in parsed.cells)
    CHECKS += 1

    result = subprocess.run([TOOL, '--html', str(paths[0]), str(paths[0])],
                            env=ENV, capture_output=True)
    assert (result.returncode, result.stdout, result.stderr) == (0, b'', b'')
    CHECKS += 1

    repository = directory / 'empty-tree'
    subprocess.run(['git', 'init', '-q', str(repository)], env=ENV, check=True)
    subprocess.run(['git', '-C', str(repository), '-c', 'user.name=Test Fixture',
                    '-c', 'user.email=test@example.com', '-c', 'core.hooksPath=/dev/null',
                    'commit', '--quiet', '--no-gpg-sign', '--allow-empty', '-m', 'Empty input'],
                   env=ENV, check=True)
    result = subprocess.run([TOOL, '--html', '--git-tree=' + str(repository),
                             'HEAD', 'HEAD'], env=ENV, capture_output=True)
    assert (result.returncode, result.stdout, result.stderr) == (0, b'', b'')
    CHECKS += 1

    for invalid in ('--highlight=typo', '--html=xml', '--theme=unknown'):
        result = subprocess.run([*arguments, invalid], env=ENV, capture_output=True)
        assert result.returncode and not result.stdout
        CHECKS += 1


def replacement_cases(directory):
    global CHECKS
    prefix = ['void sample(void)\n', '{\n', '\tlocal_bh_disable();\n']
    suffix = ['\tboundary();\n', '}\n']
    arguments = ['mpls_platform_label_seq', 'net->mpls.platform_label_seq']
    calls = [f'\twrite_seqcount_begin(&{argument});\n' for argument in arguments]
    disable = '\tpreempt_disable_nested();\n'
    enable = '\tpreempt_enable_nested();\n'
    empty = prefix + suffix
    before_extra = [prefix + [disable, calls[0]] + suffix, prefix + [calls[1]] + suffix]
    existing = prefix + ['\ttrace_namespace(net);\n', calls[1]] + suffix
    cases = [
        ('extra-before', [empty, empty], before_extra, True),
        ('extra-after', [empty, empty],
         [prefix + [calls[0], enable] + suffix, prefix + [calls[1]] + suffix], True),
        ('extra-both', [empty, empty],
         [prefix + [disable, calls[0], enable] + suffix, prefix + [calls[1]] + suffix], True),
        ('removed-call', before_extra, [empty, empty], True),
        ('existing-call', [empty, existing],
         [before_extra[0], prefix + ['\taudit_change();\n', *existing[len(prefix):]]], True),
        ('repeated-call', [empty, empty],
         [prefix + [disable, calls[0], calls[0]] + suffix, prefix + [calls[1]] + suffix], False),
        ('crossed-calls', [empty, empty],
         [prefix + [calls[0], calls[0].replace('_begin(', '_end(')] + suffix,
          prefix + [calls[1].replace('_begin(', '_end('), calls[1]] + suffix], False),
    ]
    paths = [directory / 'replacement-left.patch', directory / 'replacement-right.patch']
    for name, before, after, reliable in cases:
        patches = [patch(old, new) for old, new in zip(before, after)]
        for reverse in (False, True):
            order = [1, 0] if reverse else [0, 1]
            positional = {
                tuple(text.rstrip('\n') for text in pair)
                for pair in zip_longest(*(after[side][len(prefix):-len(suffix)]
                                          for side in order), fillvalue='')
            }
            for path, side in zip(paths, order):
                path.write_bytes(patches[side])
            for locale in ('C', 'C.UTF-8'):
                env = dict(ENV, LC_ALL=locale)
                command = [TOOL, *map(str, paths), '--max-column-width=80']
                plain = subprocess.check_output([*command, '--color=never'], env=env)
                check_review([patches[side] for side in order], plain)
                for mode in ('words', 'characters', 'none'):
                    html = subprocess.check_output(
                        [*command, '--html', f'--highlight={mode}'], env=env).decode()
                    report = Report(html)
                    assert report.plain == plain.decode()
                    pairs = list(zip(report.cells[::2], report.cells[1::2]))
                    paired_calls = [pair for pair in pairs
                                    if any('write_seqcount_begin(' in cell['source'] for cell in pair)]
                    assert paired_calls, name
                    for pair in paired_calls:
                        if not reliable:
                            assert tuple(cell['source'] for cell in pair) in positional, (name, pair)
                            continue

                        for cell, side in zip(pair, order):
                            assert cell['source'] == calls[side].rstrip('\n'), (name, pair)
                            assert cell['words'] == [arguments[side]], (name, cell)
                            assert cell['characters'], (name, cell)
                    CHECKS += 1


def check_whitespace_cells(parsed, before, after, order, offset):
    for side, index in enumerate(order):
        for cell in parsed.cells[side::2]:
            if cell['gap']:
                continue

            source = after[index] if cell['new'] else before
            position = int(cell['new'] or cell['old']) - offset
            expected = display_text(source[position].encode()).removesuffix(b'\n').decode()
            assert cell['source'] == expected, cell
            if 'return value;' in expected or 'aligned();' in expected:
                assert cell['indentation'] == ['\t' if index == 0 else ' ' * 8], cell
            assert all(tab == '\t' for tab in cell['tabs']), cell
            assert all(space == ' ' for space in cell['spaces']), cell


def check_whitespace_rendering(command, patches, before, after, order, offset):
    global CHECKS
    plain = subprocess.check_output([*command, '--color=never'], env=ENV)
    check_review([patches[side] for side in order], plain)
    if command[-1] == '--max-column-width=80':
        for source in after[order[1]]:
            assert '\u2502'.encode() + display_text(source.encode()) in plain, source
    for mode in ('words', 'characters', 'none'):
        ansi = subprocess.check_output(
            [*command, '--color=always', f'--highlight={mode}'], env=ENV)
        assert ANSI.sub(b'', ansi) == plain
        parsed = Report(subprocess.check_output(
            [*command, '--html', f'--highlight={mode}'], env=ENV).decode())
        assert parsed.plain.encode() == plain
        assert plain.count(b'\t') == sum(cell['source'].count('\t') for cell in parsed.cells)
        assert '0' in parsed.indents, 'equal-width tabs/spaces lost their indentation change'
        check_whitespace_cells(parsed, before, after, order, offset)
        CHECKS += 1


def whitespace_cases(directory):
    before = ['void whitespace(void)\n', '{\n', '\told_value();\n', '}\n']
    bodies = [
        ['\treturn value;\n', '\t\taligned();\n', ' \t mixed();\n',
         '\tfn(left,\tright);\n', '\ttrail();\t \n', '\tcall(文件,\tcount);\n',
         '\t\t\n', '\tcontrol(\x1b,\tvalue);\n', '\tcombining(e\u0301,\tvalue);\n'],
        ['        return value;\n', '\t        aligned();\n', '\t  mixed();\n',
         '\tfn(left,        right);\n', '\ttrail();         \n',
         '\tcall(文件,      count);\n', '                \n',
         '\tcontrol(\x1b,        value);\n', '\tcombining(e\u0301,        value);\n'],
    ]
    after = [before[:2] + body + before[-1:] for body in bodies]
    patches = [patch(before, source) for source in after]
    paths = [directory / 'whitespace-left.patch', directory / 'whitespace-right.patch']
    for offset in (1, 1000000):
        shifted = [re.sub(rb'(?m)^@@ -1,4 \+1,',
                          f'@@ -{offset},4 +{offset},'.encode(), data) for data in patches]
        for order in ((0, 1), (1, 0)):
            for path, side in zip(paths, order):
                path.write_bytes(shifted[side])
            for columns in (2, 8, 15, 16, 23, 80):
                command = [TOOL, *map(str, paths), f'--max-column-width={columns}']
                check_whitespace_rendering(command, shifted, before, after, order, offset)


def whitespace_blocks_cases(directory):
    global CHECKS
    cases = [
        ('\tcall();', '        call();', (0, 8)),
        ('\tcall();', '\t  call();', (0, 2)),
        ('alignxx\tcall();', 'alignxx call();', (0, 1)),
        ('call();\t', 'call();   ', (0, 3)),
        ('\t', '        ', (0, 8)),
        ('\t ', '\t  ', (0, 1)),
        ('call( a);', 'call(\ta);', (1, 0)),
        ('call(\t a);', 'call(\t  a);', (1, 2)),
        ('call(  a);', 'call(    a);', (2, 4)),
        ('  call();', '    call();', (0, 2)),
        ('        call();', '                call();', (0, 8)),
        ('return  value;', 'return    value;', (2, 4)),
        ('call(a,\told);', 'call(a,  new);', (0, 2)),
        ('call(\t a, old);', 'call(\t a, new);', (0, 0)),
        ('call(a,  b, \tc);', 'call(a,    b,    c);', (3, 8)),
        (' \tcall();', '  \tcall();', (0, 1)),
        ('\tcall(); ', '\tcall();  ', (1, 2)),
        ('\t\t *      1. read state', '\t\t * \t1. read state', (6, 1)),
        ('do {                                           \\', 'do {\t\t\t\t\t\t\\', (43, 0)),
        ('\t\t\t* comment', '\t\t\t * comment', (0, 1)),
    ]
    before = ['void blocks(void)\n', '{\n', '\told_value();\n', '}\n']
    paths = [directory / 'blocks-left.patch', directory / 'blocks-right.patch']
    for left, right, counts in cases:
        for order in ((0, 1), (1, 0)):
            sources = (left, right)
            for path, side in zip(paths, order):
                path.write_bytes(patch(before, [*before[:2], sources[side] + '\n', before[-1]]))
            html = subprocess.check_output([TOOL, '--html', *map(str, paths)], env=ENV).decode()
            parsed = Report(html)
            for leg, side in enumerate(order):
                cells = [cell for cell in parsed.cells[leg::2]
                         if cell['source'] == sources[side] and cell['sign'] == '+']
                assert cells, (sources, parsed.cells)
                assert len(cells[0]['spaces']) == counts[side], (sources, cells[0])
                assert all(space == ' ' for space in cells[0]['spaces'])
            CHECKS += 1


def commit_subject_cases(directory):
    global CHECKS
    repository = directory / 'subjects'
    subprocess.run(['git', 'init', '-q', str(repository)], env=ENV, check=True)
    git = ['git', '-C', str(repository), '-c', 'user.name=Test Fixture',
           '-c', 'user.email=test@example.com', '-c', 'core.hooksPath=/dev/null']
    messages = ['First <script> & "quoted" café\ncontinued title\n\nPrivate body text.',
                'Second subject\n\nAnother body that stays out of the header.']
    commits, subjects = [], []
    for index, message in enumerate(messages):
        (repository / 'file.c').write_text(f'int value = {index};\n')
        subprocess.run([*git, 'add', 'file.c'], env=ENV, check=True)
        subprocess.run([*git, 'commit', '--quiet', '--no-gpg-sign', '-m', message], env=ENV, check=True)
        commits.append(subprocess.check_output([*git, 'rev-parse', 'HEAD'], env=ENV).decode().strip())
        subjects.append(subprocess.check_output([*git, 'show', '-s', '--format=%s', 'HEAD'],
                                                env=ENV).decode().strip())
    subprocess.run([*git, 'tag', '-a', '--no-sign', '-m', 'Annotation is not a subject',
                    'first-tag', commits[0]], env=ENV, check=True)
    revisions = ['first-tag', '..'.join(commits)]
    for order in ((0, 1), (1, 0)):
        html = subprocess.check_output([TOOL, '--html', '--git-tree=' + str(repository),
                                        *[revisions[side] for side in order]], env=ENV).decode()
        parsed = Report(html)
        assert parsed.subjects == [subjects[side] for side in order], parsed.subjects
        assert parsed.title == ' / '.join(parsed.subjects) + ' | diffofdiffs'
        assert parsed.scripts == 1 and not parsed.resources
        assert all(message.split('\n\n')[1] not in html for message in messages)
        assert 'Annotation is not a subject' not in html
        CHECKS += 1

    for value in (2, 3):
        (repository / 'file.c').write_text(f'int value = {value};\n')
        subprocess.run([*git, 'add', 'file.c'], env=ENV, check=True)
        subprocess.run([*git, 'commit', '--quiet', '--no-gpg-sign',
                        '--allow-empty-message', '-m', ''], env=ENV, check=True)
        commits.append(subprocess.check_output([*git, 'rev-parse', 'HEAD'],
                                               env=ENV).decode().strip())
    html = subprocess.check_output([TOOL, '--html', '--git-tree=' + str(repository),
                                    *commits[-2:]], env=ENV).decode()
    parsed = Report(html)
    assert parsed.subjects == ['(no commit subject)', '(no commit subject)']
    assert parsed.title == '(no commit subject) | diffofdiffs'
    CHECKS += 1


def terminal_whitespace_cases(directory):
    global CHECKS
    cases = [
        ('\tcall("PUT last-nack");', '\tcall("PUT oob      ");', False),
        ('\tcall("PUT oob");', '\tcall("PUT oob      ");', False),
        ('return value;', '    return value;', False),
        ('\t\treturn value;', '        return value;', False),
        ('\treturn value;', '\t    return value;', True),
        ('\tcall(\tvalue);', '\tcall(\t  value);', True),
        ('\tcall(    value);', '\tcall(\tvalue);', False),
        ('\tcall();\t', '\tcall();\t  ', True),
    ]
    before = ['void sample(void)\n', '{\n', '\told_value();\n', '}\n']
    paths = [directory / 'space-left.patch', directory / 'space-right.patch']
    for left, right, visible in cases:
        for path, line in zip(paths, (left, right)):
            after = [*before[:2], line + '\n', before[-1]]
            path.write_bytes(patch(before, after))
        for order in (paths, paths[::-1]):
            for columns in (8, 23, 80):
                arguments = [TOOL, *map(str, order), f'--max-column-width={columns}']
                plain = subprocess.check_output([*arguments, '--color=never'], env=ENV)
                for mode in ('words', 'characters', 'none'):
                    ansi = subprocess.check_output(
                        [*arguments, '--color=always', f'--highlight={mode}'], env=ENV)
                    assert ANSI.sub(b'', ansi) == plain
                    spans = re.findall(rb'\x1b\[4m(.*?)\x1b\[24m', ansi, re.S)
                    assert bool(spans) == (visible and mode != 'none'), (left, right, mode)
                    assert all(span and not span.strip(b' ') for span in spans), spans

                    # Tabs may precede painted spaces, but never change their color
                    cyan = re.findall(rb'\x1b\[36m\t*\x1b\[4m', ansi)
                    assert len(cyan) == len(spans), (left, right, mode)
                    assert b'\x1b[1m\x1b[4m' not in ansi
                    CHECKS += 1


def terminal_output(arguments):
    master, slave = pty.openpty()
    tty.setraw(slave)
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 24, 400, 0, 0))
    process = subprocess.Popen(arguments, stdout=slave, stderr=subprocess.PIPE, env=ENV)
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
    _, error = process.communicate(timeout=10)
    assert process.returncode == 0, error
    return b''.join(chunks)


def terminal_tab_cases(directory):
    """Tab expansion must preserve source-based ranges and whitespace neighbors."""
    global CHECKS
    before = ['old_value();\n']
    paths = [directory / 'tty-left.patch', directory / 'tty-right.patch']
    sources = ['\t\ufffd\x1b\tcall(界é\u0301,\t' + word + ');\n'
               for word in ('old_count', 'new_count')]
    for path, source in zip(paths, sources):
        path.write_bytes(patch(before, [source]).replace('\ufffd'.encode(), b'\xff'))
    for tab_width in (1, 4, 8, 16):
        for columns in (2, 7, 80):
            args = [TOOL, *map(str, paths), f'--tab-width={tab_width}',
                    f'--max-column-width={columns}']
            plain = terminal_output([*args, '--color=never'])
            assert b'\t' not in plain
            for mode in ('words', 'characters', 'none'):
                ansi = terminal_output([*args, '--color=always', f'--highlight={mode}'])
                assert ANSI.sub(b'', ansi) == plain
                for color, word in ((91, b'old'), (92, b'new')):
                    fragments = re.findall(b'\x1b\\[' + str(color).encode() +
                                           rb'm(.*?)\x1b\[0m', ansi, re.S)
                    wanted = word + b'_count' if mode == 'words' else word
                    assert b''.join(fragments) == (b'' if mode == 'none' else wanted), (
                        tab_width, columns, mode, color, fragments)
                CHECKS += 1

    for left, right, visible in (
            ('return value;', '    return value;', False),
            ('\tcall("PUT oob");', '\tcall("PUT oob      ");', False),
            ('\treturn value;', '\t    return value;', True),
            ('\tcall(\tvalue);', '\tcall(\t  value);', True),
            ('\tcall(    value);', '\tcall(\tvalue);', False),
            ('\tcall();\t', '\tcall();\t  ', True)):
        for path, source in zip(paths, (left, right)):
            path.write_bytes(patch(before, [source + '\n']))
        for tab_width in (1, 4, 8, 16):
            for columns in (2, 23):
                args = [TOOL, *map(str, paths), f'--tab-width={tab_width}',
                        f'--max-column-width={columns}', '--color=always']
                for mode in ('words', 'characters', 'none'):
                    ansi = terminal_output([*args, f'--highlight={mode}'])
                    spans = re.findall(rb'\x1b\[4m(.*?)\x1b\[24m', ansi, re.S)
                    assert bool(spans) == (visible and mode != 'none'), (
                        left, right, tab_width, columns, mode)
                    assert all(span and not span.strip(b' ') for span in spans)
                    assert len(re.findall(rb'\x1b\[36m\x1b\[4m', ansi)) == len(spans)
                    assert b'\x1b[1m\x1b[4m' not in ansi and b'\t' not in ansi
                    CHECKS += 1


def html_tab_cases(directory):
    """Visual tab settings must leave source bytes and edit ownership intact."""
    global CHECKS
    before = ['old_value();\n']
    sources = ['\treturn value;\n', '        return value;\n']
    paths = [directory / 'html-left.patch', directory / 'html-right.patch']
    for path, source in zip(paths, sources):
        path.write_bytes(patch(before, [source]))
    args = [TOOL, *map(str, paths)]
    plain = subprocess.check_output([*args, '--color=never'], env=ENV)
    check_review([path.read_bytes() for path in paths], plain)
    cells = None
    for tab_width in (1, 4, 8, 16):
        html = subprocess.check_output([*args, '--html', f'--tab-width={tab_width}'],
                                       env=ENV).decode()
        parsed = Report(html)
        assert f'--tab-size: {tab_width};' in html
        width = max(len(source.rstrip('\n').expandtabs(tab_width))
                    for source in [*before, *sources])
        assert f'--report-width: {2 * (width + 14)}ch;' in html
        assert set(parsed.indents) == {str(8 - tab_width)}, parsed.indents
        assert parsed.plain.encode() == plain
        for side, source in enumerate(sources):
            assert any(cell['source'] == source.rstrip('\n') and cell['sign'] == '+'
                       for cell in parsed.cells[side::2])
        if cells is not None:
            assert parsed.cells == cells
        cells = parsed.cells
        CHECKS += 1


def html_operand_title_cases(directory):
    """Use the same file-or-stream identities in HTML and its plain report."""
    global CHECKS
    paths = [directory / '63', directory / 'right<&>.patch']
    context = '// ' + 'context ' * 10 + '\n'
    inputs = [patch([context, '\treturn old;\n'], [context, '\treturn ' + word + ';\n'])
              for word in ('left', 'right')]
    for path, data in zip(paths, inputs):
        path.write_bytes(data)
    descriptors = next((Path(name) for name in ('/dev/fd', '/proc/self/fd')
                        if Path(name).is_dir()), None)
    methods = ['file', 'stdin']
    if descriptors is not None:
        methods += ['fd-file', 'fd-pipe']
    for method, reverse, backport in product(methods, (False, True), (False, True)):
        order = (1, 0) if reverse else (0, 1)
        labels = [path.name for path in paths]
        with ExitStack() as resources:
            arguments = list(map(str, paths))
            options = {'pass_fds': ()}
            for leg in (0, 1):
                if method == 'stdin' and leg == 0:
                    arguments[leg] = '-'
                    labels[leg] = None
                    options['input'] = inputs[leg]
                elif method.startswith('fd-'):
                    if method == 'fd-pipe':
                        descriptor, writer = os.pipe()
                        resources.callback(os.close, descriptor)
                        with os.fdopen(writer, 'wb') as output:
                            output.write(inputs[leg])
                    else:
                        descriptor = resources.enter_context(paths[leg].open('rb')).fileno()
                    arguments[leg] = str(descriptors / str(descriptor))
                    labels[leg] = None
                    options['pass_fds'] += (descriptor,)
            command = [TOOL, '--html']
            if backport:
                command.append('--backport-labels')
            result = subprocess.run(command + [arguments[leg] for leg in order],
                                    capture_output=True, env=ENV, timeout=20, **options)
        assert result.returncode == 0 and not result.stderr, (method, result.stderr)
        report = Report(result.stdout.decode())
        roles = ['Backport patch', 'Upstream patch'] if backport else ['Patch 1', 'Patch 2']
        titles = [
            role + (': ' + labels[leg] if labels[leg] is not None else '')
            for role, leg in zip(roles, order)]
        assert report.identities == titles
        assert all(report.plain.count(title) == 1 for title in titles)
        check_review([inputs[leg] for leg in order], report.plain.encode())
        assert [cell['source'] for cell in report.cells if cell['sign'] == '+'] == [
            '\treturn ' + ('left' if leg == 0 else 'right') + ';' for leg in order]
        CHECKS += 1


def display_character_cases(directory):
    global CHECKS
    cases = [
        (b'A', 1, 1),
        ('é'.encode(), 2, 1),
        ('界'.encode(), 3, 2),
        ('\u0301'.encode(), 2, 0),
        (b'\t', 1, -1),
        (b'\x07', 1, -1),
        (b'\xff', 0, -1),
        (b'\xe2\x82', 0, -1),
        (b'\0', 0, -1),
        (b'', 0, -1),
    ]
    path = directory / 'character'
    for source, byte_count, columns in cases:
        path.write_bytes(source)
        output = subprocess.check_output([DRIVER, '--display', str(path)], env=ENV)
        assert tuple(map(int, output.split())) == (byte_count, columns, byte_count)
        CHECKS += 1


def main():
    global TOOL, DRIVER
    TOOL, DRIVER = map(lambda value: str(Path(value).resolve()), sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix='dod-highlight-') as temporary:
        directory = Path(temporary)
        display_character_cases(directory)
        native_cases(directory)
        renderer_cases(directory)
        replacement_cases(directory)
        whitespace_cases(directory)
        whitespace_blocks_cases(directory)
        commit_subject_cases(directory)
        terminal_whitespace_cases(directory)
        terminal_tab_cases(directory)
        html_tab_cases(directory)
        html_operand_title_cases(directory)
    print(f'check-highlight: {CHECKS} native range and renderer checks passed')


if __name__ == '__main__':
    main()
