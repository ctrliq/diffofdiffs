#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Audit every corpus report against its inputs and actual Git source files."""

import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

from review_oracle import paired_cells, check_review, parse_patch
from tree_oracle import derive_inputs, tree_sources

ROOT = Path(__file__).resolve().parents[1]


def digest(data):
    return hashlib.sha256(data).hexdigest()


def context_matches(report):
    pairs = {}
    for row in report['rows']:
        if row.section == 'context':
            pairs.setdefault((row.paths, row.pair), {})[row.leg] = row
    return {(p[0].paths, p[0].pos[1] + 1, p[1].pos[1] + 1)
            for p in pairs.values() if len(p) == 2 and p[0].sign == p[1].sign == ' '}


def check_expectations(case, report, mode, expectations):
    """Location assertions chosen from the original patches, not tool output."""
    for known in expectations:
        if known['bp'] != case['bp'] or known['up'] != case['up']:
            continue
        actual = {(row.leg, row.paths[row.leg], row.sign, row.pos[row.sign == '+'] + 1)
                  for row in report['rows'] if row.section == 'delta' and row.sign in '+-' and not row.shared}
        assert actual == set(map(tuple, known['edits'])), ('source-derived native rows', case['name'], actual)
        if mode == 'tree':
            wanted = {(tuple(paths), left, right) for paths, left, right in known.get('equal_context', [])}
            assert wanted <= context_matches(report), ('equal context pairs', case['name'])
            wanted = set(map(tuple, known.get('context_rows', [])))
            actual = {(row.leg, row.paths[row.leg], row.sign, row.pos[1] + 1)
                      for row in report['rows'] if row.section == 'context'}
            assert wanted <= actual, ('source context correspondence', case['name'])
            for excerpt in known.get('context_exact', []):
                actual = [row.pos[1] + 1 for row in report['rows']
                          if row.section == 'context' and
                          row.leg == excerpt['leg'] and
                          row.paths[row.leg] == excerpt['path'] and
                          ('sign' not in excerpt or row.sign == excerpt['sign']) and
                          ('between' not in excerpt or
                           excerpt['between'][0] <= row.pos[1] + 1 <= excerpt['between'][1])]
                assert actual == excerpt['lines'], ('source context excerpt', case['name'], excerpt, actual)
        if known.get('empty_delta'):
            assert not any(r.section == 'delta' for r in report['rows']), ('nonempty delta', case['name'])

