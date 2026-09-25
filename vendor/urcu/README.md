<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Userspace RCU list headers

diffofdiffs uses Userspace RCU's intrusive lists without any RCU APIs or
library linkage. These headers remove the need to install liburcu to build
diffofdiffs.

The source is [Userspace RCU v0.15.7][release], commit
`bbc6bc94901aebe6380e4e798858b995a8d3fd5d`:

- `list.h` is an unmodified copy of upstream `include/urcu/list.h`
- `compiler.h` keeps the copyright notice, include guard, `<stddef.h>` include,
  and `caa_container_of` definition from upstream `include/urcu/compiler.h`
- `LICENSE` is an unmodified copy of upstream `LICENSES/LGPL-2.1-or-later.txt`

The `compiler.h` subset was extracted on 2026-09-23. Its other helpers and
includes were omitted so that `list.h` doesn't pull in liburcu's generated
configuration header. The retained macro is unchanged, including its argument
type check and single evaluation of the input pointer.
Using other Userspace RCU headers requires an explicit update to the bundled
subset.

Both headers retain their upstream LGPL-2.1-or-later license and copyright
notices. Keep their formatting when updating them. To update, replace `list.h`
and `LICENSE` from a pinned upstream revision, refresh the `compiler.h` subset,
and record the new revision and any local changes here. Run `make check` and
`make check-asan`, and verify a clean build with the system liburcu headers
unavailable.

To check the header dependencies without changing system packages:

```sh
printf '#include <urcu/list.h>\n' | cc -std=gnu17 -Ivendor -M -x c -
```

Run this from the project root. The output should name `vendor/urcu/list.h`
and `vendor/urcu/compiler.h`, with no system urcu headers or `urcu/config.h`.

[release]: https://github.com/urcu/userspace-rcu/tree/bbc6bc94901aebe6380e4e798858b995a8d3fd5d
