<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Code design decisions

Follow [CODING_STYLE.md](CODING_STYLE.md) on every change. The refinements
below record the review decisions behind the implementation and style.

## Comments and spacing

Comments explain constraints, ownership, ordering, or reasoning that is
not apparent from the code. Add useful explanation throughout the code,
but omit narration of an obvious assignment, condition, or call. Follow
CODING_STYLE.md for comment prose and commit messages.

Separate a standalone comment from preceding code in the same scope with
a blank line. Do not insert a blank line before a comment that is the first
content after an opening brace. Keep an SPDX tag adjacent to the following
copyright or overview comment. A comment at the start of a file needs no
leading blank line.

Treat clang-format off/on directives as a pair, like lock/unlock. Do not
insert a blank line immediately before the closing directive.

A one-line comment containing one sentence has no final period. A one-line
comment containing multiple sentences ends with a period, as does every
multi-line prose comment.

Use the Oxford comma in project-authored prose lists of three or more items.
This includes documentation, comments, and test descriptions. Verbatim
quotations, source fixtures, and third-party text retain their original
punctuation.

## Control flow and declarations

Put declarations before statements in each scope. Only for-loop initializers
may introduce variables after statements. Moving a declaration must not move
an allocation or computation ahead of the checks that make it valid.

A body that contains another control statement takes braces, even if that
nested statement fits on one line. A loop body containing a simple
statement that spans multiple lines also takes braces.

An if that guards one simple statement in total stays unbraced even if the
condition or statement wraps. This exception does not cover an if whose
body is itself a loop or another if. If any branch in an if/else chain needs
braces, keep braces on every branch.

Use for loops for counted iteration and scans. Put advancement in the
header when possible, and declare the iterator in the initializer when
only the loop needs it. Keep an iterator outside the loop if its final
value is used afterward, or if it is a parameter. This includes scans such
as skip_to_blank(), and loop-local iterators in stripped() and
scan_mode_line(). Cursor exhaustion and convergence loops may remain while
loops when there is no useful counted iteration to express.

Mark unused parameters with __unused on the parameter declaration. Do not
add meaningless (void)parameter statements to silence compiler warnings.

Keep ternary expressions out of if conditions. Use explicit Boolean
conditions or compute a selected value before testing it. Keep return
statements visible, including ownership transfers through no_free_ptr().
Macros must not hide a function's return statement.

Assign compound literals using typeof(lval), rather than repeating the
declared type. For array elements, typeof(array[0]) expresses the element
type without repeating a changing index. Use Boolean values and comparisons
directly instead of redundant condition ? 1 : 0 conversions. When an
integer or pointer needs normalization to zero or one, use !!value.

Avoid four nested loops that force the useful work into heavily wrapped
code. Extract a coherent operation into a helper, as with measuring a
source row and section, or restructure the iteration.

Review control flow, function complexity, nesting, and repeated logic along
with comment coverage. Separate distinct decisions and group related state
when that makes the dependencies easier to follow. Comments explain the
remaining constraints; they do not substitute for simplifying avoidable
complexity. Extract helpers around coherent operations, not arbitrary line
counts, and preserve ordering and failure behavior when moving code.

## Names, constants, and tables

Function names describe their operation and object. Prefer names such as
display_character_bytes(), render_line_gutter(), and compare_images()
over character(), gutter(), and compare(). The shared byte duplication helper
is named memdup(); it copies the full byte range, preserves embedded NULs, and
appends a trailing NUL.

Name return values and output parameters by their units. Character decoding
returns bytes consumed and optionally writes terminal columns through a
nullable pointer. Always initialize a supplied output, including on failure.
Use TAB_WIDTH for the default eight-column tab stops. The --tab-width option
controls TTY and HTML presentation; it must not change the comparison engine's
source equality, indentation evidence, or edit ownership.

Define ANSI escape sequences as named macros. Keep each explicitly
designated index in unicode_glyphs and ascii_glyphs on its own line.

CODING_STYLE.md forbids trailing commas in non-enum initializers. Keep table
entries on separate lines, and use designated fields when positional values
obscure their meaning, as with old_side, created, and deleted in mode_words.
Use a narrowly scoped clang-format off/on pair when the formatter would
otherwise collapse a readable table. Keep the closing directive adjacent.

## Correspondence and presentation

Deterministic matching alone does not establish correct correspondence.
Keep exact source equality separate from display correspondence. Full tree
images supply more evidence, but repeated text can still be ambiguous;
preserve visible differences when correspondence cannot be established.

Indentation correspondence uses libgit2's patience comparison on bodies
with only leading spaces and tabs removed. A candidate run needs a consistent,
nonzero indentation shift and two lines containing words whose bodies are
unique in both source windows. Existing exact matches with unique bodies
remain ordering boundaries. Unknown patch gaps break runs, and the existing
paired function regions bound complete C-source comparisons. This adds no
control-statement or branch parser.

