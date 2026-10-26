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
  # 超时是**护栏**，不是性能预算，也不是"环境余量"。180s 的来历：
  #   * 全部用例正常跑完是 0.9s..5s 量级，所以 180s 只可能在**真挂死**时触发；
  #   * 每个用例进 main 先打印 `== TestXxx` 并 flush，真挂住时最后一行就是凶手，
  #     ctest 的 Timeout 不会让人少知道任何信息。
  #
  # ★ 这个数字曾经是 60，而当时给它的理由是错的：说"首次执行被杀软扫描挡住"。
  #   真实原因是 **EventLoop::Loop() 抹掉了早到的 Quit()**（loop 线程一进来就 quit_=false，
  #   把"线程已创建、还没被调度到 Loop 第一行"这个窗口里别人调的 Quit() 擦掉了），
  #   于是线程永不退出、Stop() 里的 join() 永久挂死。已修（12617e8），
  #   并由 tests/test_event_loop.cpp 用确定性时序锁住。
  #   留这条注释的原因：**一个错误的环境归因，正是并发 bug 熬过一次重试的方式**。
  #   "重跑一次就好了"对任何原因都成立，所以它什么都不能证明。
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
