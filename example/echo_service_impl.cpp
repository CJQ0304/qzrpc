// 轻舟 RPC 示例：Echo 服务的业务实现
// example/echo_service_impl.cpp

#include "echo_service_impl.h"

#include <utility>

#include "common/logger.h"

namespace demo {

EchoServiceImpl::EchoServiceImpl(std::uint16_t listening_port) {
  port_.store(listening_port, std::memory_order_relaxed);
}

EchoServiceImpl::~EchoServiceImpl() = default;

qzrpc::Status EchoServiceImpl::Echo(const EchoRequest& request, EchoResponse* response) {
  // response 一定非空（生成的 handler 在栈上构造后传进来），但防御性检查比崩溃划算。
  if (response == nullptr) {
    return qzrpc::Status::InvalidArgument("EchoServiceImpl::Echo: response is null");
  }
  echo_total_.fetch_add(1, std::memory_order_relaxed);

  response->set_message(request.message());
  response->set_served_by(
      static_cast<std::int32_t>(port_.load(std::memory_order_relaxed)));

  // 下面这段不是为了业务，而是为了让"字段真的过来了"这件事在日志里可见：
  // 演示时把 client 的 --verbose 打开就能看到 repeated / optional / enum 都活着。
  if (request.has_body() || request.ids_size() > 0 || request.has_lang()) {
    QZ_LOG_DEBUG << "echo from " << request.message() << ": ids=" << request.ids_size()
                 << " lang=" << (request.has_lang() ? request.lang() : std::string("<unset>"))
                 << " body.weight=" << (request.has_body() ? request.body().weight() : -1)
                 << " site=" << SiteType_Name(request.site());
  }
  return qzrpc::Status::Ok();
}

qzrpc::Status EchoServiceImpl::Notify(const EchoRequest& request, EchoResponse* response) {
  if (response == nullptr) {
    return qzrpc::Status::InvalidArgument("EchoServiceImpl::Notify: response is null");
  }
  notify_total_.fetch_add(1, std::memory_order_relaxed);

  QZ_LOG_INFO << "[port " << port_.load(std::memory_order_relaxed)
              << "] Notify: " << request.message();
  // 非幂等方法也要回一个可用响应，否则客户端拿到的是空 body。
  response->set_message("notified");
  response->set_served_by(
      static_cast<std::int32_t>(port_.load(std::memory_order_relaxed)));
  return qzrpc::Status::Ok();
}

}  // namespace demo