Accepted runs can displace conflicting repeated braces and other weak
matches. Their original bytes, native signs, and final-newline status stay
intact. Only exact original bytes can establish shared edits; sharing a
blank through reindented neighbors requires support on both sides. Display
correspondence can also bound a known context excerpt without asserting
equality. An unknown gap outside that excerpt must neither hide the known
differences nor justify marking unanchored edge text as different.

Indentation highlighting preserves the underlying source text and edit
identity. It is now implemented as a presentation feature for CLI and HTML
output, separately from the source correspondence decisions.

## HTML presentation

Keep Review and Console treatments, yellow shared additions/removals, gap
hatching for positions without a source line, and helpful tooltips. Preserve
source text, coordinates, signs, ordering, and classifications in every view.
Presentation galleries should offer a patch selector on every comparison
page so the same options can be evaluated on different inputs. Include
theme and section controls, wrapping, text sizes, source copying, and
file/difference navigation.

Gap hatching must remain aligned across adjacent rows rather than restarting
on every row.
The viewer must offer theme selection and controls to toggle or change these
presentation choices independently.

Use GitHub's two-level diff treatment for inline emphasis: readable neutral
source text, muted full-line addition/removal backgrounds, and stronger
red/green backgrounds behind changed spans. Enabling highlights must not
fade the line backgrounds or source foreground. Keep whole-word emphasis
as the default, with exact-character detail available separately.

Inline red denotes text on the left, and inline green denotes text on the
right within an aligned pair, independently of each patch's original + or -
sign. Explain that distinction in the legend and tooltips. Yellow shared line
shading retains its existing meaning. Both themes follow GitHub's two-level
diff treatment. Syntax coloring is separate from diff ranges.

Saved viewing preferences and configuration files are deferred.

Indentation-only treatment requires exact source equality after stripping
only leading spaces and tabs, with nonblank text remaining. Internal and
trailing whitespace are significant. Adjacent qualifying rows in a hunk
share a label only when their display-column shift is the same. These
features decorate existing correspondences without semantic parsing or
changing edit identity. Leading whitespace remains present and copyable.

## Native highlighting and HTML

CLI colors use standard ANSI palette entries and the terminal's default
foreground/background. Do not inspect emulator configs or hardcode RGB
values for terminal output. Additions use green text and removals use red
text. Changed words use bright red on the left and bright green on the right,
without bold or forcing the default foreground. Emphasize changed horizontal
whitespace only when it touches unchanged horizontal whitespace on the same
source line. Use cyan for eligible whitespace in both indentation and the
source body, and underline its spaces. Establish adjacency before wrapping;
wrapping must neither create nor hide eligibility. Never underline code
characters: underlines can merge with underscores, and a bold default
foreground can overwhelm the source colors. Avoid reverse video and colored
backgrounds, whose contrast depends on the terminal palette. Reset inline
emphasis before restoring the row color so it cannot bleed into the remaining
source or padding. HTML keeps its own themes, and --theme selects only the
initial HTML theme.

Center each CLI operand title within its complete side column, including
the line-number gutter. Measure display columns rather than bytes, preserve
wrapping for narrow columns, and omit trailing spaces. Titles name the input
kind and role, e.g. Backport commit: 0123456789ab or Patch 1: fix.patch.
Print the titles once at the top of the report, between horizontal borders
that distinguish them from the invoking command. Do not repeat them for each
file or report section.

The presentation options are
--html, -o/--output=FILE, --highlight=words|characters|none, and
--theme=dark|light. Built-in defaults are text, words, and dark. ANSI emphasis
requires color output. Disabling color preserves the existing plain report
byte for byte.

Both renderers share source escaping and a single native range generator.
It uses the existing libgit2 dependency through udiff_match(): first compare
word, whitespace, and punctuation tokens, then refine unmatched runs by
Unicode code point. Character refinement stays between matched words so it
cannot borrow punctuation from another argument or statement. Emphasis is
suppressed on unrelated pairs and bounded at 8,192 combined escaped bytes.
These are presentation decisions, without semantic parsing or changes to
row pairing. This is not a claim about GitHub.com's exact private algorithm.

Replacement-row alignment precedes highlighting. Within a fully known gap
between exact source matches, compare identifier bytes through libgit2 and
require at least 75% of the longer line's identifier bytes to survive. Only
mutual unique best matches can displace positional pairing. Ties retain the
existing pairing, and crossing candidates leave the entire gap unchanged.
Unknown source, oversized gaps, and previously rejected byte-identical
matches cannot provide new anchors. This changes display pairing only;
source equality, edit ownership, shared markings, and per-side row order
remain unchanged. It does not parse control-flow syntax or establish edit
identity from text similarity.

Raw source slices retain their final-newline status even though display
strings omit the line terminator. An indentation-only label requires exact,
nonblank body equality including that status. Leading tabs and spaces remain
distinguishable even when they occupy the same display columns. Internal and
trailing whitespace remain significant.

