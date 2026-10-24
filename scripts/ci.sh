#!/usr/bin/env bash
# Qingzhou RPC - CI pipeline (Linux / macOS, and MSVC via Git Bash).
#
# Stages (same as scripts/ci.ps1):
#   1. build + ctest        -> scripts/build.sh
#                              (ctest = C++ unit + codegen Python + interop_protobuf;
#                               the interop case self-skips, exit code 2, when this
#                               machine has no protobuf/protoc -- see tools/interop/)
#   2. idl contract         -> qzcodegen --check on idl/*.qz (writes nothing)
#   3. generated/ in sync   -> regenerate into build/ci/gen_fresh, then diff -r
#   4. codegen compile gate -> regenerate idl/*.qz and codegen/tests/fixtures/*.qz,
#                              compile EVERY produced .cpp (no link)
#
# Stage 4 is the only automated check that catches "the generator emits C++ that does
# not compile for some field shape": idl/echo.qz only exercises 8 shapes, while
# codegen/tests/fixtures/kitchen_sink.qz covers 11 scalars x 4 labels, [packed=false],
# empty messages, nested types and INT64_MIN/UINT64_MAX defaults.
#
# Usage:
#   ./scripts/ci.sh
#   SKIP_TESTS=1 ./scripts/ci.sh
#   SKIP_BUILD=1 ./scripts/ci.sh   # gates 2-4 only (tree may not compile: teammate mid-edit)
#
# MSVC note: under Git Bash, cl.exe needs Windows paths for every argument that is a
# path. Passing /tmp/x.cpp straight through makes cl treat it as an option and fail
# with "D8003: missing source filename". winpath() below does the cygpath -w dance.
#
# ★ HONESTY NOTE -- this file has NOT been executed on Linux or macOS.
#   It was written against the same four stages as scripts/ci.ps1 (which runs green on
#   Windows) and reviewed line by line, but the only machine available to its author
#   runs MSVC. Concretely: the GCC/Clang branch, `-Werror` behaviour on the generated
#   code under GCC, and the BSD userland of macOS have not been exercised.
#   Saying so is more useful than shipping a script that *looks* cross-platform
#   verified. If you run it somewhere else, expect to fix small things -- the two
#   known portability traps (BSD `date +%s%N`, `sort -z`) are already worked around
#   below, but there may be others.
#
# Portability rules followed here, because macOS ships BSD userland:
#   * no `date +%s%N` (GNU only)   -> elapsed_ms() falls back to second precision
#   * no `sort -z` / `find -print0` pairs (not in BSD sort) -> plain line loop

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "${here}/.." && pwd)"
build_dir="${root}/build"
ci_dir="${build_dir}/ci"
codegen_dir="${root}/codegen"

# Pick a Python and pin it to an ABSOLUTE path, then print it.
#
# Same lesson as the one that made one ctest case "Passed in PowerShell, skipped in
# Git Bash": a bare name ("python3") is resolved against PATH at run time, so the same
# command means different things in different shells on the same machine. Resolving it
# once, up front, and echoing the winner turns a silent "skipped" into a visible fact.
pick_python() {
  if [ -n "${PYTHON:-}" ]; then printf '%s' "${PYTHON}"; return; fi
  for cand in python3 python py; do
    if command -v "${cand}" >/dev/null 2>&1; then command -v "${cand}"; return; fi
  done
  printf 'python3'
}
python_bin="$(pick_python)"

if ! command -v "${python_bin}" >/dev/null 2>&1; then
  printf 'ERROR: no python interpreter found (tried %s). Set PYTHON=/path/to/python3.\n' \
    "${python_bin}" >&2
  exit 1
fi
printf '==> python: %s\n' "${python_bin}"

# Compiler selection: honour CXX, otherwise take the first one that actually exists.
# On Windows/Git Bash there is no c++/g++, but cl.exe is present as soon as
# scripts/msvc_env.cmd has been sourced (or run as the parent process), so `cl` is in
# the candidate list -- the compile gate would otherwise die at stage 4 with
# "c++: command not found" 40 lines in.
pick_cxx() {
  if [ -n "${CXX:-}" ]; then printf '%s' "${CXX}"; return; fi
  for cand in c++ g++ clang++ cl; do
    if command -v "${cand}" >/dev/null 2>&1; then printf '%s' "${cand}"; return; fi
  done
  printf 'c++'
}
cxx="$(pick_cxx)"
cxx_base="$(basename "${cxx}")"

step() { printf '\n==> %s\n' "$1"; }
fail() { printf 'ERROR: %s\n' "$1" >&2; exit 1; }

# Milliseconds since epoch, portably.
# GNU date understands %N (nanoseconds); BSD/macOS date prints a literal "N", which
# under `set -e` would turn the arithmetic below into a hard failure instead of a
# timer bug. Detect that and fall back to whole seconds.
now_ms() {
  local raw
  raw="$(date +%s%N 2>/dev/null || date +%s)"
  case "${raw}" in
    *N*|'') date +%s | awk '{ printf "%d", $1 * 1000 }' ;;
    *) awk -v ns="${raw}" 'BEGIN { printf "%d", ns / 1000000 }' ;;
  esac
}

# Convert a POSIX path to a Windows path when the compiler is cl.exe under MSYS/Git
# Bash; identity elsewhere.
winpath() {
  if command -v cygpath >/dev/null 2>&1; then
    cygpath -w "$1"
  else
    printf '%s' "$1"
  fi
}

