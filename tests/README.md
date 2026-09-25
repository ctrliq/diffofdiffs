# Regression checks

Tests require Python 3.9 or newer. Building diffofdiffs itself only needs
Python 3.6 or newer to embed the HTML assets.

```sh
make check
make check-asan
make run-NAME
make run-asan-NAME
```

Make uses `python3` by default. Set `PYTHON` to an interpreter name or
executable path to use another version for asset generation and every test
helper:

```sh
make PYTHON=python3.9 check
```

Each directory with a `spec` file is a byte/diagnostic fixture. The runner
copies inputs into its own directory under `build/tests`, runs the selected
binary, and checks stdout, stderr, exit status, and any specified limits. A
failed directory survives for inspection. The default binary is DIFFOFDIFFS.
Large output can be recorded with `stdout-sha1` instead of `expected.out`.
See `run-one.sh` for the spec keys and input generators.

Both full suites include the native source-match and public review checks.
The sanitizer suite keeps address, undefined-behavior, and leak checks enabled.
It may need an ordinary process environment for LeakSanitizer's process
inspection. Fixtures and review checks keep their inputs under `build/tests`;
other focused checkers use temporary directories that they remove on exit.

## What establishes correctness

Expected output records the reviewed presentation. It is not an independent
answer to whether the program accounted for the changes correctly.

`review_oracle.py` parses the original patches independently of the C reader.
It checks that each displayed edit belongs to its own original patch at the
printed coordinates, with the right bytes, sign, and final newline. A displayed
edit may appear only once. Retained rows must be real parent and result source.
The original changes left undisplayed must balance as shared changes.

After normal I/O checks, `check-fixture-review.py` audits successful complete
comparisons through the numbered text renderer. In tree mode it checks actual
Git blobs. Help, interrupted output, and other deliberately incomplete I/O
tests keep their own specific assertions instead.

A fixture may provide independent complete source files:

- `p1-source` and `p1-result` for patch1.
- `p2-source` and `p2-result` for patch2.

GNU patch must apply each original input to its own source at zero fuzz and
obtain its recorded result, including final newlines. The source checker also
checks the quoted coordinates against these files. Write these files from
the intended input changes, never from the program's report.

`check-review.py` adds independently chosen edit expectations, sparse-source
and complete-tree cases, source movement, repeated context, file actions,
binary records, quoted names, final newlines, and terminal behavior. Deliberate
corruptions verify that the oracle rejects wrong ownership, invented source,
wrong coordinates or files, missing or duplicated edits, and altered newlines.
Exact margin cases cover zero, one, two, three, and larger context counts on
both sides of differing additions and deletions, including shared edits.
Wrapping checks reassemble Unicode and non-UTF-8 source, preserve final
newlines, and reject changed continuation text. Physical source lines have
fixed bounds; wrapping never changes their original coordinates.

TTY tests expand tabs independently from each source line's start at several
tab widths, including wrapped tabs, wide characters, trailing whitespace, and
growing number gutters. They check automatic color, width fitting beyond
100 source columns, explicit caps, and destination detection through `-o`.
Redirected reports keep source tabs and ignore terminal dimensions. Equal
widths, number gutters, padding, and blank line color are checked independently
of the C renderer. Compact framing checks cover long labels, four-digit
coordinates, shared edits without signs, narrow banners, and ASCII separators
when no UTF-8 locale is available. They reject colored padding, trailing layout
spaces, and backgrounds.

Regrouping input hunks must not move an edit's counterpart. Zero-margin and
one-sided source cases keep context differences while excluding remote
background.
The native matcher's separate checks validate source identity directly.

Accounting is necessary but cannot decide whether a repeated line was paired
at the correct place. Location assertions and manual comparison with the
original source supplement it. For example, an addition within a shared loop
must remain visible without presenting the loop itself as a deletion.

## Optional external corpora

The ordinary suites are self-contained. `check-corpus.py` additionally accepts
an external input set without changing it. Each run requires a new output
directory so partial and completed evidence survive. The directory records
the program, source hashes, input patches, assertions, output, commands, and
per-pair results.

The corpus directory contains a `repro.json` array. Each object has a unique
`name` suitable for a directory name and two revision identifiers, `bp` and
`up`. Patch mode reads `patches/bp-<bp>.patch` and `patches/up-<up>.patch`.
Tree mode resolves both revisions in `--git-tree` and derives their changes
from the first parent. Commit-based inputs intended for others to reproduce
should identify the public repository and revisions in the corpus's own
documentation.

```sh
make diffofdiffs tests/support/derive-patch
python3 tests/check-corpus.py --mode patch --reverse \
    --corpus /path/to/corpus \
    --output build/corpus-patch
python3 tests/check-corpus.py --mode tree --reverse \
    --corpus /path/to/corpus --git-tree /path/to/repository \
    --output build/corpus-tree
```

Use `--binary ./diffofdiffs-asan` with a separate output directory for the
sanitizer audit. `--jobs` controls concurrency. `--reverse` also compares the
operands in the opposite order and requires the same displayed source rows,
roles, and coordinates. Tree input derivation is checked against actual parent
and result blobs read independently with Git.

Use `--expectations FILE` for additional source-location assertions chosen
independently of the output. The JSON array contains objects with `bp` and
`up` equal to the identifiers in `repro.json`, and `edits` listing every
unshared delta edit as `[leg, path, sign, line]`. `leg` is 0 or 1; `line` is
one-based in the original parent for removals or result for additions. An
optional `empty_delta: true` also excludes retained or shared delta rows.

Tree assertions may additionally specify:

- `equal_context`: required pairs as `[[left_path, right_path], left_line,
  right_line]`, using result coordinates and unchanged rows on both sides.
- `context_rows`: required rows as `[leg, path, sign, result_line]`.
- `context_exact`: excerpts with `leg`, `path`, and an ordered `lines` array
  of result coordinates. Optional `sign` and inclusive `between: [first,
  last]` fields restrict the rows before comparing that exact array.

The corpus audit checks source fidelity and direction consistency; it does
not certify every heuristic correspondence. Compare reports with both
original patches and complete source where available. Add a concrete location
assertion when a plausible match turns out to be wrong.
