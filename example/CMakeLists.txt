# 轻舟 RPC —— example / Demo（docs/00-file-map.md B-03）
#
# 生成物（generated/echo.qz.cpp）**直接编进每个可执行文件**，不做成独立库：
#   1) 生成物是 IDL 的产物，不属于框架本体；
#   2) 多个 exe 共享同一个生成物库时容易重复链接同一个 .obj（LNK2005 / duplicate symbol）。
# 依赖关系仍然显式（见下方 QZRPC_REGENERATE_ON_BUILD）。
#
# 每个可执行文件按**模块完备性**条件创建：client/ 与 registry/ 由霍承钧按波次补齐，
# 某个模块还没落地时对应的 exe 只打印 STATUS 并跳过，不影响其余目标与 ctest。

option(QZRPC_REGENERATE_ON_BUILD "Run codegen before building examples (needs Python)" OFF)

# 模块完备性判定（qzrpc_module_ready）与 QZRPC_HAVE_SERVER / _CLIENT / _REGISTRY
# 三个结果都由**顶层 CMakeLists.txt** 用同一份 QZRPC_*_SOURCES 清单算出。
# 这里不再重复定义：tools/interop 也要问"客户端可用吗"，两处各写一份必然漂移。

function(qzrpc_add_example target_name)
  add_executable(${target_name} ${ARGN})
  # generated/ 单独进 include 路径：生成物之间用裸文件名互引（#include "echo.qz.h"）
  target_include_directories(${target_name} PRIVATE "${CMAKE_SOURCE_DIR}/generated")
  target_link_libraries(${target_name} PRIVATE qzrpc)
  # 生成物已入库，所以默认**不**挂 add_dependencies(gen_proto)：挂上去会让每次构建
  # 都跑一遍 Python 并重写 generated/，既破坏增量构建，也让没装 Python 的机器构建不了
  # example。改了 idl/ 之后请手动 `cmake --build build --target gen_proto`；
  # 需要自动化的机器（含 CI）打开 -DQZRPC_REGENERATE_ON_BUILD=ON。
  if (QZRPC_REGENERATE_ON_BUILD AND TARGET gen_proto)
    add_dependencies(${target_name} gen_proto)
  endif()
endfunction()

if (NOT EXISTS "${CMAKE_SOURCE_DIR}/generated/echo.qz.cpp")
  message(STATUS "qzrpc example: generated/echo.qz.cpp missing "
                 "(run: cmake --build <build> --target gen_proto); skipped")
  return()
endif()

# ⚠️ 为什么 echo_server 也要等 client/ 落地：
# generated/echo.qz.cpp 是**单文件单 TU**，里面同时有服务端 ServiceBase/Dispatcher 和
# 客户端 EchoServiceStub；Stub::Echo 的实现会调 rpc::Channel::Call，所以链接这个 TU
# 就必然需要 src/client/channel.cpp。实测报错：
#   echo.qz.cpp.obj : error LNK2019: 无法解析的外部符号 rpc::Channel::Call(...)
# 换句话说"生成物"把 server 与 client 绑在一起了 —— 见交付报告里给苏映棠的建议
# （按 client/server 拆成两个 TU 可以解耦，但那是 codegen 侧的改动）。
# 这正是顶层 QZRPC_HAVE_SERVER 的定义方式（含 QZRPC_CLIENT_SOURCES）。

set(QZRPC_EXAMPLE_GENERATED "${CMAKE_SOURCE_DIR}/generated/echo.qz.cpp")

# echo_server：业务实现 + 生成的消息/Stub + 框架
if (QZRPC_HAVE_SERVER)
  qzrpc_add_example(echo_server
    echo_server.cpp
    echo_service_impl.cpp
    "${QZRPC_EXAMPLE_GENERATED}")
else()
  message(STATUS "qzrpc example: echo_server skipped (server+client/net/protocol/serialize incomplete)")
endif()

# echo_client：--once 单次调用 / --bench N T 压测
if (QZRPC_HAVE_CLIENT)
  qzrpc_add_example(echo_client
    echo_client.cpp
    "${QZRPC_EXAMPLE_GENERATED}")
else()
  message(STATUS "qzrpc example: echo_client skipped (client incomplete)")
endif()

# registry_main：用本框架自举的注册中心
if (QZRPC_HAVE_REGISTRY)
  qzrpc_add_example(registry_main
    registry_main.cpp
    "${QZRPC_EXAMPLE_GENERATED}")
else()
  message(STATUS "qzrpc example: registry_main skipped (registry incomplete)")
endif()
