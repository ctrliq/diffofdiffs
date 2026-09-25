# SPDX-License-Identifier: GPL-2.0-only
"""Check indentation correspondence against independently chosen edit sites."""

from collections import defaultdict
import difflib
import os
from pathlib import Path
import shutil
import subprocess

from review_oracle import check_review, lines, paired_cells, parse_patch


ROOT = Path(__file__).resolve().parents[1]


def nested_case(name, indentation='\t', path='f.c', tail=''):
    before = [
        ['void receive(void)', '{', '\tif (ready)', '\t\toriginal();',
         '\tfinish();', '}'],
        ['void receive(void)', '{', '\twhile (more()) {', '\t\tif (ready)',
         '\t\t\toriginal();', '\t\trelease();', '\t}', '\tfinish();', '}'],
    ]
    block = ['\tif (ready) {', '\t\tcopy = prepare();', '', '\t\tif (copy) {',
             '\t\t\tuse_copy();', '\t\t} else {', '\t\t\tno_copy();',
             '\t\t}', '\t} else {', '\t\toriginal();', '\t}']
    right = ['\t' + row if row else row for row in block]
    right.insert(6, '\t\t\t\t/* The allocation failed */')
    after = [before[0][:2] + block + before[0][4:],
             before[1][:3] + right + before[1][5:]]
    text = lambda rows: ('\n'.join(rows) + '\n').replace('\t', indentation) + tail
    return dict(name=name, before=list(map(text, before)), after=list(map(text, after)),
                path=path, shared={(0, '+', 4), (1, '+', 5)},
                pairs={(2, 3), (3, 4), (5, 6), (6, 7), (7, 8), (8, 10),
                       (9, 11), (10, 12), (12, 14)})


def bounded_case(name, blocks, anchor=False):
    before, after = [], []
    for leg, side in enumerate(('left', 'right')):
        head, tail = f'{side}_head();\n', f'{side}_tail();\n'
        fixed = 'anchor();\n' if anchor else ''
        before.append(head + fixed + tail)
        after.append(head + (fixed if leg else '') + blocks[leg] +
                     (fixed if not leg else '') + tail)
    return dict(name=name, before=before, after=after, path='f', shared=set())


def partial_context_case():
    before, after = [], []
    for leg, side in enumerate(('left', 'right')):
        indent = '\t' * (leg + 1)
        prefix = [f'{side}_prefix{i};\n' for i in range(10)]
        suffix = [f'{side}_suffix{i};\n' for i in range(10)]
        block = [indent + row + '\n' for row in (
            f'{side}_edge();', 'first();', 'old_call();', 'last();',
            f'{side}_tail();')]
        before.append(''.join(prefix + block + suffix))
        block[2] = indent + 'new_call();\n'
        after.append(''.join(prefix + block + suffix))
    return dict(name='known-context-edges', before=before, after=after,
                path='f', shared=set(),
                context_rows={(leg, pos) for leg in (0, 1) for pos in (11, 12, 13)})


def source_cases():
    cases = [nested_case('nested-tabs'), nested_case('nested-spaces', '  '),
             nested_case('nested-non-c', path='f'),
             nested_case('unterminated-blank', path='f', tail=' \t'),
             partial_context_case()]
    cases.append(bounded_case('one-unique-word', [
        '\tunique();\n\n\t{\n\t}\n',
        '\t\tunique();\n\n\t\t{\n\t\t}\n']))
    cases.append(bounded_case('repeated-words', [
        '\trepeat();\n\trepeat();\n\n\t{\n\t}\n',
        '\t\trepeat();\n\t\trepeat();\n\n\t\t{\n\t\t}\n']))
    cases.append(bounded_case('inconsistent-shift', [
        '\tfirst();\n\n\t{\n\tsecond();\n\t}\n',
        '\t\tfirst();\n\n\t\t{\n\t\t\tsecond();\n\t\t\t}\n']))
    cases.append(bounded_case('exact-anchor-boundary', [
        '\tfirst();\n\n\tsecond();\n',
        '\t\tfirst();\n\n\t\tsecond();\n'], anchor=True))
    return cases


def native_edits(patches):
    return {(leg, row.sign, row.pos[row.sign == '+'])
            for leg, patch in enumerate(patches) for file in parse_patch(patch)
            for row in file.rows if row.sign in '+-'}


