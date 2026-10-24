# 轻舟 RPC (Qingzhou RPC) —— 单元测试
#
# 约定：**每个 test_*.cpp 一个可执行 + 一个 ctest 用例**。
# 为什么不把所有测试链接成一个可执行：一个用例崩了会拖垮其余用例，
# 而 ctest 的输出里"哪个 test 挂了"是一眼可见的。
#
# 新增测试 = 在这里登记一行 + 在测试文件里写 main()。不使用 GLOB（理由同顶层）。

set(QZRPC_TEST_SOURCES
  # 不依赖网络、不依赖序列化层的地基测试（波次 1–2）
  test_endian.cpp
  test_buffer.cpp
  # 以下待对应模块实现后自动生效（docs/00-file-map.md T-03..T-11）
  test_writer_reader.cpp
  test_timer_queue.cpp
  test_thread_pool.cpp
  test_load_balancer.cpp
  test_codec.cpp
  test_frame_metadata.cpp
  test_service_registry.cpp
  test_dispatcher.cpp
  test_event_loop.cpp
  test_e2e_echo.cpp
)

foreach(test_source IN LISTS QZRPC_TEST_SOURCES)
  if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${test_source}")
    message(STATUS "qzrpc tests: not written yet, skipped: ${test_source}")
    continue()
  endif()

  get_filename_component(test_name "${test_source}" NAME_WE)
  add_executable(${test_name} "${test_source}")
  # qzrpc 的 include 目录、/utf-8 /W4、ws2_32 都通过 PUBLIC 传递，测试无需重复声明。
  target_link_libraries(${test_name} PRIVATE qzrpc)

  # test_e2e_echo 需要一份真实的 IDL 产物（demo.EchoService 的 Stub / Dispatcher）。
  # 这里**不是**在测 codegen —— 那是 codegen_python 的事；借生成物是因为手写一个
  # serde::Message 只能测到"我自己的编解码"，测不到"生成代码 + 框架调度"这条真实链路。
  # generated/ 是生成物的 golden 基准，随仓库入库（见 README），所以它一定在；
  # 仍然做存在性判断：删掉生成物应该让这条用例被跳过，而不是让整棵树编不过。
  if (test_name STREQUAL "test_e2e_echo")
    if (NOT EXISTS "${CMAKE_SOURCE_DIR}/generated/echo.qz.cpp")
      message(STATUS "qzrpc tests: generated/echo.qz.cpp missing, test_e2e_echo skipped")
      continue()
    endif()
    target_sources(${test_name} PRIVATE "${CMAKE_SOURCE_DIR}/generated/echo.qz.cpp")
    target_include_directories(${test_name} PRIVATE "${CMAKE_SOURCE_DIR}/generated")
  endif()

  add_test(NAME ${test_name} COMMAND ${test_name})
  # 超时是**误报判据**，不是性能判据 —— 180s 这个数是这样来的：
  #   * 全部用例正常跑完是 0.9s..4.2s 量级；
  #   * 但这台机器上存在一个与代码无关的首次执行延迟：**刚链接出来/刚被写入的二进制，
  #     第一次执行会被主机实时杀软拦下来扫描**。实测把一个 exe 拷成新名字立刻跑，
  #     >70s 且**零输出**（连 main 都没进），同一文件一分钟后再跑 0.9s PASS。
  #     而"链接完成 → ctest 启动"只隔几秒，正好撞上，这恰恰是全新目录 CI 的常态路径。
  # 所以 60s 会**在最健康的那条路径上误报**，而 180s 不会漏掉任何诊断信息：
  # 每个用例进 main 先打印 `== TestXxx` 并 flush，真挂住时最后一行就是凶手。
  # 遇到"偶发超时 / 零输出"的排查顺序**先环境后代码**：单独跑一次 + 立刻重跑一次，
  # 两次都正常就是杀软扫描，不是回归。
  set_tests_properties(${test_name} PROPERTIES TIMEOUT 180)
endforeach()

# ---------------------------------------------------------------------------
# codegen 的 Python 单测也挂进 ctest：一条 `ctest` 同时覆盖 C++ 与 Python，
# 关千锤的验收不需要记两套命令。
#
# 用标准库的 unittest discover（**不用 pytest**：本机装不了第三方包）。
# 不设 PYTHONPATH：`-s codegen/tests` 会把该目录放进 sys.path，
# 测试里的 _support.py 再把 codegen/ 加进去。
# ---------------------------------------------------------------------------
if (EXISTS "${CMAKE_SOURCE_DIR}/codegen/tests")
  add_test(NAME codegen_python
    COMMAND "${QZRPC_PYTHON}" -m unittest discover -s codegen/tests
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
  set_tests_properties(codegen_python PROPERTIES TIMEOUT 300)
else()
  message(STATUS "qzrpc tests: codegen/tests not present yet, Python tests not registered")
endif()
