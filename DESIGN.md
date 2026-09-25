<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Design

The engine compares each patch's original actions and the actual source at
the two tips. A renderer receives selected pairs of source rows. It does not
make another set of matching decisions. The text and standalone HTML
renderers consume the same selected rows and highlight ranges.

## Data flow and ownership

| Module | Responsibility |
| --- | --- |
| `main.c`, `cli.c` | Options, operands, and input mode |
| `reader.c` | Patch grammar, exact names, metadata, and file pairing |
| `patch.c` | Original rows, parent/result views, and related file actions |
| `review.c` | Correspondence, edit ownership, and selected row pairs |
| `render.c` | Number gutters, equal columns, wrapping, and optional color |
| `html.c`, `html.css`, `html.js` | Standalone HTML rendering and viewer controls |
| `highlight.c` | Word, character, and indentation highlight ranges |
| `display.c` | Source escaping and display-column measurement |
| `udiff.c` | Exact line matching and unified diff through libgit2 |
| `gitread.c`, `treediff.c`, `assemble.c`, `gittree.c` | Git objects and operand patches |
| `iomem.c`, `util.c` | Owned bytes, allocation, I/O, and diagnostics |

A patch row owns its original sign, old/new coordinates, source bytes,
final-newline state, and hunk identity. The parent view omits additions; the
result view omits deletions. Both views index the same rows. Unknown intervals
are explicit gaps whose storage does not grow with their line numbers.
Contradictory quotations preserve their rows without gaining a guessed match.

For each paired file, `review.c` builds temporary source views, parent/result
matches, and original-edit mates. It selects delta rows and tip rows, then
copies only the selected source, coordinates, and labels into the report.
Full Git blobs and temporary maps are freed before processing the next file.
The report owns everything needed by the renderer and has one destructor.
This avoids keeping all complete source files alive while measuring the final
column width.

## Source correspondence

Matching is byte-exact, including whitespace and final-newline state. A
canonical order based on source and original row roles resolves ties
consistently when operands are reversed. It does not change the display order.

In patch mode, the known parent quotations are matched directly. In tree
mode, the original quotations are first compared in their own canonical
order. Unique, nonblank matches locate their neighborhoods in the complete
parents. A matched native edit block can also provide anchors: if it contains
a matched nonblank line unique in both complete views, all of its matched
rows keep their correspondence, including repeated returns and blanks.
Unmatched edits inside the opposite block do not break that relationship;
retained source between edits does. Removals anchor parents, and additions
anchor results. The intervals between anchors are compared normally.

Unique, nonblank parent matches that both patches retain also anchor the
result comparison. An added block cannot cross these retained boundaries to
meet identical text at a different location. Other result matches remain
flexible around insertions and deletions. A retained blank is not a hard
boundary that can split a shared loop. Original hunk grouping is not a
source of result boundaries: merging two hunks must not let a later insertion
take an earlier insertion's counterpart.

`udiff.c` is a small adapter over the existing libgit2 dependency. It borrows
complete byte buffers when supplied and packs separate rows otherwise. It
uses the indentation heuristic. Small comparisons also request a minimal edit
script; comparisons above 65,536 combined rows use the library's bounded
default search. Identical images use an identity fast path. Every returned
context match is checked against the original line bytes before becoming a
correspondence. The adapter retains the 2,097,152-line and 64 MiB per-image
limits, original source signs, coordinates, and newline markers.
The command keeps one library reference for the entire review. Individual
adapter calls retain their own references for standalone callers, without
repeating library startup for every small interval in patch mode.

An unequal input must produce hunks, and a full matching traversal must visit
every original row. The adapter checks those conditions because an allocation
failure in the library can return zero without callbacks. Complete inputs
avoid duplicate buffers so the large-output fixture fits the existing memory
limit in both normal and sanitizer builds.

## Original edit ownership and paired rows

Original edit mates record corresponding rows with the same original sign.
Delta classification reads this relation independently of display selection.
An addition paired with retained source is still an edit difference. Equal
result text alone cannot hide a differing deletion.

Two existing operation rules remain. An identical original hunk can be
shared in a different file when its signature is unique; duplicate candidates
stay visible, and exact signs and bytes decide equality. An irreversible
whole-file deletion that omits source can share an explicitly quoted deletion
only when both name the same nonzero preimage object. Two quoted deletions
are still compared as original rows.

A cross-file hunk match keeps its partner file and hunk, not just a shared
flag. A focused context comparison follows that pair through the complete
sources, using original edit lines and their insertion gaps as boundaries.
Normal file actions remain attached to their original entries. Context for
the transferred hunk is not also selected in the unrelated file comparison.
Partial, modified transfers remain a limitation of this whole-hunk rule.

