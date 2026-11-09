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
# CHECKS=1 与 scripts/build.ps1 -Checks 等价：打开 /Werror（QZRPC_WERROR）与生成物
# 编译闸（QZRPC_CHECK_CODEGEN）。scripts/ci.sh 的 Stage 1 一律带它 —— 否则同一个
# "CI 通过"在两个平台上含义不同：Linux 侧会静默放过警告，覆盖范围比 Windows 少一层。
extra_args=()
if [ "${CHECKS:-0}" = "1" ]; then
  extra_args+=("-DQZRPC_WERROR=ON")
  extra_args+=("-DQZRPC_CHECK_CODEGEN=ON")
  echo "==> Checks ON (WERROR + codegen compile gate)"
fi
cmake -S "${root}" -B "${build_dir}" -DCMAKE_BUILD_TYPE="${config}" \
  ${extra_args[@]+"${extra_args[@]}"}

echo "==> Building with ${jobs} jobs"
cmake --build "${build_dir}" -j "${jobs}"

if [ "${SKIP_TESTS:-0}" != "1" ]; then
  # ---------------------------------------------------------------------------
  # 守卫：磁盘上每个 tests/test_*.cpp 都必须是一个已注册的 ctest 用例。
  #
  # 这不是假设。configure 若在某个测试文件还没写完时跑过，它会被跳过
  # （tests/CMakeLists.txt 里的 `if (NOT EXISTS ...) continue()`），而
  # `cmake --build` 之后再也不会重跑 configure。结果是一次完美通过的运行却少了用例：
  #   100% tests passed, 0 tests failed out of 9     <- 少了 4 个用例 / 8035 条断言
  # 不报错、不告警，而且看起来比真实情况更健康。所以这里校验**数量**而不是相信结论：
  # "全绿"必须同时意味着"全在"。与 scripts/build.ps1 的同名守卫等价。
  # ---------------------------------------------------------------------------
  echo "==> Guard: ctest 注册项 vs tests/test_*.cpp"
  listing="$(ctest --test-dir "${build_dir}" -N)" || {
    echo "ERROR: ctest -N failed" >&2
    exit 1
  }
  reg_cpp=()
  while IFS= read -r name; do
    case "${name}" in
      test_*) reg_cpp+=("${name}") ;;
    esac
  done < <(printf '%s\n' "${listing}" |
           sed -nE 's/^[[:space:]]*Test[[:space:]]+#[0-9]+:[[:space:]]+([^[:space:]]+).*/\1/p')

  disk=()
  for f in "${root}"/tests/test_*.cpp; do
    [ -e "${f}" ] || continue
    disk+=("$(basename "${f}" .cpp)")
  done

  missing=()
  for b in ${disk[@]+"${disk[@]}"}; do
    hit=0
    for r in ${reg_cpp[@]+"${reg_cpp[@]}"}; do
      if [ "${r}" = "${b}" ]; then hit=1; break; fi
    done
    [ "${hit}" -eq 1 ] || missing+=("${b}")
  done
  if [ "${#missing[@]}" -gt 0 ]; then
    echo "ERROR: 以下测试源文件存在但没有注册到 ctest: ${missing[*]}" >&2
    echo "       （过期的 configure 会跳过它当时还不存在的文件；删掉 build 目录重试，" >&2
    echo "        或修 tests/CMakeLists.txt）" >&2
    exit 1
  fi
  echo "    ctest 注册 ${#reg_cpp[@]} 个 C++ 用例，tests/ 有 ${#disk[@]} 个 test_*.cpp"

  echo "==> Running ctest"
  ctest --test-dir "${build_dir}" --output-on-failure
fi

echo "==> OK"
