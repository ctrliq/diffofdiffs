<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Sample reports

These self-contained example patches show how diffofdiffs presents changes
made against different versions of a source file. Each HTML report contains
the complete output, including its delta and context sections. Download a
report and open it locally to try the viewing controls.

| Example | What to look for | Report |
| --- | --- | --- |
| Argument changes | Upstream's calls use `record->state.seq` instead of `record->seq`; both patches add the same generation increment | [arguments.html](arguments.html) |
| Indentation | One source nests the added lines; the other uses an early return. A separate addition uses eight spaces on one side and a tab on the other | [indentation.html](indentation.html) |

The inputs are in [arguments/](arguments/) and [indentation/](indentation/).
The reports use patch mode, so source outside the quoted regions remains
unknown. For complete source, compare commits with `--git-tree` as described
in the [user guide](../GUIDE.md).

## Regeneration

Build diffofdiffs, then run these commands from the repository root:

```sh
(
    cd samples/arguments
    ../../diffofdiffs --backport-labels --html --theme=light \
        -o ../arguments.html backport.patch upstream.patch
)
(
    cd samples/indentation
    ../../diffofdiffs --backport-labels --html --theme=dark \
        -o ../indentation.html backport.patch upstream.patch
)
```

To view the same comparison in a terminal, omit `--html`, `--theme`, and `-o`.
Add `--color=always` when saving ANSI output.

The PNG screenshots show each report through the end of its delta section,
with Text size set to Large, a 1440-pixel browser viewport, and a device scale
factor of 1.5. Their themes match the commands above; the remaining controls
use their defaults.