def check_reports(binary, directory, env, patches, expected, sources=None,
                  wanted_pairs=(), wanted_context=None):
    count = 0
    for mode in (('tree', 'patch') if sources else ('patch',)):
        for width in (0, 3, 12):
            forward = None
            for reverse in (False, True):
                order = [1, 0] if reverse else [0, 1]
                args = ([f'--git-tree={directory}/repo',
                         *[('bp', 'up')[i] for i in order]] if mode == 'tree' else
                        [str(directory / f'patch{i + 1}') for i in order])
                result = subprocess.run([binary, '--color=never', f'-U{width}',
                                         '--max-column-width=120', *args],
                                        env=env, capture_output=True, timeout=20)
                stem = f'{mode}-U{width}' + ('-reverse' if reverse else '')
                (directory / f'{stem}.txt').write_bytes(result.stdout)
                assert result.returncode == 0 and not result.stderr, (
                    directory.name, stem, result.stderr)
                report = check_review([patches[i] for i in order], result.stdout,
                                      [sources[i] for i in order] if mode == 'tree' else None,
                                      context=width)
                edits = {(order[r.leg], r.sign, r.pos[r.sign == '+'])
                         for r in report['rows'] if r.section == 'delta' and
                         r.sign in '+-' and not r.shared}
                assert edits == expected, (directory.name, stem,
                                           'missing', expected - edits,
                                           'extra', edits - expected)
                if mode == 'patch' and wanted_context is not None:
                    context = {(order[r.leg], r.pos[1]) for r in report['rows']
                               if r.section == 'context'}
                    assert context == wanted_context, (directory.name, stem,
                                                        'context', context)
                cells = paired_cells(report['rows'], reverse=reverse)
                if reverse:
                    assert cells == forward, (directory.name, stem, 'operand reversal')
                else:
                    forward = cells
                    pairs = defaultdict(dict)
                    for row in report['rows']:
                        if row.section == 'delta':
                            pairs[row.pair][row.leg] = row
                    actual = {(p[0].pos[1], p[1].pos[1]) for p in pairs.values()
                              if len(p) == 2 and p[0].sign == p[1].sign == '+'}
                    assert set(wanted_pairs) <= actual, (directory.name, stem,
                                                        set(wanted_pairs) - actual)
                count += 1
    return count


def check_reindentation_cases(binary, work, env):
    count = 0
    generator = ROOT / 'tests/support/gitread-unit'
    derive = ROOT / 'tests/support/derive-patch'
    for case in source_cases():
        directory = work / ('reindent-' + case['name'])
        directory.mkdir(exist_ok=True)
        shutil.rmtree(directory / 'repo', ignore_errors=True)
        for leg in (0, 1):
            for kind, field in (('source', 'before'), ('result', 'after')):
                (directory / f'p{leg + 1}-{kind}').write_text(case[field][leg])
        family = 'recipe-c' if case['path'].endswith('.c') else 'recipe'
        subprocess.run([generator, 'build-store', family, directory / 'repo'],
                       env=env, cwd=directory, check=True, capture_output=True)
        patches = [subprocess.check_output([derive, directory / 'repo', revision], env=env)
                   for revision in ('bp', 'up')]
        for leg, patch in enumerate(patches):
            (directory / f'patch{leg + 1}').write_bytes(patch)
        expected = native_edits(patches)
        assert case['shared'] <= expected, (case['name'], 'changed native patch roles')
        expected -= case['shared']
        sources = [{case['path']: [dict(enumerate(lines(case[field][leg].encode())))
                                  for field in ('before', 'after')]} for leg in (0, 1)]
        count += check_reports(binary, directory, env, patches, expected, sources,
                               case.get('pairs', ()), case.get('context_rows'))

    directory = work / 'reindent-unknown-gap'
    directory.mkdir(exist_ok=True)
    patches = []
    for leg, side in enumerate(('left', 'right')):
        indent = '\t' * (leg + 1)
        before = [f'{side}_head();\n', f'{side}_tail();\n'] + [
            f'{side}_gap_{i}();\n' for i in range(30)] + [
            f'{side}_next();\n', f'{side}_end();\n']
        after = before[:1] + [indent + 'first();\n', '\n', indent + '{\n',
                             indent + '}\n'] + before[1:-1] + [
            indent + 'second();\n', '\n', indent + '{\n', indent + '}\n'] + before[-1:]
        patch = ''.join(difflib.unified_diff(before, after, 'a/f', 'b/f', n=1)).encode()
        assert patch.count(b'@@ -') == 2
        patches.append(patch)
        (directory / f'patch{leg + 1}').write_bytes(patch)
    count += check_reports(binary, directory, env, patches, native_edits(patches))
    return count


if __name__ == '__main__':
    import sys
    binary = Path(sys.argv[1]).resolve()
    work = ROOT / 'build/tests' / (binary.name + '-reindent')
    work.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, ASAN_OPTIONS='verify_asan_link_order=1',
               UBSAN_OPTIONS='halt_on_error=1', LD_PRELOAD='')
    print(f'PASS {check_reindentation_cases(binary, work, env)} indentation correspondence checks')
