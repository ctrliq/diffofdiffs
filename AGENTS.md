# Project instructions

Read [CODING_STYLE.md](CODING_STYLE.md) before changing code; it is mandatory.
[CODE_DESIGN.md](CODE_DESIGN.md) records additional design and style requirements.
[DESIGN.md](DESIGN.md) describes the comparison pipeline and ownership rules.
Read [tests/README.md](tests/README.md) before changing tests or expected output.

Use `Assisted-by: LLM` as the assistant trailer for commits in this repository.

## Comparison contracts

- Preserve each patch's original edit signs, source bytes, coordinates, and
  final-newline state. Do not reconstruct a hypothetical common result by
  applying, reversing, or relocating edits.
- Exact source equality and display correspondence are separate. Normalized
  indentation and text similarity can guide presentation; they cannot make
  unequal bytes into shared edits or change edit ownership.
- Unquoted source in patch mode remains unknown. Tree mode reads Git objects
  without checking out revisions or changing the repository's index.
- Preserve source order on each side. Check operand reversal and changes in
  hunk grouping when modifying correspondence or file pairing.
- Matching and selection belong in the comparison engine. The CLI and HTML
  renderers consume the selected row pairs without deciding which edits match.
- Keep literal tabs in redirected text output and HTML text nodes. For TTY
  text output, expand tabs from each source line's start using --tab-width,
  with eight columns by default. Preserve trailing source whitespace, and
  keep layout padding from creating trailing spaces. Rendering must not
  change source identity or edit ownership.
- Keep HTML reports standalone, with their assets embedded and no network
  access needed for viewing them.

## Build and validation

Build dependencies and supported options are documented in README.md. The
usual commands are:

```sh
make
make checkstyle
make check
make check-asan
```

Run checks appropriate to the change. Engine, reader, and renderer changes
need the normal and sanitizer suites. Individual fixtures can be run with
`make run-NAME` and `make run-asan-NAME`; the matcher, review, and highlight
checkers also have separate Makefile targets.

Derive expected behavior from the original inputs. Recorded output is a
presentation baseline, not proof of correctness. Explain changed rows before
updating a golden file, and use source or ownership assertions for logic
regressions. Preserve useful coverage when rewriting an unclear test comment.

## Public, reproducible content

Comments, tests, and documentation must make sense without access to a
developer's machine, private reports, or session history. Describe the input
condition, the required behavior, and any relevant constraint directly.

Keep ordinary tests self-contained. Temporary Git repositories generated
from tracked source are suitable fixtures. External integrations must be
explicitly optional and document their inputs; commit-based examples need a
verifiable public repository and revision. Finding an object in a local
repository does not establish public provenance.

Keep each change focused. Follow CODING_STYLE.md for comments, naming,
formatting, and commit messages rather than introducing a separate style here.
