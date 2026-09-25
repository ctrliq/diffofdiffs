#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# tests/run-one.sh - execute a single test directory and report PASS/FAIL.
#
# Invocation: run-one.sh <test-dir>. A directory is a test iff it holds a
# file named "spec". The spec is a plain key: value list; blank lines and
# lines opening with # are skipped, and # is never stripped from a value,
# so inline comments don't exist. See the keys handled in parse_spec()
# below. We copy the test dir's
# input files (interpreting any .gen generators) into a fresh arena, run the
# resolved binary there with its stdin, stdout, and stderr all connected
# through pipes, and byte-compare the result against expected.out/err. A spec
# may pin stdout by content hash instead (stdout-sha1), for outputs too large
# to track as files; such a test carries no expected.out at all. A fixture
# with full source files also checks the original input patches against
# independently written file contents, using GNU patch.
#
# We never capture stream data through a shell variable or $(): NUL bytes and
# trailing-newline stripping would corrupt the comparison, so every stream
# stays in a file from the moment it leaves the pipe. The small values we do
# capture with $() below (byte counts, cmp offsets, the arena's mkdtemp path)
# are metadata we generate ourselves, not test I/O, so none of that hazard
# applies to them.
#
# On PASS we remove the arena; on FAIL (or an infra error discovered once the
# arena exists) we leave it in place and print its path, since that's the
# first thing anyone debugging the failure will want.

set -u

# Expected diagnostics include glibc strerror text. Use the C locale so
# translated messages cannot change the recorded output.
LC_ALL=C
export LC_ALL

test_name=""
arena=""

note() {
	printf 'run-one: %s\n' "$*" >&2
}

die() {
	status=$1
	shift
	note "$*"
	if [ -n "$arena" ] && [ -d "$arena" ]; then
		note "arena kept at $arena"
	fi
	exit "$status"
}

report_pass() {
	# The arena dies before the PASS line goes out: a closed or
	# truncated reader (the suite piped into head, or a pager quit
	# early) kills this shell with SIGPIPE at the printf, and an arena
	# removed first can't be stranded by that death.
	rm -rf "$arena"
	printf 'PASS %s\n' "$test_name"
	exit 0
}

report_fail() {
	printf 'FAIL %s\n' "$test_name"
	note "arena kept at $arena"
	if [ -s "$arena/.shellnoise" ]; then
		note "shell noise during the run:"
		cat "$arena/.shellnoise" >&2
	fi
	# The sanitizer runtime prints this one line and stops when another
	# library loads ahead of it. The pattern anchors on the runtime's
	# own ==pid== prefix so a tool merely printing the same words can't
	# draw the note, and the plain lane suppresses it outright: no
	# sanitizer runtime lives there, so the text can only be test
	# output. The note hints at every source that survives the
	# composition's LD_PRELOAD blanking rather than asserting one; the
	# blanking pins the environment the tool starts with, not what the
	# tool re-establishes for itself afterward.
	if [ "${RUN_VARIANT:-}" != plain ] && grep -q \
		"^==[0-9][0-9]*==ASan runtime does not come first" \
		"$arena/actual.err" 2>/dev/null; then
		note "the sanitizer runtime refused startup, since a" \
			"library loaded ahead of it; check the resolved" \
			"binary's own link order, a preload the binary" \
			"re-establishes for itself, and /etc/ld.so.preload" \
			"if this machine has one"
	fi
	exit 1
}

is_uint() {
	case "$1" in
	'' | *[!0-9]*) return 1 ;;
	esac
	# Digits alone don't make the value usable: past the shell's integer
	# range, [ -eq ] and $(( )) blow up instead of comparing. 15 digits
	# keeps every validated value far inside that range, and no
	# legitimate spec value comes anywhere near it.
	[ "${#1}" -le 15 ]
}

