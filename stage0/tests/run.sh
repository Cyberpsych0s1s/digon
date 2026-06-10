#!/usr/bin/env bash
# Golden + compile-fail test runner for digon stage 0 (spec §20.1: a shell
# runner, no test framework).
#
# Tests are organised by feature category under both directories. The runner
# globs every .dg file recursively; the sidecar files sit next to it:
#
#   golden/<category>/NAME.dg         must compile and run
#   golden/<category>/NAME.out        (optional) expected stdout, byte-for-byte
#   golden/<category>/NAME.exit       (optional) expected process exit code
#   compile_fail/<category>/NAME.dg   must FAIL to compile
#   compile_fail/<category>/NAME.txt  (optional) substring expected in diags
#
# Usage: tests/run.sh [path-to-digon.exe]

set -u

# Resolve this script's directory using only bash builtins. ctest launches an
# msys2 bash whose PATH (ucrt64/bin) has the compiler but NOT coreutils like
# `dirname`/`cat`/`grep`/`tr`; relying on those silently emptied $here and made
# the whole suite match zero files and "pass" vacuously. Everything below uses
# builtins; the only externals are `digon` and the C compiler it shells out to.
script_dir="${0%/*}"
[[ "$script_dir" == "$0" ]] && script_dir="."
here="$(cd "$script_dir" && pwd)"
digon="${1:-$here/../build/digon.exe}"
[[ -x "$digon" ]] || digon="$here/../build/digon"

if [[ ! -x "$digon" ]]; then
    echo "digon binary not found (build first): $digon" >&2
    exit 2
fi

pass=0
fail=0

# Produce a short "category/name" label from a .dg path (for status output).
rel_label() {
    local dg="$1" root="$2"
    local rel="${dg#$root/}"
    echo "${rel%.dg}"
}

check_golden() {
    local dg="$1"
    local base="${dg%.dg}"            # sibling sidecars share this prefix
    local label; label="$(rel_label "$dg" "$here/golden")"
    local out_file="${base}.out"
    local exit_file="${base}.exit"

    local actual rc
    # Capture without a pipeline so $? is digon's, then strip CR via parameter
    # expansion (Windows puts/printf emit CRLF; we want LF for comparison).
    actual="$("$digon" run "$dg")"
    rc=$?
    actual="${actual//$'\r'/}"

    local ok=1
    if [[ -f "$out_file" ]]; then
        local expected; expected="$(<"$out_file")"; expected="${expected//$'\r'/}"
        [[ "$actual" == "$expected" ]] || { ok=0; echo "  stdout mismatch: $label (got '$actual')"; }
    fi
    if [[ -f "$exit_file" ]]; then
        local want; want="$(<"$exit_file")"; want="${want//[$'\r\n']/}"
        [[ "$rc" == "$want" ]] || { ok=0; echo "  exit mismatch: $label (got $rc, want $want)"; }
    fi
    if [[ $ok == 1 ]]; then echo "  pass  golden/$label"; ((pass++)); else ((fail++)); fi
}

check_compile_fail() {
    local dg="$1"
    local base="${dg%.dg}"
    local label; label="$(rel_label "$dg" "$here/compile_fail")"
    local txt_file="${base}.txt"

    local output rc
    output="$("$digon" build "$dg" -o "${base}.exe" 2>&1)"
    rc=$?
    rm -f "${base}.exe" "${base}.exe.o" 2>/dev/null  # cleanup only; ok if rm is absent

    local ok=1
    [[ $rc -ne 0 ]] || { ok=0; echo "  expected failure but it compiled: $label"; }
    if [[ -f "$txt_file" ]]; then
        local sub; sub="$(<"$txt_file")"; sub="${sub//$'\r'/}"; sub="${sub%$'\n'}"
        [[ "$output" == *"$sub"* ]] || { ok=0; echo "  missing diagnostic '$sub' in: $label"; }
    fi
    if [[ $ok == 1 ]]; then echo "  pass  compile_fail/$label"; ((pass++)); else ((fail++)); fi
}

shopt -s nullglob globstar
for dg in "$here/golden/"**/*.dg;       do check_golden "$dg"; done
for dg in "$here/compile_fail/"**/*.dg; do check_compile_fail "$dg"; done

echo
echo "$pass passed, $fail failed"
[[ $fail -eq 0 ]]
