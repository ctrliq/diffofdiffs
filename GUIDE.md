<!-- SPDX-License-Identifier: GPL-2.0-only -->

# User guide

diffofdiffs compares two versions of a patch side by side. Its report shows
both what each patch changed and how the surrounding source differs after
those changes. This helps you review a backport, a revised fix, or a rebase
without losing track of the original additions and removals.

See the [README](README.md) for installation and a quick introduction. The
examples using `samples/` below run from this repository's root after building
diffofdiffs. Run `diffofdiffs --help` for the complete option list.

## Contents

- [Choose the inputs](#choose-the-inputs)
  - [Patch mode](#patch-mode)
  - [Tree mode](#tree-mode)
- [Read the report](#read-the-report)
  - [Common layout](#common-layout)
  - [Delta differences](#delta-differences)
  - [Context differences](#context-differences)
  - [Identical results, different edits](#identical-results-different-edits)
- [Adjust the context](#adjust-the-context)
- [Read highlights and whitespace](#read-highlights-and-whitespace)
- [Use terminal output](#use-terminal-output)
  - [Width and wrapping](#width-and-wrapping)
  - [Saving and copying](#saving-and-copying)
- [Use a standalone HTML report](#use-a-standalone-html-report)
  - [Viewing controls](#viewing-controls)
  - [Navigation and copying](#navigation-and-copying)
- [Understand the limits](#understand-the-limits)

## Choose the inputs

Each of the two inputs represents one patch, possibly changing several files.
You can supply patch files directly or ask diffofdiffs to read commits from a
local Git repository. The first input appears on the left, and the second
appears on the right.

### Patch mode

To compare two patch files:

```sh
diffofdiffs first.patch second.patch
```

Patch mode accepts unified diffs from `git diff` or `diff -u`, including Git
metadata for renames, mode changes, and binary changes. A single
`git format-patch` email can include its preamble. Context diffs (`diff -c`)
and combined diffs (`git diff --cc`) aren't supported.

Supply one patch per input. The reader doesn't detect a concatenated email
series as separate commits; split it into individual patches before comparing
them. Either input can be `-` to read a patch from standard input, but only
one of them can use it at a time.

Patch mode knows only the source lines quoted in the patches. Code omitted
between hunks or beyond their edges remains unknown. A larger context setting
cannot recover those missing lines.

### Tree mode

To compare commits using the complete files before and after each change:

```sh
diffofdiffs --git-tree=/path/to/repository commit1 commit2
```

Tree mode reads Git objects without checking out revisions or changing the
index. The repository can be a working tree or a bare repository. Use commit
IDs, unambiguous abbreviated IDs, branch names, tags, or `HEAD`. Revision
expressions such as `HEAD~2` aren't supported; resolve them to commit IDs with
Git first.

Each revision is compared with its first parent; a root commit uses an empty
tree. To select another parent, use `BASE..TIP` as an operand, where `BASE`
must be a parent of `TIP`. This notation selects one commit's change against
that parent, not a series of commits.

Tree mode handles regular text files, symbolic links, and submodule entries.
It stops with an error on changed files containing NUL bytes, paths containing
tabs or newlines, and unsupported entry modes. To compare binary changes, use
patch mode with patches produced by `git diff --binary`.

## Read the report

The [arguments sample](samples/arguments) provides a small comparison to follow:

```sh
./diffofdiffs --backport-labels \
    samples/arguments/backport.patch samples/arguments/upstream.patch
```

Both patches add sequence-count calls around an assignment in `record.c`.
The upstream calls use `record->state.seq`, while the backport uses
`record->seq`. Both add `record->generation++;`, and the upstream source also
contains a guard that was already present before its patch.

The report separates these facts into **DELTA DIFFERENCES**, for the original
edits, and **CONTEXT DIFFERENCES**, for the nearby resulting source. You can
also follow the sample in its [HTML report](samples/arguments.html).

### Common layout

A nonempty text report begins with centered input labels between horizontal
borders. The labels identify each input as a patch or commit and appear only
at the top. `--backport-labels` names the left side Backport and the right
side Upstream, as in the sample.

Below the labels, a note explains which source is available: complete files
before and after each commit in tree mode, or only the lines quoted in the
patches in patch mode. Section banners then introduce the comparisons.

A thick horizontal line starts each file, and thinner lines separate groups
of changes. A group's `@@` label gives the available function name or original
hunk comment; a bare `@@` means neither is available. These labels appear in
cyan when terminal color is enabled.

A blank source line has a line number. An alignment gap has neither source
nor a number, and HTML marks these gaps with hatching by default. File
creation, deletion, rename, copy, and mode differences also appear when
relevant. Patch mode can also report binary differences.

Only sections with output appear in a text report. Identical inputs produce
no text output. A report describes the comparison; it isn't a patch
to apply to either input.

### Delta differences

This section keeps each patch's original additions and removals, with its
own line numbers: `old` means before that patch, and `new` means after it.
In text output, the sign and numbers tell you what happened to a line:

| Sign | Numbers shown | Meaning |
| --- | --- | --- |
| `+` | `new` | An addition without a matching addition in the other patch |
| `-` | `old` | A removal without a matching removal in the other patch |
| Sign-less | `old` and `new` | Source retained by this patch |
| Sign-less | `new` only | An addition both patches make |
| Sign-less | `old` only | A removal both patches make |

With terminal color enabled, differing additions have green text, and
differing removals have red text. Sign-less lines use the same text color as
unchanged source. A differing blank line still has a colored sign and line
number.

Shared edits appear when needed as context around differing edits. In HTML,
differing additions and removals have green and red row backgrounds. Shared
edits keep their original `+` or `-` signs and have yellow shading by default.
An edit can be shared even when its counterpart appears at another position.

In the sample, both `write_seqcount_begin()` calls have a `+`: each patch
adds a call, but their arguments differ. Their new line numbers are 3 on the
left and 5 on the right. The `write_seqcount_end()` calls likewise differ.

The shared `record->generation++;` addition has no sign in the text report,
only new line numbers 5 and 7. The retained `record->value = value;` has both
old and new numbers: 3 and 4 on the left, and 5 and 6 on the right. These
numbers distinguish a shared addition from code neither patch changed.

### Context differences

This section compares the resulting source near the edits. Only result line
numbers appear, under `new`. Here `-` marks left-only source, and `+` marks
right-only source. With terminal color enabled, left differences have red
text, and right differences have green text. HTML uses the same red and green
assignment for row backgrounds.

In the sample, the upstream `if (!record->active)` and `return;` lines face
an empty backport cell. They have `+` signs in this section because they exist
only in the upstream result. They were sign-less retained source in the delta
section: the upstream patch didn't add them.

The differing sequence-count arguments also appear in the resulting source,
so the context section can show those calls with `-` on the left and `+`
on the right. The signs now describe the difference between the two results,
rather than each patch's original edit.

### Identical results, different edits

Matching final code doesn't mean two patches made the same change. For
example, save this patch as `first.patch`:

```diff
--- a/task.c
+++ b/task.c
@@ -1,2 +1,2 @@
-legacy_check();
+check();
 run();
```

Save the following patch as `second.patch`:

```diff
--- a/task.c
+++ b/task.c
@@ -1,3 +1,2 @@
 check();
-obsolete_step();
 run();
```

Compare them with `diffofdiffs first.patch second.patch`. Both leave exactly
the same two lines, `check();` followed by `run();`. However, only the first
patch adds `check();`; the second already had it. The patches also remove
different calls: `legacy_check();` and `obsolete_step();`.

The delta section shows that addition opposite retained source, along with
both differing removals. There is no context section for these inputs
because the resulting source agrees. Comparing only the final files would
hide all three edit differences.

## Adjust the context

Like GNU diff's unified-context option, `-U N` (or `--unified=N`) requests
N context lines before and after differences. The default is three. In the
delta section, N counts each patch's original rows, including removals and
shared edits; in the context section, it counts result lines. Alignment gaps
don't count. Nearby excerpts merge when their margins meet, and file
boundaries or missing patch quotations can leave fewer lines.

For a compact version of the arguments sample, omit surrounding matching
lines with `-U0`:

```sh
./diffofdiffs -U0 --backport-labels \
    samples/arguments/backport.patch samples/arguments/upstream.patch
```

The differing calls remain in the delta section, while the shared generation
increment drops out. The upstream guard still appears in the context section:
it is a difference within the quoted source, not matching context.

The context section is a bounded excerpt. A differing stretch must include
retained source near an edit; differences involving only added lines are
already covered by delta. Here, "near" includes the original quoted result
lines and N result lines around each edit. Each initial selection then gets
up to N source lines on either side; an empty side gets the same margin around
its corresponding gap. Newly exposed differences don't trigger another
expansion, so an excerpt can stop inside a longer source difference.

In tree mode, the complete files are compared before excerpts are selected.
Each commit's derived patch initially quotes three context lines,
independently of `-U`. In patch mode, enough quoted source must be available to
establish a difference. Unquoted gaps and quotation edges can leave context
differences unreported, including a final-newline mismatch at an uncertain
file boundary. Increasing `-U` cannot recover unknown source. Neither mode
prints every difference between two releases.

## Read highlights and whitespace

Line colors describe whole rows. Inline highlights draw attention to changed
text within an aligned pair:

- `--highlight=words`, the default, emphasizes changed words, punctuation,
  and whitespace.
- `--highlight=characters` narrows the emphasis to changed characters within
  those word ranges.
- `--highlight=none` disables inline emphasis while keeping line colors.

For example, when comparing `check_old(sp);` with `check_new(sp);`, word mode
emphasizes the entire names `check_old` and `check_new`. Character mode
emphasizes only `old` and `new`, leaving `check_` unmarked in both names.

Inline red marks changed left-side text, and inline green marks changed
right-side text. This is independent of the original edit signs: both
sequence-count calls in the sample are additions, yet their differing
arguments receive left-red and right-green emphasis.

In terminal output, highlighting requires color. Changed text uses bright
red or green. Changed whitespace receives cyan emphasis only when it touches
an unchanged space or tab on the same source line; its displayed spaces are
underlined. A separate run of changed spaces stays unmarked so it doesn't
look like underscores. Word highlighting can mark a whole whitespace run as
changed, leaving it without an unchanged neighbor; character highlighting
can isolate the extra spaces within that run. Wrapping doesn't change the
adjacency decision, and code characters aren't underlined or made bold.

HTML uses stronger red and green backgrounds behind changed text. Each
highlighted space has its own block, while a highlighted tab fills its tab
stop. `--tab-width=N` sets the tab stops, with eight columns by default.
Hover over a whitespace block to see its character type. These blocks
decorate the existing source; they don't replace tabs or spaces with symbols.

An HTML "Indentation only" label means the nonblank text agrees exactly after
removing leading spaces and tabs. Internal whitespace, trailing whitespace,
and the final-newline state must also agree. Leading tabs and spaces can
differ even when they occupy the same display width: a tab and eight spaces
can receive this label while remaining different source bytes.

A `No newline at end of file` note follows a source line whose final line
terminator is missing. It doesn't mean an extra blank line should be added.
Lines that differ only in this final-newline state aren't equal.

## Use terminal output

### Width and wrapping

On a terminal, source columns can grow to fit the available width. Use
`--max-column-width=N` to cap each source column at N display columns,
excluding the signs, numbers, and separators. Redirected output uses a
default cap of 100, as does a terminal whose width cannot be read. Both
columns use the same width throughout the report, measured from the source;
longer paths, labels, and notes wrap without widening it.

TTY output expands source tabs to spaces, using tab stops measured from each
source line's start. `--tab-width=N` accepts a positive integer and defaults
to eight; `--tab-width=4`, for example, uses four-column stops. This gives
identical source the same indentation in both columns, regardless of the
signs and line numbers before it.

Source columns have a minimum width of two. Redirected text keeps literal
tabs and needs at least eight columns when tabs occur, even if a smaller cap
is requested. A very narrow terminal may be too small for the two-column
layout.

Long source lines wrap without repeating their signs or line numbers on
continuation lines. UTF-8 characters remain intact, and other nonprinting
bytes are escaped as `\xNN`. Borders use box-drawing characters in an effective
UTF-8 locale and ASCII otherwise; in a single-byte locale, the program first
tries `C.UTF-8`.

### Saving and copying

Redirected text keeps literal source tabs. Its layout assumes eight-column
terminal stops measured from the whole output row, so gutters and column
positions affect their displayed width if you later print that file with
`cat`. `--tab-width` affects TTY rendering and HTML display; it doesn't expand
tabs in saved text. Trailing source whitespace is preserved, and layout
padding doesn't add trailing spaces.

Use `-o FILE` or `--output=FILE` to save the report. Output otherwise goes to
standard output, and `-o -` explicitly selects it. Terminal color is automatic
and is disabled when output is redirected. To retain color in saved output,
use `--color=always`; use `--color=never` for plain text.

Tab expansion and terminal width detection follow the report's actual
destination. Saving with `-o report.txt` keeps literal tabs even when standard
output is a terminal; writing to a terminal device with `-o` uses the terminal
layout.

TTY output contains expanded spaces, and terminal selection cannot recover
the original tabs. Even when printing a saved file containing tabs, terminals
such as Konsole can copy the resulting screen cells as spaces. To retain
tabs, read saved text output from a file, select source in HTML, or use the
HTML report's Copy report and Save text controls. Saving a report includes its
numbers and layout; it doesn't produce a replacement source file.

## Use a standalone HTML report

Generate an HTML version of the same sample:

```sh
./diffofdiffs --html --theme=light --backport-labels -o report.html \
    samples/arguments/backport.patch samples/arguments/upstream.patch
```

Open `report.html` directly in a browser. This single file embeds its styles,
scripts, and viewing controls, so it needs no server or network connection.
The HTML and text reports use the same paired source rows. For Git inputs,
HTML also shows each result commit's subject beneath its identifier and in
the browser tab title.

### Viewing controls

The seven controls under **Viewing options** change the presentation:

| Control | Choices or effect |
| --- | --- |
| Layout | Review or Console |
| Theme | Dark or Light |
| Highlight | Words, Exact characters, or Off |
| Yellow shared edits | Toggle yellow shading on shared additions and removals |
| Mark gaps | Toggle hatching where no source line is present |
| Wrap lines | Toggle wrapping of long source lines |
| Text size | Small, Medium, or Large |

Review uses rounded panels and a reading layout; Console uses the full page
width, square corners, and monospace headings. Reports start in Review with
the Dark theme and Words highlighting. Use `--theme=dark|light` and
`--highlight` to choose the initial theme and highlighting, or change them
in the viewer. Turning off shading or hatching doesn't change which edits
are shared or which cells have no source.

### Navigation and copying

Use **Show** to select both report sections or just delta or context
differences. **File** jumps to a file, and **Previous** and **Next** move
between difference groups. Tooltips explain line numbers, gaps, shared edits,
and whitespace; the legends explain edit signs.

**Copy report** and **Save text** include the complete plain-text report,
including sections hidden by **Show**. Source selection and these controls
preserve literal tabs and spaces. All source remains visible without
JavaScript; the viewing controls require it.

## Understand the limits

diffofdiffs compares source and original edits without interpreting what the
code does. Text similarity and indentation can help align rows for reading,
but they don't make unequal source into a shared edit. Highlighting decorates
the selected pairs; it doesn't establish that two changes mean the same thing.
Unrelated pairs and very long lines can appear without inline highlights.

Files with different basenames aren't paired, though differing leading
directories can still match. Identical edits can be shared across separate
files, so a change of path alone doesn't guarantee a report. When the edits
also differ, unpaired files appear in separate sections with an empty side.

Repeated or reordered source and missing patch quotations can leave
identical edits unpaired and visible. Ambiguous overlapping or conflicting
quotations suppress a file's context comparison; its original edits remain
available for delta comparison. Tree mode supplies more source evidence, but
complete source doesn't remove every ambiguity.

A successful comparison returns exit status 0 even when it finds differences,
so status alone isn't a changed-or-unchanged test. When no differences are
found, both text and HTML output are empty. Check for both empty output and
status 0 to distinguish that result from an error, which returns a nonzero
status.

Use the report to inspect the edits and their surroundings. Deciding whether
a backport is correct still requires reviewing what those changes do.
