# 轻舟 RPC (Qingzhou RPC) —— 顶层构建
#
# 设计原则：
#   1) 零第三方依赖：全文不出现任何 find_package / FetchContent（CI 会机械检查）。
#   2) 源文件**显式登记**，不用 file(GLOB) —— 理由见下方 QZRPC_*_SOURCES 的注释。
#   3) 现在只有 common/ 时也必须能 configure 成功；net/serialize/protocol/server/
#      client/registry 由霍承钧按波次补齐，文件存在才编进去。
#   4) 公共头零平台依赖；/utf-8 是所有目标（含测试）的硬要求。

cmake_minimum_required(VERSION 3.16)

project(qzrpc VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 单配置生成器（Ninja/Makefile）没给 -DCMAKE_BUILD_TYPE 时，默认 Release：
# 本项目的验证目标有一条"P99 延迟"，Debug 下的数字没有意义。
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
               Release RelWithDebInfo Debug MinSizeRel)
endif()

option(QZRPC_BUILD_TESTS "Build unit tests (ctest)" ON)
option(QZRPC_WERROR "Treat compiler warnings as errors" OFF)

# Python 解释器与 codegen 包位置：定义在最前面，因为 tests/ 里也要用（把 codegen 的
# Python 单测挂进同一个 ctest）。刻意不用 find_package(Python3)：CMakeLists 里
# 不出现任何 find_package 是一条可机械检查的"零第三方依赖"证据。
set(QZRPC_PYTHON "python" CACHE STRING "Python 3 interpreter for the code generator")
set(QZRPC_CODEGEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/codegen" CACHE PATH "Directory containing the qzcodegen package")

# ---------------------------------------------------------------------------
# 源文件清单：**显式列出，禁止 file(GLOB)**
#
# 为什么不用 GLOB：GLOB 只在 configure 阶段展开。CI 里"拉下新提交后直接
# cmake --build"，新增的 .cpp 会被静默漏掉，症状是链接期 undefined symbol；
# 而本地（重新 configure 过）却一切正常 —— 这类问题排查成本极高。
# 显式清单的代价只有一条：**新增源文件必须登记到这里**。
#
# 每个模块的清单按 docs/02-architecture.md §6 的目录结构登记；
# 文件尚未实现时（并行开发期的常态）会被跳过并打印 STATUS，不影响 configure。
# ---------------------------------------------------------------------------
set(QZRPC_COMMON_SOURCES
  src/common/buffer.cpp
  src/common/config.cpp
  src/common/logger.cpp
  src/common/metrics.cpp
  src/common/status.cpp
  src/common/string_util.cpp
  src/common/time_util.cpp
)

set(QZRPC_NET_SOURCES
  src/net/platform.cpp
  src/net/socket_util.cpp
  src/net/poller.cpp
  src/net/timer_queue.cpp
  src/net/event_loop.cpp
  src/net/channel.cpp
  src/net/thread_pool.cpp
  src/net/acceptor.cpp
  src/net/connector.cpp
  src/net/connection.cpp
)

set(QZRPC_SERIALIZE_SOURCES
  src/serialize/field_meta.cpp
  src/serialize/writer.cpp
  src/serialize/reader.cpp
  src/serialize/message.cpp
  src/serialize/serializer.cpp
)

set(QZRPC_PROTOCOL_SOURCES
  src/protocol/frame.cpp
  src/protocol/codec.cpp
)

set(QZRPC_SERVER_SOURCES
  src/server/dispatcher.cpp
  src/server/service_registry.cpp
  src/server/server_options.cpp
  src/server/server.cpp
)

set(QZRPC_CLIENT_SOURCES
  src/client/client_options.cpp
  src/client/load_balancer.cpp
  src/client/connection_pool.cpp
  src/client/registry_client.cpp
  src/client/channel.cpp
)

set(QZRPC_REGISTRY_SOURCES
  src/registry/registry_client_proto.cpp
  src/registry/registry_server.cpp
)

set(QZRPC_SOURCES "")
foreach(module_list
    QZRPC_COMMON_SOURCES
    QZRPC_NET_SOURCES
    QZRPC_SERIALIZE_SOURCES
    QZRPC_PROTOCOL_SOURCES
    QZRPC_SERVER_SOURCES
    QZRPC_CLIENT_SOURCES
    QZRPC_REGISTRY_SOURCES)
  foreach(relative_path IN LISTS ${module_list})
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${relative_path}")
      list(APPEND QZRPC_SOURCES "${relative_path}")
    else()
      message(STATUS "qzrpc: source not implemented yet, skipped: ${relative_path}")
    endif()
  endforeach()
endforeach()

# ---------------------------------------------------------------------------
# 模块完备性判定（"这一组源文件是否**全部**落地"）
#
# 定义在顶层而不是 example/ 里，理由：tools/interop 的 dump_wire 也要用同一个
# 判定（它链接 generated/echo.qz.cpp，而那个 TU 里含客户端 Stub → 需要 client/）。
# "客户端是否可用"必须只有一个定义，两处各写一份清单迟早会漂移。
#
# 结果在顶层算好后供子目录读取（子目录里读到的是父作用域变量），
# 避免子目录之间靠"谁先被 add_subdirectory"隐式传递状态。
# ---------------------------------------------------------------------------
function(qzrpc_module_ready out_var)
  set(ready TRUE)
  foreach(module_var IN LISTS ARGN)
    foreach(relative_path IN LISTS ${module_var})
      if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${relative_path}")
        message(STATUS "  waiting for: ${relative_path}")
        set(ready FALSE)
      endif()
    endforeach()
  endforeach()
  set(${out_var} ${ready} PARENT_SCOPE)
endfunction()

qzrpc_module_ready(QZRPC_HAVE_SERVER
  QZRPC_SERVER_SOURCES QZRPC_CLIENT_SOURCES
  QZRPC_NET_SOURCES QZRPC_PROTOCOL_SOURCES QZRPC_SERIALIZE_SOURCES)
qzrpc_module_ready(QZRPC_HAVE_CLIENT
  QZRPC_CLIENT_SOURCES QZRPC_NET_SOURCES QZRPC_PROTOCOL_SOURCES QZRPC_SERIALIZE_SOURCES)
qzrpc_module_ready(QZRPC_HAVE_REGISTRY
  QZRPC_REGISTRY_SOURCES QZRPC_SERVER_SOURCES QZRPC_CLIENT_SOURCES
  QZRPC_NET_SOURCES QZRPC_PROTOCOL_SOURCES QZRPC_SERIALIZE_SOURCES)

add_library(qzrpc STATIC ${QZRPC_SOURCES})

# include 约定：编译单元统一 -I src，所有 include 写成 "common/buffer.h" 这种
# 相对 src 的路径，公共头里不出现 "../"。
target_include_directories(qzrpc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

# ---------------------------------------------------------------------------
# 编译选项
#
# /utf-8 不是可选项：所有源文件是 **UTF-8 无 BOM 的中文注释**，缺了它 MSVC 会按
# 本地代码页（936）解读，中文字符串被截断、甚至吞掉后面的标识符，报出离奇的
# 语法错误。用 PUBLIC 是刻意的：测试与 example 也必须带上它。
#
# /W4：本项目的验收标准是"零警告"，不使用任何 #pragma warning(disable) 掩盖。
# /WX 默认关闭（QZRPC_WERROR=ON 才开）：开发期不希望第三方/临时文件的小警告
# 阻塞其他人的构建，但 CI 应当打开它。
# ---------------------------------------------------------------------------
if (MSVC)
  target_compile_options(qzrpc PUBLIC /utf-8 /W4 /permissive-)
  if (QZRPC_WERROR)
    target_compile_options(qzrpc PUBLIC /WX)
  endif()
else()
  target_compile_options(qzrpc PUBLIC -Wall -Wextra)
  if (QZRPC_WERROR)
    target_compile_options(qzrpc PUBLIC -Werror)
  endif()
endif()

# ---------------------------------------------------------------------------
# 平台库
#
# Windows 必须链接 ws2_32，否则 WSAStartup / WSAPoll / getsockopt 全部链接失败。
# UNIX 直接用 -pthread 而不 find_package(Threads)：保持"零 find_package"的可检查性。
# ---------------------------------------------------------------------------
if (WIN32)
  target_link_libraries(qzrpc PUBLIC ws2_32)
elseif (UNIX)
  target_link_libraries(qzrpc PUBLIC pthread)
endif()

# ---------------------------------------------------------------------------
# 测试
# ---------------------------------------------------------------------------
if (QZRPC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# codegen 调用规则（docs/00-file-map.md B-04）
#
# 只在 codegen 的 CLI 入口存在时才注册，保证并行开发期（Python 侧还没写完）也能 configure。
# 生成物提交入库，example 直接编译已入库的产物，所以下面两个 target 都是**手动触发**的，
# 不挂在默认构建里。
#
# 三个细节是实测踩出来的（苏映棠提供，已复核）：
#   1) 必须设 PYTHONPATH=codegen：包在 codegen/qzcodegen/ 而不是仓库根，
#      直接 `python -m qzcodegen` 会 ModuleNotFoundError；
#   2) WORKING_DIRECTORY 必须是**仓库根**且不传 --source-root：生成物文件头里记的是
#      相对路径 `idl/echo.qz`，若在 codegen/ 下执行会记成 `../idl/echo.qz`，
#      而 test_golden 是逐字节比对 generated/ 的 —— 会红；
#   3) 多个 .qz 必须**一次**传入：分多次调用会让 L1（codegen 内）的跨文件
#      method_id 冲突检测退化。qzcodegen 自己展开 glob（Windows cmd 不展开）。
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# QZRPC_PYTHON / QZRPC_CODEGEN_DIR 已在文件开头定义（tests/ 也要用）。

if (EXISTS "${QZRPC_CODEGEN_DIR}/qzcodegen/__main__.py")
  # 生成：idl/*.qz -> generated/
  add_custom_target(gen_proto
    COMMAND ${CMAKE_COMMAND} -E env "PYTHONPATH=${QZRPC_CODEGEN_DIR}"
            "${QZRPC_PYTHON}" -m qzcodegen
            --input "idl/*.qz"
            --out   "generated"
            --cpp
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "qzcodegen: idl/*.qz -> generated/"
    VERBATIM)

  # 只校验不落盘，供 CI 用（退出码 0/1）
  add_custom_target(check_idl
    COMMAND ${CMAKE_COMMAND} -E env "PYTHONPATH=${QZRPC_CODEGEN_DIR}"
            "${QZRPC_PYTHON}" -m qzcodegen
            --input "idl/*.qz"
            --check
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "qzcodegen: validating idl/*.qz (no output)"
    VERBATIM)
else()
  message(STATUS "qzrpc: codegen CLI not present yet, 'gen_proto'/'check_idl' not registered")
endif()

# ---------------------------------------------------------------------------
# 生成物编译门（可选，CI 打开）：QZRPC_CHECK_CODEGEN=ON
#
# 用打满分支的 fixture（codegen/tests/fixtures/kitchen_sink.qz）生成一份 C++ 并**只编译**。
# 为什么值得单列一道门：idl/echo.qz 只覆盖 8 种字段形态，生成器在它上面可以做到
# /W4 零警告却仍然产出错误的线格式（实测踩到"packed 字段的 tag 用了 kVarint"——
# 编译过、单测过，只有对端解码时才暴露）。fixture 覆盖 11 个标量 x 4 种 label、
# [packed=false]、空消息、多级包名、嵌套类型、同文件多服务重名方法、
# INT64_MIN/UINT64_MAX 默认值。
#
# 默认 OFF：它需要 Python，且会在每次构建时跑一次；CI 应当显式打开。
# ---------------------------------------------------------------------------
option(QZRPC_CHECK_CODEGEN "Compile the codegen fixture output as a build-time gate" OFF)

if (QZRPC_CHECK_CODEGEN)
  if (NOT EXISTS "${QZRPC_CODEGEN_DIR}/qzcodegen/__main__.py")
    message(FATAL_ERROR "QZRPC_CHECK_CODEGEN=ON requires the qzcodegen package at ${QZRPC_CODEGEN_DIR}")
  endif()
  set(QZRPC_FIXTURE_DIR "${CMAKE_BINARY_DIR}/gen_fixtures")
  set(QZRPC_FIXTURE_CPP "${QZRPC_FIXTURE_DIR}/kitchen_sink.qz.cpp")
  add_custom_command(
    OUTPUT "${QZRPC_FIXTURE_CPP}" "${QZRPC_FIXTURE_DIR}/kitchen_sink.qz.h"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${QZRPC_FIXTURE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E env "PYTHONPATH=${QZRPC_CODEGEN_DIR}"
            "${QZRPC_PYTHON}" -m qzcodegen
            --input "codegen/tests/fixtures/kitchen_sink.qz"
            --out   "${QZRPC_FIXTURE_DIR}"
            --cpp
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "codegen compile gate: codegen/tests/fixtures/kitchen_sink.qz"
    VERBATIM)
  # OBJECT 库：只编译不链接，自动继承 qzrpc 的 /utf-8 /W4 /permissive-
  add_library(qzrpc_codegen_gate OBJECT "${QZRPC_FIXTURE_CPP}")
  target_include_directories(qzrpc_codegen_gate PRIVATE
                             "${CMAKE_CURRENT_SOURCE_DIR}/src" "${QZRPC_FIXTURE_DIR}")
  target_link_libraries(qzrpc_codegen_gate PRIVATE qzrpc)
endif()

# ---------------------------------------------------------------------------
# example（docs/00-file-map.md B-03）
#
# example/ 依赖 generated/ 与全部 src 模块，出现在集成波次。这里条件包含，
# 保证"只有 common"的早期阶段也能 configure 成功。
# ---------------------------------------------------------------------------
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/example/CMakeLists.txt")
  add_subdirectory(example)
else()
  message(STATUS "qzrpc: example/CMakeLists.txt not present yet, skipped")
endif()

# ---------------------------------------------------------------------------
# tools/interop：与官方 protobuf 的线格式互通验证
#
# 它不是一个"示例"，而是**验收装置**：拿第三方权威实现给自研序列化对答案。
# protobuf/protoc 只在验证期需要，缺了只跳过、不影响 configure —— 见该目录
# CMakeLists 顶部的说明。
# ---------------------------------------------------------------------------
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/interop/CMakeLists.txt")
  add_subdirectory(tools/interop)
else()
  message(STATUS "qzrpc: tools/interop/CMakeLists.txt not present yet, skipped")
endif()
