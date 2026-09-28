<!-- SPDX-License-Identifier: Apache-2.0 -->

# Licenses and third-party material

The diffofdiffs application, build scripts, test runners, documentation, and
synthetic examples in `samples/` are licensed under the
[Apache License, Version 2.0](LICENSE), except for the material listed below.
The copyright notices in individual files identify their owners. This
licensing change applies to the current source; it does not change the
licenses of earlier revisions or releases.

Paths in this document and NOTICE refer to the source distribution.

## Userspace RCU headers

`vendor/urcu/list.h` and `vendor/urcu/compiler.h` retain their
LGPL-2.1-or-later license and upstream copyright notices. The complete license
is in [vendor/urcu/LICENSE](vendor/urcu/LICENSE). The
[vendoring notes](vendor/urcu/README.md) identify the pinned upstream version
and the extracted compiler-header subset.

The application and test programs use `struct cds_list_head`, the macros
`CDS_LIST_HEAD`, `CDS_INIT_LIST_HEAD`, `cds_list_entry`,
`cds_list_for_each_entry`, `cds_list_for_each_entry_safe`, and
`caa_container_of`, and four inline operations: `cds_list_add`,
`cds_list_add_tail`, `__cds_list_del`, and `cds_list_del`. Each used inline
definition is at most eight source lines, and each macro definition is at
most six.

The executable contains code compiled from these definitions; no liburcu
library or separately compiled liburcu objects are linked. This use relies
on the header-use provisions in section 5 of LGPL-2.1, without relicensing
the headers themselves. Section 5 also refers to section 6 for executables
containing portions of the library. Review that boundary before importing
more of liburcu or changing which header implementations the application
uses. Installed copies include the LGPL text and vendoring notes under
`docdir/vendor/urcu/`.

## HTML colors

The light-theme palette in `src/html.css` uses values from
[GitHub Primer Primitives at f48bc063f7bc](https://github.com/primer/primitives/tree/f48bc063f7bc0fb3e447386a8c259650ce46dea8).
That material is MIT-licensed, copyright (c) 2018 GitHub Inc. Its complete
notice is in [LICENSES/Primer-MIT.txt](LICENSES/Primer-MIT.txt) and the CSS
comment embedded in every generated HTML report. The remaining report code
is Apache-2.0. Each standalone report also includes the complete Apache
license and project NOTICE in an HTML comment, so these stay with distributed
copies. Source code quoted by a report retains its own license; the report
generator does not change that license.

## Formatting configuration

`.clang-format` derives from
[Linux at f5bbbfec59b4](https://github.com/torvalds/linux/blob/f5bbbfec59b4e2fb7520a91de3df8a6174325d6a/.clang-format)
and remains GPL-2.0-only. [CODING_STYLE.md](CODING_STYLE.md) describes the local
formatting changes. The license text is in
[LICENSES/GPL-2.0.txt](LICENSES/GPL-2.0.txt). This development configuration is
not part of the executable or generated reports.

## Test data

Test fixture directories under `tests/`, excluding `tests/support/`, contain
patch inputs, source excerpts, recorded output, and fixture specifications.
The JSON datasets directly under `tests/` also contain source excerpts.
Project-authored material in these data files remains GPL-2.0-only. Third-party
excerpts retain their original licenses, including GPL-2.0-only and
GPL-2.0-or-later Linux source. The RXE example in
`tests/correspondence-trees.json` adapts `drivers/infiniband/sw/rxe/rxe_srq.c`,
whose license is `GPL-2.0 OR Linux-OpenIB`. Small line-number examples in
`tests/fuzzy1/` and `tests/fuzzy2/` also come from patchutils's GPL-2.0-or-later
[tests/fuzz1/run-test](https://github.com/twaugh/patchutils/blob/e664ecc0eb772b67bc893463c737eb3fe64693e3/tests/fuzz1/run-test).
Existing license identifiers inside a patch or recorded report describe
the quoted source, not the test runner.

The GPL version 2 text is in [LICENSES/GPL-2.0.txt](LICENSES/GPL-2.0.txt).
Preserve author, commit, copyright, and license information supplied with the
fixtures. These datasets are inputs for development checks; they are not
compiled into diffofdiffs or installed with it. The programs in
`tests/support/` and the Python and shell scripts directly under `tests/`
are Apache-2.0.

## Patchutils origins

diffofdiffs grew from `interdiff --fuzzy` in
[Sultan Alsawaf's patchutils fork](https://github.com/kerneltoast/patchutils).
The Apache migration replaced inherited reader and utility implementations.
Some names and short strings still follow patchutils or GNU patch:

- Legacy interfaces include `file_list`, `add_to_list`, `stripped`,
  `determine_ignore_components`, and `read_atatline` and its parameter names.
- Header parsing retains the timestamp formats `%Y-%m-%d %H:%M:%S`,
  `%a %b %e %T %Y`, and `%b %Y %H:%M:%S` accepted by patchutils.
- The CLI retains the `usage: ... [OPTIONS] patch1 patch2` syntax,
  `OPTIONS are:`, and `only one input file can come from stdin`. Reader
  diagnostics retain `unexpected end of file in patch` and
  `%s doesn't contain a patch`.

The byte-duplication helper also uses the common allocate, copy, append NUL,
and return sequence. These remaining correspondences are recorded here for
source provenance; the replaced helper bodies are no longer in the
application.

## Linked dependency: libgit2

diffofdiffs links to the separately installed libgit2 library, relying on its
[GPLv2 linking exception](https://github.com/libgit2/libgit2/blob/v1.9.1/COPYING)
for use by an application with a different license. The exception
does not remove the license obligations for libgit2 itself. A distribution
that bundles libgit2 must also satisfy that library's notices and source
requirements. libgit2 is not included in this repository.