For TTY text output, expand source tabs to spaces using --tab-width, with
eight columns by default. Measure tab stops from the source line's start so
identical source has identical indentation in both panes. Determine TTY
status from the actual output stream, including a destination selected by -o.
Keep original source bytes available for matching and highlighting; map
highlight ranges to the expanded display without changing their ownership.
Wrapping must preserve the expanded line, including when a tab's spaces
cross a wrapping boundary.

Preserve literal source tabs in redirected CLI output, HTML text nodes,
browser selection, and copy/download payloads. Escape other nonprinting
source bytes as before. Terminal selection is a separate contract: Konsole
interprets HT as cursor movement and copies screen cells as spaces. Never
claim terminal selection preserves tabs based on byte-stream or HTML tests.
Test these properties separately; use HTML selection or saved output for
exact bytes. Measure display width separately from byte offsets, and never
trim trailing source whitespace.

Keep CLI number gutters compact: each numeric field fits the largest line
number, with a three-column minimum and one space between the fields. Tabs
must never add padding before or between these fields. Keep the edit sign
before the numbers and the vertical separator after them. Do not round gutter
or pane widths to align source origins with tab stops. Redirected text keeps
the existing eight-column terminal stops, measured from the start of the
whole output row. Its source columns need at least eight cells when tabs
occur, and wrapping keeps each tab intact at the fragment's output position.
TTY output uses the expanded text and needs only the two-cell minimum for
wide characters.

By default, allow source columns to grow up to the available TTY width.
--max-column-width remains a long-only option and imposes an explicit cap
when supplied. Keep a 100-column fallback for redirected text or unavailable
terminal dimensions. Short reports retain their natural width. Paths and
labels must not widen source columns, and the cap excludes number gutters.

HTML uses --tab-width for tab-size and the existing red/green changed-span
palette for indentation, without a separate gray or blue fill. A byte-identical
leading prefix stays unhighlighted; tabs and spaces that occupy the same width
still receive distinct ranges. Use GitHub-like color blocks for highlighted
whitespace, without dots or arrows. Every highlighted space gets its own
color block; tabs fill their tab stops. Blocks have a slight corner taper
and wide fills, with 0.75-pixel space insets, 1.5-pixel gaps between spaces,
and a one-pixel corner radius. Tab insets are 2.25 pixels, keeping a
three-pixel gap beside spaces. Keep these insets uniform within their
monospace cells. Round the source character advance to a whole CSS pixel so
the blocks do not alternate in width or spacing at the default display
scale. Leave a wider gap at tab boundaries so adjacent space and tab blocks
remain distinct. Give word blocks the same one-pixel corner radius and
0.75-pixel insets as spaces: the word/space gap must equal the space/space
gap, with rounded corners on both ends of each block. Paint each text run
separately from its neighboring whitespace. Character refinement may split
a word into several spans; join their fills in word mode without introducing
internal gaps or rounded corners.

Use background-only painting with fixed-size SVG corners and solid strips
for smooth, uniform corners at high zoom. Keep the corner colors consistent
with each theme's changed-text palette. Avoid shadows, which can leave a halo
outside the fill. A single SVG rectangle without a viewBox scales its corner
radius differently between browsers at high zoom. Padding balanced by negative
margins still disrupts Firefox tab stops and text shaping across combining
characters. Keep paint spans in normal inline flow without padding or margins:
even an empty positioned child interrupts hanging whitespace and changes
line breaks with pre-wrap. This also covers pure-space edits,
whose continuous fills could resemble tabs. Apply the same rule to spaces
beside unchanged tabs, trailing whitespace, whitespace-only lines, and tabs
that occupy just one display column. Paint separators without adding source
characters or changing native tab handling, line-breaking rules, selection,
or copy. Keep this in the renderer; matching and highlight ranges do not
need whitespace-type classification. Hide these decorations with highlighting
disabled. Keep the indentation-only label and show tabs/spaces changed when
there is no change in display width.

Git-backed HTML reports show both result commits' subjects below their
identifiers and in the document title. Read the already resolved result
commits, including annotated-tag and BASE..TIP operands; do not use tag
annotations or parent subjects. Fold only the first message paragraph into
the subject, escape it as label text, and show a clear fallback for an empty
subject. Keep metadata out of source cells and plain-report copy payloads.

The standalone HTML embeds CSS, controls, and JavaScript at build time with
a small Python standard-library script. Reports need no external assets,
network access, Node, or runtime highlighting library. Source stays visible
without JavaScript; controls appear when the script runs. The native viewer
retains Review/Console, themes, word/character/off modes, yellow shared
edits, continuous gap hatching, tooltips, section/file filters, difference
navigation, wrapping, font sizes, and complete plain-report copy/download.
Keep initial settings consistent between the HTML attributes, CSS, and
controls, including when JavaScript is unavailable. Changing a viewing
control affects only the open report.

Replacement alignment must keep corresponding calls together when one patch
also inserts a nearby statement. For example, adding preempt_disable_nested()
before write_seqcount_begin() must not prevent a changed argument in the
latter call from receiving word and character highlights. Validate both the
chosen row pairs and their source accounting, including per-side order.