Result correspondence supplies an ordered sequence of paired rows. Within
each gap between result matches, parent correspondence pairs original
deletions. Remaining rows keep their original order with empty cells where
needed. There is no separate fragment scoring or renderer realignment.

Differing edits seed delta selection. A retained counterpart can also seed a
region when the other patch quotes it, or when its exact nonblank source is
unique on both sides of that comparison. An unquoted repeated return or blank
line cannot locate a counterpart by itself. Each side adds the requested
number of available source rows around its seeds. Shared signed rows can
count as context; display padding cannot.
Matching context can fill the opposite cell when the other patch quotes it.
It cannot spread into unquoted background: a repeated declaration must not
pull an unrelated function into the report.
Groups break only at actual gaps in the selected source.

## Context scope and presentation

The context section uses the actual result views and their complete-source
correspondence. It selects differing rows within original quoted tip regions
and the requested neighborhood of edits, then adds adjacent matching context.
A component must contain relevant retained source; purely introduced
differences already belong in delta. Comparisons crossing unknown source
gaps are not presented as established differences.

The unknown-source check also bounds display margins. Once a region lacks
the source needed to establish correspondence, a neighboring selection cannot
bring its rows back as context differences. This preserves the patch-mode
outer anchor even when the requested margin extends past it.

The source region and the display margin are separate. Zero margin keeps
context differences within quoted source. A nearby unmatched interval does
not cause a whole absent file or remote inherited region to be printed.
Context reports are excerpts, and their boundaries can fall inside a larger
tip difference. The full-source comparison establishes row identity before
selection, so clipping does not manufacture a difference by comparing two
unrelated window edges.

The public report contains paired rows and group labels, with no matcher
state. Each delta row keeps old/new numbers according to its original sign;
context rows have tip numbers. Missing cells have neither source nor numbers.
The engine retains each shared row's original sign, but the CLI renderer leaves
its single sign position empty and uses the terminal's normal foreground.
The remaining old or new coordinate identifies its original role. Differing
rows use red or green foreground; changed blanks color only their gutters.
Function labels follow the existing deterministic source-line convention and
original hunk comments; they do not interpret function behavior. A compact
`@@` prefix separates these labels from source.

The renderer measures one source width for every file and section. On a TTY,
the available terminal width limits it; an explicit `--max-column-width`
can impose a smaller cap. Redirected text and unavailable terminal dimensions
use a default cap of 100 source columns. Paths, labels, and notes wrap within
this width and cannot enlarge it. A gutter contains one sign, two number
fields, one space between them, and one separator. Number fields have at
least three digits and grow together for larger coordinates. The two cells
meet at a heavier separator without extra spaces. Section, file, and group
separators use this same geometry. UTF-8 locales use box-drawing characters;
other effective locales use ASCII separators.

TTY source tabs expand to spaces at stops measured from the source line's
start. `--tab-width` selects their width, with eight columns by default, and
also controls HTML's visual tab size. Redirected text and HTML text nodes
keep literal tabs. Raw text files use eight-column stops measured from the
start of the whole output row; their pane positions affect tab measurement.
Widths aren't rounded to those stops. Source columns have a minimum width of
two cells, or eight when redirected source contains tabs, even if a requested
cap or terminal width is smaller. UTF-8 display width controls wrapping;
nonprinting bytes are escaped. In a single-byte locale, the program first
tries C.UTF-8.
Continuations receive no new sign or coordinate. Colored spans end before
padding, and layout adds no trailing spaces; trailing source whitespace is
preserved. Plain and colored output share all layout code.

## Git input and verification

Tree mode resolves each operand against its selected parent and derives a
patch with three quoted context rows. Git trees are compared by object ID and
raw mode before descent. Equal subtrees are skipped; changed entries retain
the existing ordering, raw-mode handling, and diagnostics. Empty comparisons
need no entry sorting. No checkout or index operation is involved.

The independent Python checker validates displayed source, coordinates,
original signs, final newlines, and edit accounting. These checks cannot prove
every correspondence. The suite also uses independently chosen locations,
unchanged output under hunk regrouping, exact context extents, reversed
operands, terminal tests, and deliberate corruption of valid reports. Optional
corpus checks compare every displayed source row in both directions. Direct
correspondence assertions cover repeated insertions, guards, and split
functions; balanced row counts alone would accept a wrong-function match.
A shared removal block must stay together when the other patch removes an
additional row inside that block.

Repeated source, reordered code, and changed file organization remain limits
of deterministic correspondence. Identical edits can remain visibly unpaired
when the available source does not establish their identity. Saved output is
a presentation check, not its own correctness oracle. Full source still does
not guarantee useful alignment of repeated retained braces and blank lines
around large refactorings.
