# 轻舟 RPC —— protobuf 线格式互通验证（docs/00-file-map.md 之外的验收装置）
#
# 做什么
#    dump_wire 用**框架自身的序列化实现**编码/解码一份打满边界值的 demo.EchoRequest；
#    verify_interop.py 用**官方 protobuf** 对同一份字节流做对照解析（双向）。
#    这是全项目唯一一处"拿第三方权威实现给自己对答案"的验证 —— 它不是自证，
#    所以必须能被 CI 复跑，而不是停在 .scratch 里手工 cl 链接出来的一次性证据。
#
# 三个刻意的设计
#   1) protobuf / protoc 只在**验证期**需要：不进 src/、不进 qzrpc 库、不进任何
#      框架产物。找不到就跳过（ctest 记为 Skipped），**绝不**让"这台机器没装
#      protobuf"变成 configure 失败 —— 框架本体零第三方依赖这条不能被验证工具破坏。
#   2) dump_wire 链接 generated/echo.qz.cpp，而那个 TU 里同时含客户端 Stub 实现
#      （Stub::Echo 调 rpc::Channel::Call），所以只在 QZRPC_HAVE_CLIENT（顶层算出，
#      与 example/ 共用同一份定义）为真时注册。客户端一落地自动生效。
#   3) 用 configure 期探测挑一个**能 import google.protobuf** 的解释器，而不是写死
#      `python`：本机 PATH 上的 python 未必装了 protobuf（实测：默认 python 是
#      3.13 但没装 protobuf，装了的是另一个解释器）。写死会让用例永远静默跳过，
#      而"跳过"和"通过"在 CI 报表里长得很像 —— 这是最危险的假绿。

# 生成物是 IDL 的产物且已入库；缺失时（还没跑过 gen_proto）只 STATUS，不报错。
if (NOT EXISTS "${CMAKE_SOURCE_DIR}/generated/echo.qz.cpp")
  message(STATUS "qzrpc interop: generated/echo.qz.cpp missing "
                 "(run: cmake --build <build> --target gen_proto); skipped")
  return()
endif()

if (NOT QZRPC_HAVE_CLIENT)
  message(STATUS "qzrpc interop: dump_wire skipped "
                 "(needs client/net/protocol/serialize; Channel::Call unresolved otherwise)")
  return()
endif()

add_executable(dump_wire dump_wire.cpp "${CMAKE_SOURCE_DIR}/generated/echo.qz.cpp")
# generated/ 单独进 include 路径：生成物之间用裸文件名互引（#include "echo.qz.h"）
target_include_directories(dump_wire PRIVATE "${CMAKE_SOURCE_DIR}/generated")
target_link_libraries(dump_wire PRIVATE qzrpc)
# 不挂 add_dependencies(gen_proto)：生成物已入库，挂上去会让每次构建都跑 Python
# 并重写 generated/。改了 idl/ 之后手动 `cmake --build <build> --target gen_proto`。

# ---------------------------------------------------------------------------
# ctest 用例：interop_protobuf
# ---------------------------------------------------------------------------
option(QZRPC_REQUIRE_INTEROP
  "Fail configure when the protobuf interop verification cannot run (release gate)" OFF)

set(QZRPC_INTEROP_PYTHON "" CACHE STRING
    "Python able to 'import google.protobuf' (empty = probe PATH: python/python3/py)")

# 显式指定（cache 变量或环境变量）就是显式指定：不再往后猜。
# 否则 -DQZRPC_INTEROP_PYTHON=<没有 protobuf 的解释器> 会被"常见安装位置"那一圈
# 悄悄救回来，用户以为自己钉住了解释器，实际跑的是另一个 —— 这类"我以为我指定了"
# 的静默替换是排障时最费时间的一种。
set(_qz_candidates "")
if (QZRPC_INTEROP_PYTHON)
  set(_qz_candidates "${QZRPC_INTEROP_PYTHON}")
elseif (DEFINED ENV{QZRPC_INTEROP_PYTHON})
  set(_qz_candidates "$ENV{QZRPC_INTEROP_PYTHON}")
else()
  list(APPEND _qz_candidates "${QZRPC_PYTHON}" "python" "python3" "py")
endif()