is_msvc=0
case "${cxx_base}" in
  cl|cl.exe|clang-cl|clang-cl.exe) is_msvc=1 ;;
esac

# Fail before doing 3 stages of work if the compiler is missing. This is the check that
# turns "c++: command not found" at stage 4 into one actionable line at stage 0.
if ! command -v "${cxx}" >/dev/null 2>&1; then
  fail "no C++ compiler found (selected: ${cxx}). Set CXX explicitly, e.g.
     CXX=cl  ./scripts/ci.sh    # Git Bash, after scripts/msvc_env.cmd (cl.exe on PATH)
     CXX=g++ ./scripts/ci.sh    # Linux / macOS"
fi

# ---------------------------------------------------------------------------
# 1. Build + unit tests
# ---------------------------------------------------------------------------
step "Stage 1/4: build + ctest"
if [ "${SKIP_BUILD:-0}" = "1" ]; then
  printf '    skipped (SKIP_BUILD=1)\n'
elif [ "${SKIP_TESTS:-0}" = "1" ]; then
  SKIP_TESTS=1 "${here}/build.sh"
else
  "${here}/build.sh"
fi

rm -rf "${ci_dir}"
mkdir -p "${ci_dir}"

gen() { # gen <input-pattern> <out-dir|""> [extra args...]
  local input="$1" out="$2"; shift 2
  if [ -n "${out}" ]; then
    ( cd "${root}" && PYTHONPATH="${codegen_dir}" "${python_bin}" -m qzcodegen \
        --input "${input}" --out "${out}" "$@" )
  else
    ( cd "${root}" && PYTHONPATH="${codegen_dir}" "${python_bin}" -m qzcodegen \
        --input "${input}" "$@" )
  fi
}

# ---------------------------------------------------------------------------
# 2. IDL contract
# ---------------------------------------------------------------------------
step "Stage 2/4: qzcodegen --check on idl/*.qz"
gen 'idl/*.qz' '' --check || fail "codegen --check rejected idl/*.qz"

# ---------------------------------------------------------------------------
# 3. generated/ must match the current generator output
# ---------------------------------------------------------------------------
step "Stage 3/4: generated/ is in sync with idl/"
mkdir -p "${ci_dir}/gen_fresh"
gen 'idl/*.qz' "${ci_dir}/gen_fresh" --cpp || fail "codegen failed while regenerating idl/*.qz"
if ! diff -r "${ci_dir}/gen_fresh" "${root}/generated"; then
  fail "generated/ is out of sync with idl/ (commit the regenerated files or fix the drift)"
fi
printf '    generated/ matches the generator output\n'

# ---------------------------------------------------------------------------
# 4. Compile gate
# ---------------------------------------------------------------------------
compile_gate() { # compile_gate <label> <input-pattern>
  local label="$1" input="$2"
  local out="${ci_dir}/${label}"
  step "Stage 4/4: compile gate [${label}]"
  mkdir -p "${out}/obj"

  gen "${input}" "${out}" --cpp || fail "codegen failed for ${input}"

  local sources=()
  # Plain line-based loop instead of `find -print0 | sort -z`: BSD sort (macOS) has no
  # -z, and generated file names never contain newlines, so the pair buys nothing.
  while IFS= read -r file; do
    [ -n "${file}" ] && sources+=("${file}")
  done < <(find "${out}" -maxdepth 1 -name '*.cpp' | sort)
  [ "${#sources[@]}" -gt 0 ] || fail "no .cpp produced for ${input}"

  local start end file
  start="$(now_ms)"
  for file in "${sources[@]}"; do
    if [ "${is_msvc}" = "1" ]; then
      # MSYS/Git Bash rewrites any argument starting with '/' into a Windows path, so
      # "/nologo" becomes "C:/Program Files/Git/nologo" and every cl switch is handed
      # over as a bogus source file name: warning D9024 x N, then
      # "fatal error C1083: cannot open include file" because /I was eaten too.
      # These two env vars are the documented escape hatches (MSYS2_ARG_CONV_EXCL for
      # MSYS2, MSYS_NO_PATHCONV for Git for Windows); both are no-ops elsewhere.
      # NOTE: this only matters for path-like args -- winpath() already converts the
      # ones we *want* converted; the exclusion is what lets the switches survive.
      MSYS2_ARG_CONV_EXCL='*' MSYS_NO_PATHCONV=1 \
      "${cxx}" /nologo /c /std:c++17 /utf-8 /W4 /WX /permissive- /EHsc /MD \
        /I "$(winpath "${root}/src")" /I "$(winpath "${out}")" \
        /Fd"$(winpath "${out}/obj")\\vc.pdb" /Fo"$(winpath "${out}/obj")\\" \
        "$(winpath "${file}")" \
        || fail "generated source does not compile: $(basename "${file}")"
    else
      "${cxx}" -std=c++17 -Wall -Wextra -Werror -c \
        -I "${root}/src" -I "${out}" \
        -o "${out}/obj/$(basename "${file}" .cpp).o" "${file}" \
        || fail "generated source does not compile: $(basename "${file}")"
    fi
  done
  end="$(now_ms)"
  printf '    %d TU(s) compiled in %d ms\n' "${#sources[@]}" "$(( end - start ))"
}

compile_gate idl 'idl/*.qz'
compile_gate fixtures 'codegen/tests/fixtures/*.qz'

step "CI OK"
