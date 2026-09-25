<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Coding style

This project follows the Linux kernel's coding style, with its
[.clang-format at 99df2a8eba34](https://github.com/torvalds/linux/blob/99df2a8eba34/.clang-format)
as the machine-readable base. The local file normalizes the SPDX identifier
to GPL-2.0-only and makes three formatting changes.
AllowShortEnumsOnASingleLine is set to false so short
enums keep one enumerator per line, AlignEscapedNewlines is set to
DontAlign so a macro's continuation backslashes stay where its lines end
instead of padding out to the group's widest member, which pushed lines past
80 columns. ForEachMacros carries liburcu's cds_list_for_each* names so the
formatter lays those loops out as loops rather than as call expressions.
clang-format 11 or newer understands the whole file; conformance is defined
by the output of clang-format 22.1.8, the
version this tree is formatted with, since a formatting fixed point is a
property of one binary.
Re-record the version here if the tree is ever reformatted under a newer
one.

Everything the kernel's Documentation/process/coding-style.rst says applies
here unless this document says otherwise: tabs are 8 columns wide, function
braces open on their own line while control-statement braces stay on the same
line, single-statement bodies go unbraced, continuation lines align after the
open paren, and case labels sit flush with their switch.

## Line discipline

Lines are limited to 80 columns and fill up to 80: break a line only where
the limit demands it, since wrapping early is as wrong as overflowing. The
one exception is text strings: a quoted string that would cross 80 columns
stays whole on its line so the message can be grepped.

## Braces

A control body that is itself a control statement takes braces even when
it fits on one line. A loop's simple statement takes braces when it spans
more than one line; a simple statement that fits on one line stays bare.
An if guarding one simple statement stays bare even when that statement
wraps across lines. This exception covers one statement in total: an if
guarding another control statement still takes braces.

When any branch of an if/else chain needs braces, every branch of that
chain takes them, so the chain reads uniformly from top to bottom.

## Returns breathe

A return that other statements follow takes a blank line after it, case
labels included; a return whose next line closes a brace sits flush. A
loop's `break;` and `continue;` breathe the same way when statements
follow them; a switch's `break;` is exempt. A die() or edie() call is a
return that never comes back, so it breathes the same way when statements
follow it.

## Ternaries

A ternary whose branch repeats a computed expression from its own
condition spells the computation twice; write the clamp as MIN()/MAX()
from <sys/param.h> instead. A branch that just rereads a single variable
stays a ternary.

Use booleans and comparisons directly instead of converting them through
condition ? 1 : 0. When a value needs normalization to zero or one, use
!!value.

## Declaration order

Declare locals before statements in their scope. The only exception is a
declaration in a for-loop initializer. Keep guarded allocations and dependent
computations after their checks by separating the declaration from its
assignment; cleanup-managed pointers start at NULL until assigned.

Local declarations stack in reverse x-mas tree order: within each
declaration block, the longest declaration line sits at the top and the
shortest at the bottom, with equal lengths keeping their relative order.
An initializer that names an earlier local is a hard bound the sort never
crosses; a block a bound pins takes the longest-first order the bound
allows, greedy by longest available declaration. A comment documenting a
declaration rides that declaration wherever the sort moves it.

Use for loops for counted iteration and scans, with advancement in the
header where possible. Declare an iterator in the for initializer when
only the loop needs it. If the iterator is a parameter or its final value
is used after the loop, keep it available there while using the for header
for its advancement.

Mark unused parameters with __unused in their declarations. Do not silence
warnings with a standalone (void)parameter statement.

## Assign, then check

A comparison that tests a value the immediately preceding statement
assigned puts that value first: the fresh assignment is the operand under
test, and the yardstick it's measured against comes second.

An assignment that gets error-checked at the top of a scope moves out of
its declaration: the declaration goes bare, and the assignment sits right
above the if that checks it, as the scope's first statement. The rule
covers checks on what the computation produced (a failure, an absence, a
sentinel, a value out of bounds); a condition that just uses the fresh
value in the scope's own logic leaves the initializer in the declaration.

## Structs

Functions never return a struct by value: a function that produces a struct
fills one the caller passes by pointer instead.

Struct and union definitions declare one member per line. Function-local
variable declarations are free to group several names on one line.

## Object clears

A struct clears by compound-literal assignment, never by memset, when its
padding is free: nothing may compare, hash, or write the object's raw
bytes, since the assignment leaves padding unspecified. Arrays and nonzero
fill patterns stay with memset.

Sizes and types are spelled through the variable (sizeof(*p), typeof(*p)),
never through the type name, so a declaration's change never leaves a
stale spelling behind.

A clear followed at once by stores into the object's members is one
designated-initializer compound literal, not a clear and a chain of
assignments; stores that only sometimes happen stay conditional code.

A struct embedding a list head clears the same way, with the head's init
riding in the literal as `.list = CDS_LIST_HEAD_INIT(var.list)`: a zeroed
head isn't an empty list, since an empty list points at itself. A
declaration's initializer takes the same form.

An object on its way out isn't zeroed: once nothing reads it again, a
clear after the frees is dead code. A reset a later flow really reads
(an emptied out-parameter, a rebuilt cache) is a real store, not a
farewell wipe.

## Array initializers

A designated array initializer takes one entry per line, and no
initializer, enum, or struct literal carries a comma after its last
entry. No compiler warning exists for the trailing comma (measured on
gcc 16.2.1 and clang 22.1.8), so checkstyle bans it textually.

## Integer types

Plain char is unsigned: every build variant compiles with -funsigned-char,
and the type `unsigned char` never appears in the tree. A byte that IS a
number rather than text (a digest byte, a checksum block, a field
serialized bytewise) is `u8`, so its meaning never rides on a compiler
flag; a byte that carries text, a flag, or a small state code is plain
`char`, even where a hash fold or a ctype call consumes it, since the
unsigned build makes those reads well-defined.

Fixed-width integers use the kernel names: u8, u32, u64, and s64 live in
src/include/types.h, each defined over its <stdint.h> counterpart so the
name is a pure rename and never a re-width. The long stdint spellings
appear only inside types.h itself. size_t, ssize_t, and ptrdiff_t are the correct C
spellings for sizes and differences and stay as they are.

## Booleans

A truth value is bool: a function whose every return answers yes or no,
and any local, parameter, global, or struct field that only ever holds
such an answer. The build uses GNU C17 (-std=gnu17), so include <stdbool.h>
for bool, true, and false. int stays for counts, scores, statuses, and
main's exit, and for a selector that picks a side rather than asserting
a truth.

## Includes

Project headers live in src/include/ and are included in the angle form,
`#include <file.h>`, resolved by the -Isrc/include on the compile line;
the quote form is never used, so no include depends on the including
file's own directory. -Isrc/include places the project header basenames
ahead of the system search path for angle includes: nothing collides with
a libc header today, so keep project header names specific enough that
they never do.

Bundled headers live in vendor/, with -Ivendor resolving includes such as
`<urcu/list.h>`. Both include paths precede the user's flags in CPPFLAGS so
other dependency prefixes can't replace the pinned headers.

## Casts

A cast earns its place by changing something: pinning a varargs argument's
width, truncating or re-signing a value on purpose, stripping a qualifier,
re-typing a pointer, or typing a macro constant. Never write a cast that
restates a conversion its context already performs (an assignment, an
argument matched against a prototype, a return statement); such casts are
noise and get deleted on sight.

Pinning a varargs argument's width covers types the default argument
promotions leave alone; a bool argument already arrives as an int under
those promotions, so an (int) cast on it pins nothing.

## Comments

Comments are /* */ block comments. The only // in the tree is the SPDX tag
that opens every .c file, per the kernel's own convention:

	// SPDX-License-Identifier: GPL-2.0-only

Headers take the /* */ form of the same tag.

Leave a blank line above standalone comments when code precedes them in
the same scope. A comment that opens a scope sits flush against its opening
brace, with no intervening blank line. File-opening comments need no blank
line above them either. Keep the SPDX tag adjacent to the copyright or
overview comment that follows it, with no blank line between them.
Treat clang-format off/on comments as paired directives: do not add a blank
line before the closing directive.
Comments explain constraints, ownership, ordering, or other reasoning the
code doesn't make apparent. Omit comments that merely restate the code.

Multi-line comments are kernel-true: the /* opener stands alone on its line,
every interior line leads with an aligned asterisk, and the */ closer stands
alone on the trailer line. Comment prose fills to 80 columns like code does:
breaking while the next word still fits the line is wrapping early, which is
as wrong here as it is in code. A multi-line comment ends in a period.
A single-line comment has no final period unless it contains more than
one sentence. Sentence periods take a single trailing space, never two.
Use the Oxford comma in all prose lists of three or more items, whether
the final conjunction is "and" or "or". Preserve punctuation in verbatim
quotations, source fixtures, and third-party text.

Structure breaks where it breaks: a new list item takes its own line no
matter how much room remains above it, an item's continuation lines keep
their hanging indent, and a header's Copyright and Author lines stay one
fact per line with no terminal period, since a credit is not a sentence.
The checkstyle gate enforces the fill and the period on everything else.

Kernel-doc isn't used for functions, structs, or parameters; a plain /*
comment explains what needs explaining. A file-level /** DOC: overview block
describing a whole subsystem is fine.

Comments carry present-tense rationale: what the code does now and why, never
the history of how it got that way (that story belongs in commit messages).
Byte sizes use binary units: KiB, MiB, GiB, never KB, MB, GB. All prose is
plain ASCII with no unicode punctuation and no em dashes, including `--` used
as one. Never describe anything as "load-bearing" or use "shape" as a noun;
say specifically what depends on what.

Worst-case reasoning is written as what it is: a pathological input, a cost
bound, the work a run can force. Never frame it as an adversary, an attack,
or a hostile or malicious input. This program compares two versions of a
patch and has no security dimension, so that vocabulary misdescribes the
code and sends the next reader looking for a threat model that was never
there.

## The program is not a library

All of this code serves one program, no matter how thematically the files
split it up: nothing here is a shared library, so code is written only for
the logic this program needs, never for a hypothetical outside caller. A
check for a condition no in-tree caller can produce (the test-support
harnesses count as callers) is dead code, and worse: it teaches the reader
that the condition is possible, sending them off to sleuth the codebase
only to find the check can never fire. Don't write one.

## Attribution

For project-authored files, the head-of-file block credits only the people
who worked on the file itself.
Code carried verbatim from another project keeps that project's copyright
notice beside the code it covers, since the copyright genuinely holds over
verbatim code. Ideas taken from another project take a citation where
they're used and nothing more. Snippets of ten lines or fewer don't
constitute copying in practice unless they're really unique algorithmic
code.

Vendored files under vendor/ keep their upstream copyright notices, license
text, and formatting. Don't apply the project's style rules to copied code.
Record the upstream revision and any local changes beside the vendored files.

## Failure reports

Failure reports stay lean. A refusal or an error names what the user can
recognize (an operand file, a CLI option, an image side) and prints the
numeric operands that tripped the check; it never names internal allocation
subjects, since anyone chasing a failed invariant reads the coredump and
backtrace regardless, and a subject string for an internal buffer is plumbing
without information.

A one-line message is one string literal, however far past 80 columns it
runs: the text-string exception exists exactly so a message can be grepped
whole, and splitting one across concatenated fragments defeats that.

A fatal report goes through die(), or edie() when an errno belongs in it;
both fold EXIT_FAILURE in. Raw error() survives only where the exit
status differs, like the driver's usage faces, and in the wrappers' own
interiors.

## Allocation

Allocating libc calls go through the x-layer in src/util.c (xmalloc,
xzalloc, xrealloc, their _array forms, xstrdup, memdup, xasprintf,
xgetline, xopen_memstream, xfmemopen): failure is fatal inside the
wrapper, so no call site carries a NULL branch of its own, and checkstyle
greps for raw allocator calls outside util.c. A zeroed allocation goes
through xzalloc(), never an xmalloc() followed by a whole-buffer
memset(): the pair does in two steps what one call does and spells the
same size twice. An allocation sized nmemb elements by size bytes goes
through xmalloc_array(), xzalloc_array(), or xrealloc_array(), which
check the multiply and die on an overflowed product, so no call site
multiplies raw or spells its own bound. The boundary is deliberate and closed: free() stays plain
since it cannot fail, so a wrapper would be a rename; the one fopen()
keeps its inline error() because it names the operand path a generic
wrapper would drop; qsort() allocates nothing on the project's glibc
floor; and the printf family's failures aren't allocation failures. The
grep can't see a raw allocator named inside a string literal or in a
comment sharing a line with code; neither exists in the tree, so the gate
stays simple instead of engineering for them.

An array's element count is ARRAY_SIZE() (util.h), never a hand-spelled
sizeof division; the macro refuses a pointer operand at compile time.

## Teardown naming

A teardown helper is named <X>_free, where X is the exact struct tag (or
exact type name) of what it tears down: moved_side_free() frees a struct
moved_side. X is never an abbreviation, since exactness is what keeps two
helpers from colliding or trading meanings; each carries its own type's
name. An array destructor takes the singular element tag, and a list
destructor takes its node type's tag, because the nodes are what it
frees.

An X_free() is safe on a zeroed X when X holds only counts and pointers:
count-driven walks see zero counts and pointer frees see NULL. An embedded
list head is the exception, since a zeroed head isn't an empty list (see
Object clears above), so an X carrying one is safe to free only once its
head has been initialized. A helper whose type carries one member a plain
free can't take (an offset buffer, say) guards that member itself rather
than pushing the burden onto callers.

An owning struct takes ONE destructor, covering everything the object
still owes a release at rest. A non-owning view of the same type nulls
the members it merely borrows and calls that one destructor, whose
frees then no-op over the nulls, rather than growing a sibling helper
that frees a different subset; two destructors for one type is how the
subsets drift apart. Members another owner adopts mid-flight sit
outside the destructor's domain entirely, so a transferred member is
never nulled just to dodge it.

checkstyle enforces the shape half of the naming rule: a function
definition whose name carries the token `free` without ending in _free,
or any of the teardown verbs destroy, release, clear, cleanup, or
dispose in any position, fails the gate. Whether X names the right type
is review's job; no grep can see types.

## Scope-exit cleanup

A transient buffer that lives and dies inside one function declares its
release at its declaration: `char *buf __free(free) = NULL;` runs
free(buf) on every exit from the scope, so no return path can leak the
buffer and the tails carry no free ladders. The macros are the kernel's
cleanup.h idiom re-dressed over glibc in util.h; DEFINE_FREE() mints a
hook per free routine, and the one shipped instance covers plain
buffers.

A converted declaration always carries an initializer, since the hook
runs on whatever the variable holds when its scope exits, including an
exit taken before the first assignment. Hooks run in reverse
declaration order, so a value that must outlive another declares first.
A function that hands the buffer onward instead of freeing it moves the
value out through no_free_ptr(), which nulls the local so the hook's free
no-ops. The compiler rejects a dropped no_free_ptr() result, so a transfer
can't quietly become a leak.

Keep return statements visible: write `return no_free_ptr(value);` at an
ownership transfer. Do not put a function's return statement inside a macro.

## Checked arithmetic

Validate a caller's numbers once, at the interface boundary, and fail early;
interior arithmetic on values already bounded there (by a per-image ceiling
or by the process-wide memory limit) runs plain, with the bound stated in a
comment where it isn't obvious from the surrounding lines. Never nest
checked-helper calls into chains that re-prove the same bound at every step.

## The formatter is a gate

`make checkstyle` defines conformance, and check and check-asan refuse to run
until it passes. Its formatter half runs clang-format --dry-run -Werror over
src/*.c, src/include/*.h, and tests/support/*.c; where the formatter's
output and this document disagree, the formatter's output is the tree's
definition of conformance, even when it wraps a line earlier than the
80-fill rule prefers. Comment reflowing is disabled in the config on
purpose; comments are wrapped by hand.

checkstyle's grep half catches what the formatter can't see: `unsigned
char`, the long stdint spellings outside src/include/types.h, compact
multi-line comment openers, the banned comment vocabulary, and lines past
80 columns that carry no string (tabs expanded at 8). When clang-format
itself is absent, the formatter half skips with a warning instead of
failing the suite, so the correctness tests never hinge on a formatter
install.

Casts, struct-member layout, checked-arithmetic scope, and the
failure-report subject rule are review-enforced: no machine check can judge
intent, so those rules bind authors and reviewers rather than the gate.

## Commit messages

Commits are separated thematically: one topic per commit, never a grab bag.
The subject line is "prefix: Summary" with a capitalized summary, no trailing
period, and at most 75 characters. A blank line follows, then body prose that
fills 75 columns and wraps at 75. Trailer lines and quoted machine output
(compiler diagnostics, kernel logs, command transcripts) are exempt from
wrapping: paste them verbatim, with quoted output indented two spaces.
Commit-message prose follows the same ASCII, punctuation, and terminology
rules as comments. Co-authored-by trailers use exactly that casing.
