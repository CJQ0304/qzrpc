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

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "${here}/.." && pwd)"
build_dir="${root}/build"
ci_dir="${build_dir}/ci"
codegen_dir="${root}/codegen"
python_bin="${PYTHON:-python3}"

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
  while IFS= read -r -d '' file; do sources+=("${file}"); done \
    < <(find "${out}" -maxdepth 1 -name '*.cpp' -print0 | sort -z)
  [ "${#sources[@]}" -gt 0 ] || fail "no .cpp produced for ${input}"

  local start end file
  start="$(date +%s%N)"
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
  end="$(date +%s%N)"
  printf '    %d TU(s) compiled in %d ms\n' "${#sources[@]}" "$(( (end - start) / 1000000 ))"
}

compile_gate idl 'idl/*.qz'
compile_gate fixtures 'codegen/tests/fixtures/*.qz'

step "CI OK"
