#!/usr/bin/env bash
# Qingzhou RPC - Linux/macOS build script.
#
# Usage:
#   ./scripts/build.sh              # Release
#   ./scripts/build.sh Debug        # Debug
#   SKIP_TESTS=1 ./scripts/build.sh # build only
#
# The Windows counterpart is scripts/build.ps1 (MSVC + Ninja via msvc_env.cmd).

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "${here}/.." && pwd)"
build_dir="${root}/build"
config="${1:-Release}"

jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || true)"
if [ -z "${jobs}" ]; then
  jobs=4
fi

echo "==> Configuring (${config}) -> ${build_dir}"
cmake -S "${root}" -B "${build_dir}" -DCMAKE_BUILD_TYPE="${config}"

echo "==> Building with ${jobs} jobs"
cmake --build "${build_dir}" -j "${jobs}"

if [ "${SKIP_TESTS:-0}" != "1" ]; then
  echo "==> Running ctest"
  ctest --test-dir "${build_dir}" --output-on-failure
fi

echo "==> OK"