# 自动探测时再补一轮常见安装位置（best-effort）：protobuf 常常装在 conda 里，而 PATH
# 上的 python 往往是另一个发行版 —— 实测本机就是这种情况（`python` 是 3.13 但没装
# protobuf，装了它的是 %USERPROFILE%\anaconda3）。不补这一圈，本用例会永远静默跳过，
# "跳过"和"通过"在汇总里长得很像，等于验证不存在。
# 注意：这里的 file(GLOB) 只用来**找工具**，与"源文件清单禁止 GLOB"那条规则无关。
if (WIN32 AND NOT QZRPC_INTEROP_PYTHON AND NOT DEFINED ENV{QZRPC_INTEROP_PYTHON})
  file(GLOB _qz_python_hints
    "$ENV{USERPROFILE}/anaconda3/python.exe"
    "$ENV{USERPROFILE}/miniconda3/python.exe"
    "$ENV{LOCALAPPDATA}/Programs/Python/Python3*/python.exe"
    "$ENV{ProgramFiles}/Python3*/python.exe")
  list(APPEND _qz_candidates ${_qz_python_hints})
endif()
list(REMOVE_DUPLICATES _qz_candidates)

set(_qz_interop_python "")
foreach(_candidate IN LISTS _qz_candidates)
  # 命令不存在时 execute_process 把错误写进 RESULT_VARIABLE（不致命），
  # 所以这里可以安全地挨个试。用字符串比较：失败时 _rc 是错误文本而不是数字。
  execute_process(COMMAND "${_candidate}" -c "import google.protobuf"
                  RESULT_VARIABLE _probe_rc OUTPUT_QUIET ERROR_QUIET)
  if ("${_probe_rc}" STREQUAL "0")
    set(_qz_interop_python "${_candidate}")
    break()
  endif()
endforeach()

if (NOT _qz_interop_python)
  if (QZRPC_REQUIRE_INTEROP)
    message(FATAL_ERROR
      "QZRPC_REQUIRE_INTEROP=ON but no Python with 'google.protobuf' was found "
      "(tried: ${_qz_candidates}).\n"
      "Install it (pip install protobuf) or pass -DQZRPC_INTEROP_PYTHON=<path/to/python>.")
  endif()
  message(STATUS "qzrpc interop: no Python with 'google.protobuf' found "
                 "(tried: ${_qz_candidates}); ctest case 'interop_protobuf' not registered")
  return()
endif()

# 成功时也打印选了哪个解释器：环境要可重现，"用的是哪个 python"必须能从
# configure 日志里读出来，否则换台机器就不知道该怎么复现。
#
# 而且必须把裸名（python / py）**钉成绝对路径**再交给 add_test：ctest 执行时用的是
# 调用者的 PATH，同一个 build 目录在 PowerShell 里（python=anaconda，带 protobuf）
# 和在 Git Bash 里（python=另一个发行版，没有 protobuf）会得出两种结果 —— 实测踩到，
# 表现为同一条用例在这儿 Passed、在那儿 Skipped。
execute_process(COMMAND "${_qz_interop_python}" -c "import sys;sys.stdout.write(sys.executable)"
                RESULT_VARIABLE _qz_exe_rc
                OUTPUT_VARIABLE _qz_interop_exe
                OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if ("${_qz_exe_rc}" STREQUAL "0" AND _qz_interop_exe)
  set(_qz_interop_python "${_qz_interop_exe}")
endif()
message(STATUS "qzrpc interop: verify_interop.py will run with '${_qz_interop_python}'")

add_test(NAME interop_protobuf
  COMMAND "${_qz_interop_python}" "${CMAKE_CURRENT_SOURCE_DIR}/verify_interop.py"
          --exe "$<TARGET_FILE:dump_wire>")
set_tests_properties(interop_protobuf PROPERTIES
  TIMEOUT 120
  # 退出码 2 = [SKIP]：runtime 找不到 protoc（或 protobuf 运行时版本过旧）。
  # 不把"验证工具的环境缺失"变成红灯，但也绝不当成通过 —— ctest 会明确打出
  # ***Skipped 并在汇总里单列。想让环境缺失变失败的门禁加 -DQZRPC_REQUIRE_INTEROP=ON。
  SKIP_RETURN_CODE 2)
