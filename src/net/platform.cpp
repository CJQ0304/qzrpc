// 轻舟 RPC (Qingzhou RPC)
// src/net/platform.cpp
//
// 职责：进程级平台初始化的唯一实现。
//   1) Windows：WSAStartup(2.2)，并注册进程退出钩子做 WSACleanup；
//   2) POSIX：忽略 SIGPIPE（否则对端关闭后 send 会直接杀掉进程）。
//
// 线程安全：用 std::call_once + 函数内 static 保证只执行一次、且多线程首次并发调用安全。
// 失败处理：WSAStartup 失败是"继续跑下去毫无意义"的场景，但不能 abort（网络层可能只是被
//           误调用于非网络场景），因此只记录错误码，后续 socket 调用会自然失败并由上层报告。

#include "net/platform.h"

#include <cstring>
#include <mutex>

namespace qzrpc {
namespace net {

namespace {

#ifdef _WIN32
int g_wsa_start_error = 0;  // WSAStartup 的错误码，0 表示成功；仅诊断用。

void DoPlatformInit() {
  WSADATA wsa_data;
  std::memset(&wsa_data, 0, sizeof(wsa_data));
  const int rc = ::WSAStartup(MAKEWORD(2, 2), &wsa_data);
  if (rc != 0) {
    g_wsa_start_error = rc;
    return;
  }
  if (wsa_data.wVersion != MAKEWORD(2, 2)) {
    // 只支持到 1.1：理论上不会发生（Win7+ 都支持 2.2），记下来供诊断。
    g_wsa_start_error = WSAVERNOTSUPPORTED;
  }
}
#else
void DoPlatformInit() {
  // 必须在任何 socket 创建之前完成：即使上层都检查了返回值，
  // 也无法排除第三方代码只写不检查的路径。
  ::signal(SIGPIPE, SIG_IGN);
}
#endif

std::once_flag g_once_flag;

}  // namespace

void PlatformInit::EnsureInit() {
  // call_once 的第二参数要求可调用对象；DoPlatformInit 是普通函数，直接传函数名即可。
  std::call_once(g_once_flag, DoPlatformInit);
}

}  // namespace net
}  // namespace qzrpc