def run_case(case, options, binary, derive, env):
    directory = options.output / case['name']
    directory.mkdir()
    revisions = [case['bp'], case['up']]
    if options.mode == 'tree':
        args = ['--git-tree=' + str(options.git_tree), *revisions]
        patches = derive_inputs(options.git_tree, revisions, derive, env)
        sources = tree_sources(options.git_tree, revisions, patches)
    else:
        paths = [options.corpus / 'patches' / f'{leg}-{case[leg]}.patch'
                 for leg in ('bp', 'up')]
        args = [str(p) for p in paths]
        patches = [p.read_bytes() for p in paths]
        sources = None
    for leg, patch in enumerate(patches, 1):
        (directory / f'patch{leg}').write_bytes(patch)
    command = [str(binary), '--color=never', *args]
    run = subprocess.run(command, env=env, capture_output=True, timeout=120)
    (directory / 'review.txt').write_bytes(run.stdout)
    (directory / 'stderr').write_bytes(run.stderr)
    assert run.returncode == 0 and not run.stderr, (run.returncode, run.stderr)
    report = check_review(patches, run.stdout, sources, context=3)
    check_expectations(case, report, options.mode, options.expectations)
    inventories = [Counter((row.sign, row.text) for file in parse_patch(patch)
                           for row in file.rows if row.sign in '+-' and not row.neutral)
                   for patch in patches]
    minimum = sum(sum((inventories[a] - inventories[1 - a]).values())
                  for a in (0, 1))
    result = dict(name=case['name'], checked=True, command=command,
                  inputs_sha256=[digest(p) for p in patches],
                  output_sha256=digest(run.stdout), bytes=len(run.stdout),
                  shown=report['shown'], shared=report['shared'],
                  minimum_by_text=minimum,
                  extra_over_text_minimum=sum(report['shown']) - minimum)
    if options.reverse:
        reversed_args = args[:-2] + args[-2:][::-1]
        reverse = subprocess.run([str(binary), '--color=never', *reversed_args],
                                 env=env, capture_output=True, timeout=120)
        (directory / 'reverse.txt').write_bytes(reverse.stdout)
        assert reverse.returncode == 0 and not reverse.stderr
        reverse_report = check_review(patches[::-1], reverse.stdout,
                                      sources[::-1] if sources else None, context=3)
        result['reverse_shown'] = reverse_report['shown']
        result['reverse_sha256'] = digest(reverse.stdout)
        original_rows = [Counter((row.sign, row.pos, row.text) for row in rows)
                         for rows in report['edits']]
        reversed_rows = [Counter((row.sign, row.pos, row.text) for row in rows)
                         for rows in reverse_report['edits'][::-1]]
        result['direction_changes_selection'] = original_rows != reversed_rows
        def all_rows(review, reversed=False):
            return Counter((r.section, r.paths[r.leg], 1-r.leg if reversed else r.leg,
                            r.pos, r.text, r.shared,
                            r.sign if r.section == 'delta' else r.sign != ' ')
                           for r in review['rows'])
        result['direction_changes_rows'] = all_rows(report) != all_rows(reverse_report, True)
        assert paired_cells(report['rows']) == paired_cells(reverse_report['rows'], True), 'operand reversal changed pairing'
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--corpus', type=Path, required=True)
    parser.add_argument('--mode', choices=('patch', 'tree'), required=True)
    parser.add_argument('--git-tree', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--binary', type=Path, default=ROOT / 'diffofdiffs')
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--reverse', action='store_true')
    parser.add_argument('--expectations', type=Path,
                        help='optional JSON source-location assertions')
    options = parser.parse_args()
    if options.mode == 'tree' and options.git_tree is None:
        parser.error('--mode=tree requires --git-tree')
    for key in ('corpus', 'git_tree', 'output', 'binary', 'expectations'):
        value = getattr(options, key)
        if value is not None:
            setattr(options, key, value.resolve())
    # Preserve completed and partial audits; every run gets its own directory
    options.output.mkdir(parents=True)
    binary, derive = options.output / 'program', options.output / 'derive-patch'
    shutil.copy2(options.binary, binary)
    shutil.copy2(ROOT / 'tests/support/derive-patch', derive)
    env = dict(os.environ, TMPDIR=str(options.output), LD_PRELOAD='',
               ASAN_OPTIONS='verify_asan_link_order=1', LSAN_OPTIONS='',
               UBSAN_OPTIONS='halt_on_error=1')
    files = sorted(ROOT.glob('src/**/*.c')) + sorted(ROOT.glob('src/**/*.h'))
    files += [Path(__file__), ROOT / 'tests/review_oracle.py', ROOT / 'tests/tree_oracle.py']
    manifest = dict(binary_sha256=digest(binary.read_bytes()),
                    derive_sha256=digest(derive.read_bytes()),
                    source_sha256={str(p.relative_to(ROOT)): digest(p.read_bytes()) for p in files},
                    arguments={k: str(v) if isinstance(v, Path) else v
                               for k, v in vars(options).items()})
    data = options.expectations.read_bytes() if options.expectations else b'[]'
    (options.output / 'expectations.json').write_bytes(data)
    manifest['expectations_sha256'] = digest(data)
    options.expectations = json.loads(data)
    (options.output / 'provenance.json').write_text(json.dumps(manifest, indent=2) + '\n')
    cases = json.loads((options.corpus / 'repro.json').read_text())

    def checked(case):
        try:
            return run_case(case, options, binary, derive, env)
        except Exception as error:
            return dict(name=case['name'], checked=False, failure=repr(error))

    with ThreadPoolExecutor(max_workers=options.jobs) as pool:
        records = []
        with (options.output / 'checks.jsonl').open('w') as output:
            for record in pool.map(checked, cases):
                records.append(record)
                output.write(json.dumps(record) + '\n')
                output.flush()
    summary = dict(pairs=len(records), passed=sum(r['checked'] for r in records),
                   failed=[r['name'] for r in records if not r['checked']],
                   direction_changes=[r['name'] for r in records
                                      if r.get('direction_changes_selection') or r.get('direction_changes_rows')])
    (options.output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary))
    return bool(summary['failed'] or summary['direction_changes'])


if __name__ == '__main__':
    sys.exit(main())