ltrim() {
	trimmed=$1
	while [ "${trimmed# }" != "$trimmed" ]; do
		trimmed=${trimmed# }
	done
}

# Print an od excerpt of $1 around 1-indexed byte offset $2, tagged $3, to
# stderr. Used for the divergence diagnostics on stream mismatches.
show_od_excerpt() {
	excerpt_file=$1
	excerpt_off=$2
	excerpt_tag=$3

	excerpt_start=$((excerpt_off - 16))
	if [ "$excerpt_start" -lt 0 ]; then
		excerpt_start=0
	fi
	printf 'od excerpt of %s (%s) near offset %d:\n' \
		"$excerpt_file" "$excerpt_tag" "$excerpt_off" >&2
	od -A d -c -j "$excerpt_start" -N 48 "$excerpt_file" >&2
}

# Compare an actual stream file (always present, possibly empty) against an
# expected file that may not exist at all, in which case the stream must be
# empty. Returns 1 and writes diagnostics to stderr on a mismatch.
check_stream() {
	stream_exp=$1
	stream_act=$2
	stream_label=$3

	if [ -f "$stream_exp" ]; then
		if cmp -s "$stream_exp" "$stream_act"; then
			return 0
		fi
		note "$stream_label mismatch: $stream_exp vs $stream_act"
		cmp -l "$stream_exp" "$stream_act" 2>/dev/null | head >&2
		divergence=$(cmp -l "$stream_exp" "$stream_act" 2>/dev/null |
			awk 'NR==1{print $1}')
		if [ -z "$divergence" ]; then
			# No byte-level difference was reported, so the two
			# streams differ only in length; show the tail end of
			# the shorter one, right where they part ways.
			divergence=$(wc -c <"$stream_exp")
		fi
		show_od_excerpt "$stream_exp" "$divergence" expected
		show_od_excerpt "$stream_act" "$divergence" actual
		return 1
	fi

	if [ -s "$stream_act" ]; then
		note "$stream_label mismatch: expected empty, actual is" \
			"$(wc -c <"$stream_act") bytes ($stream_act)"
		show_od_excerpt "$stream_act" 1 actual
		return 1
	fi

	return 0
}

# Interpret one <input>.gen directive, writing its generated content to
# $arena/<input>. The line format is "repeat <count> <template>" (an awk
# BEGIN loop; the template runs through awk's printf with the zero-based
# iteration index as its argument, so one conversion like %036d numbers the
# lines, and a literal '%' needs doubling to '%%' the same as any printf
# format string), "bytes <count> <byte>" (dd | tr; <byte> is either a
# single character or the literal token NUL for a zero byte, since a real
# NUL can't live in a text gen file), "sparse <count>" (truncate; a
# stat-visible size with no bytes behind it, for an operand whose size is
# the whole test and whose content would be too big to write), "gitstore
# <family>" (the per-variant gitread unit binary's build-store case; a git
# object store can't be a tracked fixture, so it's constructed at
# generation time, exec-free with deterministic ids), or the bare verb
# "dir" (mkdir; a directory can't be a tracked fixture either, so a
# directory operand is declared and generated like any other unstorable
# input).
run_gen() {
	gen_file=$1
	gen_target=$2

	gen_line=$(head -n 1 "$gen_file")

	# A directory operand: the whole line is the verb, with no count or
	# payload, since the only thing to generate is the directory itself.
	if [ "$gen_line" = dir ]; then
		mkdir "$gen_target" ||
			die 2 "dir generator failed for $gen_file"
		return
	fi

	gen_kind=${gen_line%% *}

	# A sparse operand: two fields, since there's no payload to carry.
	# The guard against a bare "sparse" works because ${var#* } hands
	# back the whole string when there's nothing to strip, and is_uint
	# then rejects the verb itself.
	if [ "$gen_kind" = sparse ]; then
		gen_count=${gen_line#* }
		if ! is_uint "$gen_count"; then
			die 2 "bad .gen directive in $gen_file: want" \
				"sparse <count>"
		fi
		truncate -s "$gen_count" "$gen_target" ||
			die 2 "sparse generator failed for $gen_file"
		return
	fi

	# A generated git object store: two fields, the verb and the family
	# name the driver's build-store case turns into a bare store at the
	# target. The same ${var#* } guard as sparse's catches a bare
	# "gitstore", and a family carrying a space is refused here so the
	# mistake reads as a directive problem rather than a driver one.
	if [ "$gen_kind" = gitstore ]; then
		gen_family=${gen_line#* }
		case "$gen_family" in
		"$gen_line" | "" | *" "*)
			die 2 "bad .gen directive in $gen_file: want" \
				"gitstore <family>"
			;;
		esac
		if [ -z "${GITREAD_UNIT:-}" ]; then
			die 2 "environment variable GITREAD_UNIT is not set"
		fi
		"$GITREAD_UNIT" build-store "$gen_family" "$gen_target" ||
			die 2 "gitstore generator failed for $gen_file"
		return
	fi

	gen_rest=${gen_line#* }
	gen_count=${gen_rest%% *}
	gen_payload=${gen_rest#* }

	# ${var#* } hands back the whole string when there's no space left
	# to strip, so a two-field "repeat 4" would silently reuse the count
	# as the template, and a trailing delimiter ("repeat 4 ") slips past
	# both equality tests with an empty payload; demand all three fields
	# and a non-empty payload up front.
	if [ "$gen_rest" = "$gen_line" ] || [ "$gen_payload" = "$gen_rest" ] ||
		[ -z "$gen_payload" ]
	then
		die 2 "bad .gen directive in $gen_file: want" \
			"<verb> <count> <payload>"
	fi

	if ! is_uint "$gen_count"; then
		die 2 "bad .gen count in $gen_file: $gen_count"
	fi

	case "$gen_kind" in
	repeat)
		awk -v n="$gen_count" -v tpl="$gen_payload" \
			'BEGIN { for (i = 0; i < n; i++) printf tpl, i }' \
			>"$gen_target" ||
			die 2 "repeat generator failed for $gen_file"
		;;
	bytes)
		if [ "${#gen_payload}" -ne 1 ] && [ "$gen_payload" != NUL ]
		then
			die 2 "bad .gen byte token in $gen_file:" \
				"$gen_payload (want one character or NUL)"
		fi
		if [ "$gen_payload" = NUL ]; then
			dd if=/dev/zero bs=1 count="$gen_count" \
				of="$gen_target" 2>/dev/null ||
				die 2 "bytes generator failed for $gen_file"
		else
			dd if=/dev/zero bs=1 count="$gen_count" 2>/dev/null |
				tr '\0' "$gen_payload" >"$gen_target" ||
				die 2 "bytes generator failed for $gen_file"
		fi
		;;
	*)
		die 2 "bad .gen directive in $gen_file: $gen_kind"
		;;
	esac
}

# Populate the arena from the test dir: plain files are copied verbatim
# (except spec/expected.out/expected.err, which describe the test rather
# than feed it, and *.gen files, which are generated instead of copied).
populate_arena() {
	src_dir=$1

	for src_file in "$src_dir"/*; do
		[ -f "$src_file" ] || continue
		src_base=$(basename "$src_file")
		case "$src_base" in
		spec | expected.out | expected.err | *.gen) continue ;;
		esac
		cp "$src_file" "$arena/"
	done

	for gen_file in "$src_dir"/*.gen; do
		[ -f "$gen_file" ] || continue
		gen_base=$(basename "$gen_file")
		run_gen "$gen_file" "$arena/${gen_base%.gen}"
	done
}

# Parse the spec file into the spec_* globals. Every key is validated here.
parse_spec() {
	spec_file=$1

	spec_bin=DIFFOFDIFFS
	spec_args=""
	spec_stdin=""
	spec_exit=0
	spec_rlimit_fsize=""
	spec_timeout=""
	spec_timeout_asan=""
	spec_sigpipe=no
	spec_sigxfsz=no
	spec_stdout_limit=""
	spec_stdout_sha1=""

	while IFS= read -r spec_line || [ -n "$spec_line" ]; do
		[ -n "$spec_line" ] || continue
		case "$spec_line" in
		'#'*) continue ;;
		esac
		spec_key=${spec_line%%:*}
		spec_val=${spec_line#*:}
		ltrim "$spec_val"
		spec_val=$trimmed

		case "$spec_key" in
		bin)
			spec_bin=$spec_val
			;;
		args)
			spec_args=$spec_val
			;;
		stdin)
			spec_stdin=$spec_val
			;;
		exit)
			spec_exit=$spec_val
			;;
		rlimit-fsize)
			# The value is bytes. ulimit -f speaks in 512-byte
			# blocks under sh, so the exec wrapper divides by 512;
			# a value that division would round is refused rather
			# than silently shrunk. The form is pinned to plain
			# decimal before any arithmetic sees it: a leading
			# zero reads as octal and a value past 15 digits can
			# wrap, and either would quietly install a cap nobody
			# asked for.
			case "$spec_val" in
			0 | [1-9][0-9]*) ;;
			*) die 2 "bad rlimit-fsize: $spec_val" ;;
			esac
			if [ "${#spec_val}" -gt 15 ] ||
				[ $((spec_val % 512)) -ne 0 ]; then
				die 2 "bad rlimit-fsize (want a multiple of" \
					"512 bytes): $spec_val"
			fi
			spec_rlimit_fsize=$spec_val
			;;
		timeout)
			# GNU timeout treats a zero duration as no cap at
			# all, so a 0 here would join the serial lane while
			# capping nothing; refuse it, and refuse the same
			# zero on the timeout-asan override below.
			if ! is_uint "$spec_val" || [ "$spec_val" -eq 0 ]; then
				die 2 "bad timeout: $spec_val"
			fi
			spec_timeout=$spec_val
			;;
		timeout-asan)
			if ! is_uint "$spec_val" || [ "$spec_val" -eq 0 ]; then
				die 2 "bad timeout-asan: $spec_val"
			fi
			spec_timeout_asan=$spec_val
			;;
		sigpipe)
			if [ "$spec_val" != ignore ]; then
				die 2 "bad sigpipe value: $spec_val"
			fi
			spec_sigpipe=yes
			;;
		sigxfsz)
			if [ "$spec_val" != ignore ]; then
				die 2 "bad sigxfsz value: $spec_val"
			fi
			spec_sigxfsz=yes
			;;
		stdout-limit)
			if ! is_uint "$spec_val"; then
				die 2 "bad stdout-limit: $spec_val"
			fi
			spec_stdout_limit=$spec_val
			;;
		stdout-sha1)
			if [ "${#spec_val}" -ne 40 ]; then
				die 2 "bad stdout-sha1 (want 40 hex): $spec_val"
			fi
			case "$spec_val" in
			*[!0-9a-f]*)
				die 2 "bad stdout-sha1 (want 40 hex): $spec_val"
				;;
			esac
			spec_stdout_sha1=$spec_val
			;;
		*)
			die 2 "unknown spec key '$spec_key' in $spec_file"
			;;
		esac
	done <"$spec_file"

	case "$spec_exit" in
	sig:*) ;;
	*)
		if ! is_uint "$spec_exit"; then
			die 2 "bad exit value: $spec_exit"
		fi
		;;
	esac

	# The base timeout key doubles as the serial-lane membership marker
	# in the Makefile, so an ASan override without the base cap would
	# time-cap a test the lane never serializes; refuse the pairing gap.
	if [ -n "$spec_timeout_asan" ] && [ -z "$spec_timeout" ]; then
		die 2 "timeout-asan without timeout (the base cap also joins" \
			"the serial lane)"
	fi
}

# Resolve the expected numeric exit status from spec_exit, honoring the
# sig:NAME grammar for signal deaths (128 + signal number, the convention
# both bash and dash report a signal death's "$?" with).
resolve_expected_status() {
	case "$spec_exit" in
	sig:*)
		sig_name=${spec_exit#sig:}
		sig_num=$(kill -l "$sig_name" 2>/dev/null)
		if [ -z "$sig_num" ] || ! is_uint "$sig_num"; then
			die 2 "unknown signal name in exit key: $sig_name"
		fi
		expected_status=$((128 + sig_num))
		;;
	*)
		expected_status=$spec_exit
		;;
	esac
}

# The explicit target file lets fixtures use their own diff header names
recipe_apply() {
	if ! grep -q '^@@ ' "$1"; then
		return 0
	fi
	patch --batch --fuzz=0 --no-backup-if-mismatch --forward "$2" \
		"$recipe_file" <"$1"
}

check_recipe() (
	recipe_dir="$arena/.recipe"
	recipe_file="$recipe_dir/file"
	mkdir "$recipe_dir" || exit 1

	for recipe_side in 1 2; do
		cp "$test_dir/p${recipe_side}-source" "$recipe_file" || exit 1
		recipe_apply "$test_dir/patch${recipe_side}" -N || exit 1
		if ! cmp -s "$recipe_file" "$test_dir/p${recipe_side}-result"; then
			echo "input patch${recipe_side} recipe differs from p${recipe_side}-result"
			exit 1
		fi
	done

)

main() {
	if [ $# -ne 1 ]; then
		die 2 "usage: run-one.sh <test-dir>"
	fi

	tests_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd) || exit 2
	test_name=$1
	test_dir=$1
	case "$test_dir" in
	/*) ;;
	*) test_dir="$PWD/$test_dir" ;;
	esac
	[ -d "$test_dir" ] || die 2 "not a directory: $test_dir"

	spec_file="$test_dir/spec"
	[ -f "$spec_file" ] || die 2 "missing spec: $spec_file"

	parse_spec "$spec_file"

	# The two stdout expectations are exclusive: a tracked expected.out
	# beside a stdout-sha1 key leaves the reader guessing which one the
	# test actually pins.
	if [ -n "$spec_stdout_sha1" ] && [ -f "$test_dir/expected.out" ]; then
		die 2 "spec pins stdout-sha1 but expected.out also exists"
	fi

	# A digest of a truncated stream cannot verify the complete report.
	# Refuse this combination so the hash always covers all output.
	if [ -n "$spec_stdout_sha1" ] && [ -n "$spec_stdout_limit" ]; then
		die 2 "stdout-sha1 cannot compose with stdout-limit"
	fi

	# The file-size cap composes with neither of these. stdout-limit
	# routes the tool's stdout through a FIFO, whose writes RLIMIT_FSIZE
	# never counts, so the cap silently stops covering the one stream
	# most tests care about. timeout(1) narrates a capped child's
	# SIGXFSZ core dump onto its own stderr, which is the tool's stderr
	# here, contaminating the pinned face. No dispositioned test needs
	# either pairing; a future one has to redesign the combination
	# rather than inherit a quietly broken form.
	if [ -n "$spec_rlimit_fsize" ]; then
		if [ -n "$spec_stdout_limit" ]; then
			die 2 "rlimit-fsize cannot compose with stdout-limit"
		fi
		if [ -n "$spec_timeout" ]; then
			die 2 "rlimit-fsize cannot compose with timeout"
		fi
	fi

	# The file-size cap is the only SIGXFSZ source this harness has, so
	# an unpaired ignore could never change an outcome; refuse the dead
	# grammar rather than carry it.
	if [ "$spec_sigxfsz" = yes ] && [ -z "$spec_rlimit_fsize" ]; then
		die 2 "sigxfsz: ignore without rlimit-fsize"
	fi

	case "$spec_bin" in
	DIFFOFDIFFS | UDIFF_DRIVER | GITREAD_UNIT) ;;
	*) die 2 "unknown bin key: $spec_bin" ;;
	esac
	eval "bin_path=\${$spec_bin:-}"
	if [ -z "$bin_path" ]; then
		die 2 "environment variable $spec_bin is not set"
	fi
	if [ ! -x "$bin_path" ]; then
		die 2 "resolved binary is not executable: $bin_path" \
			"(from $spec_bin)"
	fi

	# Resolve relative interpreter paths before entering the fixture arena
	python_path=$(command -v "${PYTHON-python3}") ||
		die 2 "Python interpreter not found: ${PYTHON-python3}"
	case "$python_path" in
	/*) ;;
	*) python_path="$PWD/$python_path" ;;
	esac

	# The variant label arrives from the Makefile beside the binary
	# bindings, and the executor stays variant-blind except where a spec
	# declares a per-variant cap: the sanitizer build runs the heaviest
	# completion pins an order of magnitude slower, and one cap loose
	# enough for it forfeits the plain build's regression sensitivity.
	# The label and the binary can only disagree on a hand-typed run
	# or a command-line binding override, and a wrong pairing would
	# silently install the wrong cap, so a mismatch refuses in both
	# directions rather than guessing.
	if [ -n "${RUN_VARIANT:-}" ]; then
		case "$RUN_VARIANT" in
		plain | asan) ;;
		*) die 2 "bad RUN_VARIANT: $RUN_VARIANT (want plain or asan)" ;;
		esac
		# The file itself decides the variant: the sanitizer
		# runtime's init symbol is present in a sanitizer link's
		# dynamic symbols and absent otherwise, where a raw byte
		# search would also match the string inside any unrelated
		# file and would hinge on the grep implementation's
		# binary-file policy. The match takes the symbol as a whole
		# trailing field, so a name that merely contains the text
		# (an exported unrelated__asan_initializer, say) stays
		# plain. nm's exit status separates a file it
		# can't read as an ELF (a wrapper script, say) from an ELF
		# without the symbol; no read of the named file can classify
		# a stand-in that hands the work to another binary, so the
		# unreadable case refuses instead of guessing. A
		# -static-libasan link would hide the symbol from the
		# dynamic table; nothing here builds one.
		if bin_syms=$(nm -D "$bin_path" 2>/dev/null); then
			if printf '%s\n' "$bin_syms" |
				grep -q '[[:space:]]__asan_init$'; then
				bin_variant=asan
			else
				bin_variant=plain
			fi
		else
			die 2 "can't read the resolved binary's variant:" \
				"nm is missing or can't read the file as an" \
				"ELF: $bin_path"
		fi
		if [ "$bin_variant" != "$RUN_VARIANT" ]; then
			die 2 "RUN_VARIANT=$RUN_VARIANT but the resolved" \
				"binary's variant is $bin_variant: $bin_path"
		fi
	fi
	if [ -n "$spec_timeout_asan" ]; then
		if [ -z "${RUN_VARIANT:-}" ]; then
			die 2 "the spec carries timeout-asan but RUN_VARIANT" \
				"is not set (run through make, or set" \
				"RUN_VARIANT=plain or asan by hand)"
		fi
		if [ "$RUN_VARIANT" = asan ]; then
			spec_timeout=$spec_timeout_asan
		fi
	fi

	resolve_expected_status

	# A sig: spec kills the tool by design, and a piped core_pattern would
	# hand every such death to the system's core handler (RLIMIT_CORE=0
	# can't stop a piped handler, and the dumpable flag doesn't survive
	# execve from a wrapper), so the runner preloads a constructor that
	# clears the flag inside the tool itself. The same preload restores a
	# sig: spec's pinned signal to its default disposition in there, since
	# an ignored disposition survives the whole exec chain and no
	# non-interactive shell downstream of an ignoring parent may restore
	# it (POSIX): the pinned death holds by construction rather than by
	# assumption about the invoking environment. A capped run rides the
	# preload too, whatever its expected exit: a test that ignores XFSZ
	# fails BY dying on it if the tool ever regresses the disposition,
	# and that red run would feed the handler on every suite invocation
	# until someone noticed. A missing preload fails loudly here;
	# degrading quietly would resume feeding the handler.
	need_tool_setup=no
	case "$spec_exit" in
	sig:*) need_tool_setup=yes ;;
	esac
	if [ -n "$spec_rlimit_fsize" ]; then
		need_tool_setup=yes
	fi
	if [ "$need_tool_setup" = yes ]; then
		if [ -z "${TOOL_SETUP_SO:-}" ] || [ ! -f "$TOOL_SETUP_SO" ]
		then
			die 2 "TOOL_SETUP_SO is not set or missing" \
				"(sig: and rlimit-fsize specs need the" \
				"tool-setup preload)"
		fi
	fi

	arena_base=${ARENA:-$tests_dir/../build/tests}
	mkdir -p "$arena_base" || die 2 "cannot create arena directory"

	# Keep paths valid after entering the fixture arena
	arena_base=$(CDPATH= cd "$arena_base" && pwd -P) ||
		die 2 "cannot resolve arena directory"
	arena=$(mktemp -d "$arena_base/$(basename "$test_dir").XXXXXX") ||
		die 2 "mktemp failed to create an arena under $arena_base"

	populate_arena "$test_dir"

	cd "$arena" || die 2 "cannot cd into arena $arena"

	# A misspelled stdin file has to fail loudly here: piping a missing
	# file would degrade to empty stdin and let a test pass green without
	# ever feeding its input.
	if [ -n "$spec_stdin" ] && [ ! -f "$spec_stdin" ]; then
		die 2 "stdin file $spec_stdin missing from the arena"
	fi

	# Build the full command line into "$@": test args first, then the
	# resolved binary and (if requested) the timeout(1) wrapper in front
	# of it, so we never touch args_line again after this point.
	set -f
	# shellcheck disable=SC2086
	set -- $spec_args
	set +f
	set -- "$bin_path" "$@"

	# Innermost on purpose: env(1) execs the tool, so these bindings
	# ride only the process the spec is about to kill or cap, never the
	# wrapper shells around it. The run composition owns the loader and
	# sanitizer knobs the tool reads at startup: LD_PRELOAD,
	# TOOL_SETUP_SIG_DFL, ASAN_OPTIONS, LSAN_OPTIONS, and UBSAN_OPTIONS
	# are bound on every run, so an ambient value (stdbuf's preload, a
	# relax left behind by a login shell) never reaches the runtime.
	# Both asan-side option variables matter, since that runtime takes
	# its common flags from either name: ASAN_OPTIONS carries the
	# link-order check, and LSAN_OPTIONS can move a report off stderr
	# (log_path), zero its exit status (exitcode) or flood stderr
	# (verbosity), any of which would let a real report pass as a clean
	# run. UBSAN_OPTIONS is the UBSan runtime's own surface (the
	# sanitizer link carries libubsan): its reports recover by default,
	# so the exit status never changes and the pinned stderr bytes are
	# the only detector an ambient suppressions= file would silence. A
	# non-setup run pins the link-order check on rather than leaving it
	# to the runtime's default, so a toolchain that flips that default
	# can't quietly disable it; the check guards which library owns the
	# allocator symbols, so a green run under it is vouching for its
	# own instrumentation. A preload run scopes the relax to itself,
	# since any first DSO trips the check whether or not it intercepts
	# anything; its ambient UBSAN_OPTIONS was already inert (the
	# tool-setup constructor clears the dumpable flag before UBSan's
	# lazy option parse, and a non-dumpable process drops that
	# surface), so the binding there buys symmetry over a coincidence.
	# A sig: spec hands the library its pinned signal's number through
	# TOOL_SETUP_SIG_DFL, restoring the default disposition inside the
	# tool no matter what the invoking environment ignores; every other
	# run binds the variable empty, so an inherited number can't
	# smuggle a restore into a run whose spec never named one.
	# This composition is the only writer of these five names: no spec
	# grammar binds environment variables, so the list lives here alone and
	# nothing else has to stay in sync with it.
	if [ "$need_tool_setup" = yes ]; then
		set -- env "LD_PRELOAD=$TOOL_SETUP_SO" \
			"TOOL_SETUP_SIG_DFL=${sig_num:-}" \
			"ASAN_OPTIONS=verify_asan_link_order=0" \
			"LSAN_OPTIONS=" "UBSAN_OPTIONS=" "$@"
	else
		set -- env "LD_PRELOAD=" "TOOL_SETUP_SIG_DFL=" \
			"ASAN_OPTIONS=verify_asan_link_order=1" \
			"LSAN_OPTIONS=" "UBSAN_OPTIONS=" "$@"
	fi

	# The file-size cap goes on the tool alone, through an exec wrapper:
	# capping the whole run subshell would put the runner's own
	# bookkeeping (the status write, the stdout-limit reader, sh's
	# signal narration) under the same limit, and a small cap kills
	# those writers with SIGXFSZ before the test's verdict ever lands.
	# The wrapper shell execs the tool, so the cap rides the tool's own
	# pid and nothing else. ulimit -f counts 512-byte blocks under sh
	# while the spec key is bytes, hence the division. The 512 hangs on
	# that `sh -c` spelling: sh implementations speak 512-byte blocks in
	# POSIX mode (bash included), while bash under its own name speaks
	# 1024, so respelling the wrapper `bash -c` would silently double
	# every cap.
	if [ -n "$spec_rlimit_fsize" ]; then
		set -- sh -c 'ulimit -f "$1" && shift && exec "$@"' sh \
			$((spec_rlimit_fsize / 512)) "$@"
	fi
	if [ -n "$spec_timeout" ]; then
		set -- timeout "$spec_timeout" "$@"
	fi

	# The subshell's own stderr is diverted into the arena: sh narrates a
	# child's non-PIPE signal death (e.g. "Aborted (core dumped)") onto
	# it, which would otherwise land in every check log. A failing test
	# surfaces the noise through report_fail; a passing one discards it
	# with the arena.
	(
		if [ "$spec_sigpipe" = yes ]; then
			trap '' PIPE
		fi

		# An ignored disposition survives fork and exec, so the capped
		# write returns EFBIG for the tool's own write-error report
		# instead of terminating the process with SIGXFSZ.
		if [ "$spec_sigxfsz" = yes ]; then
			trap '' XFSZ
		fi

		if [ -n "$spec_stdout_limit" ]; then
			fifo="$arena/.stdout-fifo"
			mkfifo "$fifo" || exit 2
			head -c "$spec_stdout_limit" <"$fifo" \
				>"$arena/actual.out" &
			head_pid=$!
			stdout_target=$fifo
		else
			stdout_target="$arena/actual.out"
		fi

		if [ -n "$spec_stdin" ]; then
			cat "$spec_stdin" | "$@" \
				>"$stdout_target" 2>"$arena/actual.err"
		else
			"$@" </dev/null \
				>"$stdout_target" 2>"$arena/actual.err"
		fi
		run_status=$?

		if [ -n "$spec_stdout_limit" ]; then
			wait "$head_pid"
			rm -f "$fifo"
		fi

		echo "$run_status" >"$arena/.status"
	) 2>"$arena/.shellnoise"
	# The subshell's own exit status is whatever its last command
	# (writing .status) returned, normally 0, unless it bailed out early
	# from a mkfifo failure before ever reaching that line.
	subshell_status=$?
	if [ "$subshell_status" -ne 0 ] || [ ! -f "$arena/.status" ]; then
		die 2 "failed to set up the run in $arena (mkfifo failed)"
	fi
	tool_status=$(cat "$arena/.status")
	rm -f "$arena/.status"

	if [ -n "$spec_timeout" ] && [ "$tool_status" -eq 124 ]; then
		note "timed out after ${spec_timeout}s" \
			"(timeout(1) reported status 124)"
		report_fail
	fi

	failed=no

	if [ -n "$spec_stdout_sha1" ]; then
		# The digest is metadata like the byte counts above, not
		# stream data, so capturing it with $() is safe.
		actual_sha1=$(sha1sum "$arena/actual.out")
		actual_sha1=${actual_sha1%% *}
		if [ "$actual_sha1" != "$spec_stdout_sha1" ]; then
			note "stdout sha1 mismatch: expected" \
				"$spec_stdout_sha1, got $actual_sha1" \
				"($(wc -c <"$arena/actual.out") bytes)"
			failed=yes
		fi
	elif ! check_stream "$test_dir/expected.out" "$arena/actual.out" \
		stdout; then
		failed=yes
	fi
	if ! check_stream "$test_dir/expected.err" "$arena/actual.err" \
		stderr; then
		failed=yes
	fi
	if [ "$tool_status" -ne "$expected_status" ]; then
		note "exit status mismatch: expected $spec_exit" \
			"(numeric $expected_status), got $tool_status"
		failed=yes
	fi
	if [ -f "$test_dir/p1-source" ] &&
		! check_recipe >"$arena/recipe.log" 2>&1; then
		note "original input patch check failed; see $arena/recipe.log"
		failed=yes
	fi

	if [ "$spec_bin" = DIFFOFDIFFS ] && [ "$tool_status" -eq 0 ] &&
		! "$python_path" "$tests_dir/check-fixture-review.py" "$bin_path" "$arena" \
			"$test_dir" >"$arena/review-check.log" 2>&1; then
		note "review source check failed; see $arena/review-check.log"
		failed=yes
	fi

	if [ "$failed" = yes ]; then
		report_fail
	fi
	report_pass
}

main "$@"
