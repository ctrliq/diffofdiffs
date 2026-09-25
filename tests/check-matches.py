#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check source identities independently of unified diff presentation."""
from pathlib import Path
import json
import random
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
DRIVER = Path(sys.argv[1]).resolve()
WORK = ROOT / 'build' / 'tests' / DRIVER.name
WORK.mkdir(parents=True, exist_ok=True)
RESULTS = []


def check(name, left, right, required=(), exact=None):
    a, b = WORK / 'old', WORK / 'new'
    a.write_bytes(b''.join(left))
    b.write_bytes(b''.join(right))
    result = subprocess.run([str(DRIVER), str(a), str(b)], capture_output=True,
                            timeout=15)
    (WORK / 'stdout').write_bytes(result.stdout)
    (WORK / 'stderr').write_bytes(result.stderr)
    assert result.returncode == 0 and not result.stderr, (name, result.stderr)
    pairs = [tuple(map(int, line.split())) for line in result.stdout.splitlines()]
    assert len(set(a for a, b in pairs)) == len(pairs), name
    assert len(set(b for a, b in pairs)) == len(pairs), name
    assert pairs == sorted(pairs), name
    assert [b for a, b in pairs] == sorted(b for a, b in pairs), name
    assert all(left[a] == right[b] for a, b in pairs), name
    assert set(required) <= set(pairs), (name, 'missing source correspondence')
    if exact is not None:
        assert set(pairs) == set(exact), (name, pairs, exact)
    RESULTS.append({'case': name, 'left': len(left), 'right': len(right),
                    'matched': len(pairs)})


check('empty', [], [], exact=[])
check('one-empty', [b'a\n'], [], exact=[])
check('other-empty', [], [b'a\n'], exact=[])
check('identity', [b'a\n', b'\n', b'b'], [b'a\n', b'\n', b'b'],
      exact=[(0, 0), (1, 1), (2, 2)])
check('final-newline', [b'a\n'], [b'a'], exact=[])
check('embedded-nul-difference', [b'a\0b\n'], [b'a\0c\n'], exact=[])
check('embedded-nul-equality', [b'a\0b\n'], [b'a\0b\n'], exact=[(0, 0)])
check('repeated-fallback', [b'x\n'] * 200, [b'y\n'] + [b'x\n'] * 200,
      exact=[(i, i + 1) for i in range(200)])
check('unrelated-files', [f'left {i}\n'.encode() for i in range(2000)],
      [f'right {i}\n'.encode() for i in range(2000)], exact=[])

# A comment opener belongs to the whole shared helper block. Matching an
# earlier opener with it would falsely separate the first removed line.
helper = [b'/*\n', b' * helper description\n', b' */\n', b'void helper(void)\n',
          b'{\n', b'\tfirst();\n', b'\tsecond();\n', b'}\n']
prefix = [b'/*\n', b' * old surrounding comment\n', b' */\n', b'old_call();\n']
check('whole-removed-block', prefix + helper, helper,
      exact=[(i + len(prefix), i) for i in range(len(helper))])

loop = [b'\tlock();\n', b'\tfor (i = 0; i < n; i++) {\n',
        b'\t\twork(i);\n', b'\t}\n', b'\tunlock();\n']
wrong_function = [b'void other_function(void)\n', b'{\n'] + loop + [b'}\n']
correct_function = [b'void intended_function(void)\n', b'{\n'] + loop + [b'}\n']
check('loop-stays-in-its-function', wrong_function + correct_function,
      correct_function,
      exact=[(i + len(wrong_function), i) for i in range(len(correct_function))])

# Known unique lines let insertion and deletion positions be checked without
# relying on the matcher's own script or tie-breaking choices.
rng = random.Random(299)
for case in range(100):
    count = rng.randrange(2, 100)
    left = [f'original {case} {i}\n'.encode() for i in range(count)]
    start = rng.randrange(count)
    removed = rng.randrange(count - start + 1)
    added = [f'new {case} {i}\n'.encode() for i in range(rng.randrange(6))]
    right = left[:start] + added + left[start + removed:]
    expected = [(i, i) for i in range(start)]
    expected += [(i, i - removed + len(added))
                 for i in range(start + removed, count)]
    check(f'known-edit-{case}', left, right, exact=expected)
    check(f'known-edit-reversed-{case}', right, left,
          exact=[(b, a) for a, b in expected])

(WORK / 'results.json').write_text(json.dumps(RESULTS, indent=2) + '\n')
print(f'matching: {len(RESULTS)} source identity checks passed')
