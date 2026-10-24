# 轻舟 RPC 文件级任务拆分表（File Map）

> 本文是**任务分配与实现顺序的唯一依据**。三个人按此表并行开工，做到「文件级不重叠」。
>
> 作者：顾图南（架构） · 版本：v1
> 契约状态：`src/**/*.h` **全部冻结**（37 个公共头 + `net/platform.h`，共 38 个头文件，已在 MSVC 14.51 下编译通过）。
> 任何人发现头文件需要改动，**必须先找顾图南**，不得自行修改——否则三人会同时改同一个契约文件而互相覆盖。

---

## 0. 角色与范围

| 成员 | 负责范围 | 文件数 | 关键交付 |
| --- | --- | --- | --- |
| **韩渡川** | 构建系统 + 单元测试 + 脚本 + `common/` 实现 + README 门面 | 7 `.cpp` + CMake + 11 测试 + 4 脚本 | 一条命令能构建、能跑测试 |
| **霍承钧** | `net/` `serialize/` `protocol/` `server/` `client/` `registry/` 全部实现 | 28 `.cpp` | 框架本体可编译、可运行 |
| **苏映棠** | `codegen/qzcodegen/`（Python）+ `generated/` 产物 + `example/` + 前端页 | 9 `.py` + `__init__.py` + 2 生成物 + 6 示例 + 1 HTML | IDL 能生成、Demo 能跑通、页面能演示 |
| **顾图南** | 全部 `docs/` + 全部 `src/**/*.h`（契约）+ `idl/*.qz` | 37 `.h` + 1 平台头 + 5 `.md` + 1 `.qz` | 契约冻结、文档可交付 |

> **并行安全性论证**：三人的写文件集合**两两不相交**；唯一的依赖是"头文件存在"（已满足）
> 与"common 实现存在"（只影响**链接**，不影响**编译**）。因此三人可以**同时开工**，
> 不需要有人先等别人的文件落地。

---

## 1. 实现顺序总览（按阶段，不按工期）

排期在这里**故意不写天数**：工期会随人、随机器、随返工次数变化，写进文档第二天就过期。
真正稳定的是**依赖顺序**——下一阶段能开工的前提，是上一阶段的哪几个文件已存在。
按阶段名读，这份表可以一直用；按天数读，一周后就得改。

五个阶段，命名刻意用**交付物形态**而不是时间：

| 阶段 | 名称 | 闸门（进入下一阶段的条件） |
| --- | --- | --- |
| P0 | **设计契约** | 37 个公共头 + `platform.h` 在 `/std:c++17 /W4 /WX /permissive-` 下零警告编译通过；`idl/echo.qz` 与 doc 04 逐字一致 |
| P1 | **内核实现** | 一个事件循环能驱动定时器与一对 socket 收发；**`test_codec` 第 1 项（37 字节 golden 串）通过**——全项目第一个"真的能跑"的里程碑；`method_id` 两端一致 |
| P2 | **运行时实现** | `generated/echo.qz.{h,cpp}` 在 `/W4 /WX` 下零警告通过；回环 echo 两端跑通；`server`/`client`/`registry` 编成静态库 |
| P3 | **示例与演示** | `scripts/run_demo` 一键起 registry + 2 个 server + client 并正常收工；演示页能看到 QPS / P99 |
| P4 | **验收** | `test_e2e_echo` 全绿（**含"同连接并发 32 请求 + 乱序回包"用例**）；压测数据入档；CI 检查项全绿；README 能让陌生人 5 分钟内跑起来 |

> **每张表的「阶段」列就是这个文件最晚可以开工的阶段**；同一阶段内**按表的行序**决定先后
> （行序已经把"谁先编译通过"排好了，比如 `C-05 buffer.cpp` 排第一是因为 net 层全部文件都要它）。

各阶段的分工（**同一个文件只出现在一个阶段**，所以三个人不会撞）：

```
P0 设计契约（顾图南）
  docs/*.md + src/**/*.h + idl/echo.qz + platform.h
  闸门：37 头 /W4 /WX 零警告；契约冻结（改动需走 §11 流程）

P1 内核实现
  韩渡川：CMakeLists.txt + common/*.cpp + tests/{test_endian,test_buffer,
          test_writer_reader,test_timer_queue,test_codec,test_frame_metadata}.cpp
  霍承钧：net/{platform,socket_util,poller,event_loop,channel,timer_queue,
          thread_pool,acceptor,connector,connection}.cpp
  苏映棠：codegen/{diagnostics,lexer,ast_nodes,parser,semantic,methodid}.py
          + tests/{test_parser,test_semantic,test_methodid}.py

P2 运行时实现
  霍承钧：serialize/*.cpp（5）+ protocol/{frame,codec}.cpp
          + server/*.cpp（4）+ client/*.cpp（5）+ registry/*.cpp（2）
  苏映棠：codegen/{naming,cpp_generator,__main__}.py + generated/echo.qz.{h,cpp}
          + tests/{test_golden.py,fixtures/kitchen_sink.qz}
  韩渡川：CI 编译门脚本（doc 04 §8.3）+ tests/{test_thread_pool,
          test_load_balancer,test_service_registry,test_dispatcher}.cpp

P3 示例与演示
  苏映棠：example/{echo_service_impl,echo_server,echo_client,registry_main}
          + example/conf/*.conf + example/web/index.html
  韩渡川：scripts/{build,run_demo}.ps1 + scripts/build.sh + example/CMakeLists.txt

P4 验收
  韩渡川：tests/test_e2e_echo.cpp（含乱序用例）+ scripts/bench.ps1
          + CI 检查项（doc 02 §8）+ README 门面 + 双平台验证
  全员：修 bug、补测试、压测数据入档
```

> 阶段之间是**闸门**关系，不是时间关系：P1 的 `test_codec` 不过，就不该开始 P2 的
> server/client——否则你会在一个错误的帧格式上写一整套编排代码，返工成本翻倍。

---

## 2. `common/` — 基础设施（负责：韩渡川）

| # | 路径 | 实现要点 | 被谁依赖 | 阶段 |
| --- | --- | --- | --- | :-: |
| C-01 | `src/common/logger.cpp` | `Logger::Impl` 持 `std::mutex + FILE*/std::ofstream`；`Write` 一次性 `fwrite` 整行；`kFatal` 写完 `abort()`；`LogMessage::~LogMessage` 调 `Write` | 全模块 | P1 |
| C-02 | `src/common/time_util.cpp` | `steady_clock` 做 `NowMs/NowUs`；测试偏移量用 `std::atomic<int64_t>`；`FormatUnixMs` 用 `localtime_s`(Win)/`localtime_r`(POSIX)，**必须在 .cpp 里 `#ifdef`** | 全模块 | P1 |
| C-03 | `src/common/status.cpp` | `StatusCodeName` 用 `switch`（不用数组，防越界）；`Status::ToString` 拼 `"Name: message"` | 全模块 | P1 |
| C-04 | `src/common/string_util.cpp` | `Fnv1a64` 逐字节；`Format` 用 `vsnprintf` 两段式（先探长度再分配）；`HexDecode` 严格校验 | codegen 交叉验证、protocol | P1 |
| C-05 | `src/common/buffer.cpp` | 扩容策略：`EnsureWritable` 时先 `Compact()`，仍不够则容量翻倍到 `max(2*cap, read+write+n)`；**例外**：`capacity()==0` 时首次扩容以 `kDefaultInitialCapacity` 为下限（否则会 3→6→12 小步增长，见 `buffer.h` 注释）；`RetrieveAll` 直接把双下标归零 | net、protocol | **P1 最先**（霍承钧一开工就需要） |
| C-06 | `src/common/config.cpp` | 逐行状态机：`[section]` / `key=value` / 整行注释 / 行内 `#`（支持 `\#` 转义）/ 可选引号；section 与 key 转小写；错误带 `line N` | server_options、client_options | P1 |
| C-07 | `src/common/metrics.cpp` | `Histogram::Observe` 用 `BitScanReverse`-等价逻辑定位桶（`while (v >>= 1) ++i`）；`Percentile` 在桶内取桶上界；`DumpText` 输出 `# HELP/# TYPE` + 值 | server、client | P1 |

> ⚠️ **C-05 是全员的编译期依赖里最紧的一个**，建议韩渡川在 P1 一开工就先做它。

---

## 3. `net/` — 网络层（负责：霍承钧）

| # | 路径 | 实现要点 | 被谁依赖 | 阶段 |
| --- | --- | --- | --- | :-: |
| N-01 | `src/net/platform.cpp` | `PlatformInit::EnsureInit` 用函数内 `static` 保证只执行一次；Win 做 `WSAStartup(MAKEWORD(2,2))`，POSIX 做 `signal(SIGPIPE, SIG_IGN)`；**这是唯一包含 `platform.h` 的地方之一** | 所有 socket 相关 .cpp | P1 |
| N-02 | `src/net/socket_util.cpp` | `ConnectWithTimeout` = 非阻塞 connect → `poll` 等可写 → `getsockopt(SO_ERROR)`；`Accept` 内部循环 `accept`；`SetKeepAlive` Win 用 `WSAIoctl(SIO_KEEPALIVE_VALS)`，POSIX 用 `TCP_KEEPIDLE/INTVL/CNT`；`LastError` 分平台取（`WSAGetLastError`/`errno`） | acceptor/connector/connection | P1 |
| N-03 | `src/net/poller.cpp` | 两个 `#ifdef` 分支：Linux `epoll_create1/epoll_ctl/epoll_wait`（`EPOLLRDHUP` 映射到 `kHup`）；Win `WSAPoll`（自维护 `std::vector<WSAPOLLFD>`，`Add/Mod/Del` 改数组并维护 `fd→index` 表）；其余平台退化到 `poll` | event_loop | P1 |
| N-04 | `src/net/timer_queue.cpp` | `AddTimer` 加锁 push；`RunExpired` 加锁弹出全部到期项到局部 vector → **解锁** → 逐个执行 → 周期性的重新入堆；`Cancel` 只往 `cancelled_` 插 id；`NextExpirationMs` 跳过已取消项 | event_loop、connection（空闲回收） | P1 |
| N-05 | `src/net/event_loop.cpp` | 唤醒对：`socket(AF_INET, SOCK_DGRAM)` 绑 `127.0.0.1:0` → `getsockname` 取端口 → 第二个 socket `connect` 到它；`HandleWakeupRead` 一次 `recv` 128 字节排空；`Loop()` 主循环 = `poller->Wait(NextExpirationMs)` → `channels_[fd]->HandleEvent()` → `RunExpired` → `DoPendingTasks`；`DoPendingTasks` 用 `swap` 取走队列（避免持锁执行回调） | 全 net、server、client | P1 |
| N-06 | `src/net/channel.cpp` | `SyncToPoller` 按 `attached_ && events_!=0` 决定 `Add/Mod/Del`；`HandleEvent` 顺序 `Error→Hup→Read→Write`；用 `handling_event_` 标记防止回调里 `Remove()` 后继续派发 | acceptor/connector/connection | P1 |
| N-07 | `src/net/thread_pool.cpp` | 入队与容量检查同锁；`Stop(drain)` 语义见头文件注释；`idle_cv_` 在 `active_workers_==0 && queue_.empty()` 时 `notify_all` 供 `Stop` 等待 | server | P1 |
| N-08 | `src/net/acceptor.cpp` | `HandleReadable` 循环 `SocketUtil::Accept` 到 `EAGAIN` 或 `accept_batch_size_`；`Stop()` 只 `DisableReading`（不 close）；`listen_fd_` 用 RAII 包装保证析构关闭 | server | P1 |
| N-09 | `src/net/connector.cpp` | 非阻塞 connect → 返回 `EINPROGRESS/WOULDBLOCK` → `Channel` 关注写 → 可写时查 `SO_ERROR`；超时定时器与成功路径**竞态**用 `finished_` 标志 + 单一收敛函数 `FinishWithXxx` 解决。**现状：客户端已改用同步 connect（见 L-03），当前无调用方**——保留给将来的后台连接预热，删掉会让 `net/` 少一块可复用件 | net（备用） | P1 |
| N-10 | `src/net/connection.cpp` | `HandleReadable`：`ReadBegin` → `::recv` 循环到 EAGAIN → `HasWritten` → `message_cb_`；`HandleWritable`：从 `output_` 发到 EAGAIN/空；高水位在 `SendInLoop` 后检查；`CloseInLoop` 保证只执行一次 | server、client | P1 |

### 3.1 `net/` 里最容易写错的三处（提前预警）

1. **`EventLoop` 的唤醒 socket 创建顺序**（N-05）：必须先 bind 再 `getsockname` 取端口，
   再创建发送端 `connect`。搞反了会在 Windows 上拿到 `WSAEADDRNOTAVAIL`。
2. **`Connector` 的成功/超时竞态**（N-09）：可写事件与超时定时器可能在同一轮循环里都触发。
   必须用 `finished_` 做单次收敛，否则会**回调两次**，释放两次 fd。
3. **`Connection::HandleReadable` 里回调可能关闭连接**（N-10）：`message_cb_` 内部可能调
   `ForceClose`，此后 `this` 的 `input_` 已失效。读完 `recv` 后再进回调，回调返回后
   **必须重新检查 `state_`**，不得继续访问成员。

---

## 4. `serialize/` — 序列化层（负责：霍承钧）

| # | 路径 | 实现要点 | 被谁依赖 | 阶段 |
| --- | --- | --- | --- | :-: |
| S-01 | `src/serialize/field_meta.cpp` | 三个 `switch`：`FieldTypeName` / `WireTypeOf` / `IsPackable`；`WireTypeOf` 对 `kFloat→kFixed32`、`kDouble→kFixed64`、`kString/kBytes/kMessage→kLengthDelimited` | 生成代码、protocol | P2 |
| S-02 | `src/serialize/writer.cpp` | varint 用 `while (v >= 0x80)` 循环；`WriteFixed32/64` **小端**（protobuf 规定）；`WriteInt32` 负数先 `static_cast<uint64_t>(int64_t(v))` 再 varint；zigzag `(n<<1)^(n>>31)`（注意 `>>` 对负数需算术右移，用 `int32_t` 保证）；`Put` 统一处理计数模式 | 生成代码、frame | P2 |
| S-03 | `src/serialize/reader.cpp` | varint 最多 10 字节、第 10 字节只能为 0/1；所有长度先比 `bytes_left()`；`SkipFieldCapture` 先记 `pos_` 再 `SkipField` 再把区间拷成 string；`Fail()` 置 `failed_` 且后续一律返回 false | 生成代码、frame | P2 |
| S-04 | `src/serialize/message.cpp` | `SerializeToString` 用局部 `Writer`；`ParseFromArray` 先 `Clear()` 再 `Decode()` 再判 `failed()/AtEnd()`；`ByteSize` 用 `Writer::Counting()` | 生成代码、protocol | P2 |
| S-05 | `src/serialize/serializer.cpp` | 薄门面，4 个函数各 3–5 行；`SerializeToBuffer` 用 `Writer` 后 `Buffer::Append(w.data(), w.size())` | protocol、rpc | P2 |

### 4.1 protobuf 兼容性的 4 个坑

1. **`fixed32/fixed64` 是小端**，而我们的帧头是大端。写反了 protobuf 解析器能读但值全错。
2. **负 `int32` 必须写成 10 字节 varint**（先符号扩展到 64 位），否则 protobuf 解出来是正数。
3. **`packed` 块的 tag wire_type 是 2，不是元素本身的 wire_type**。
4. **`string` 长度上限 2GB**，`Writer::WriteString` 要检查 `size()` 不超过 `INT32_MAX`。

---

## 5. `protocol/` — 协议层（负责：霍承钧）

| # | 路径 | 实现要点 | 被谁依赖 | 阶段 |
| --- | --- | --- | --- | :-: |
| P-01 | `src/protocol/frame.cpp` | `FrameHeader::EncodeTo/DecodeFrom` 直接用 `common/endian.h` 的 `StoreBig32/LoadBig32`；`Metadata::Encode/Decode` 是纯 TLV 循环（key/wire_type 先行校验）；`Frame::ResetXxx` 负责把 `body_len` 等字段置默认 | codec、server、client | P2 |
| P-02 | `src/protocol/codec.cpp` | `Decode` 严格按 doc 03 §8 的隐式状态机；**关键：解析 metadata 时用 `Reader` 子视图，不复制**；`Encode` 先算 `metadata.EncodedSize()` 再写 `body_len`（**不要先写头后回填**，容易忘） | server、client | P2 |

**验收线**：`tests/test_codec.cpp` 必须逐条通过 doc 03 §14 的自检项（**12 项**，其中前 9 项属于 codec 本身，第 10–12 项属于客户端/连接层语义，归 T-11）。
其中第 1 项（用 37 字节十六进制串解出正确字段）是**整个项目的第一个"真的能跑"的里程碑**。

---

## 6. `server/` — 服务端编排（负责：霍承钧）

| # | 路径 | 实现要点 | 被谁依赖 | 阶段 |
| --- | --- | --- | --- | :-: |
| V-01 | `src/server/dispatcher.cpp` | `unordered_map` + `shared_mutex`；`Register` 检测 id 冲突并回填 err；`Find` 用 `shared_lock` | server、生成代码 | P2 |
| V-02 | `src/server/service_registry.cpp` | 双索引（`by_id_` / `by_name_`）；`RegisterMethod` 先重算 `MethodIdOfName(full_name)` 比对；`DescribeMethod` 未命中回 `"unknown#0x%08x"` | server、生成代码 | P2 |
| V-03 | `src/server/server_options.cpp` | `FromConfig` 逐字段读 + 校验；`ToIniText` 输出带 `# 注释` 的完整默认配置；`Validate` 检查 `port<=65535`、线程数 ≥1、`max_frame_size <= kAbsoluteMaxFrameSize` | server | P2 |
| V-04 | `src/server/server.cpp` | **最大的一个文件**，`Impl` 内含：`Acceptor` + `vector<unique_ptr<ReactorWorker>>`（每项 = `EventLoop` + 线程 + 连接表）+ `ThreadPool` + 空闲回收定时器 + 自适应拒绝状态 + 四步停机状态机 + **独立 metrics HTTP 监听**（手写 HTTP/1.0 解析）+ **自注册线程**（`registry_endpoint` 非空时，doc 02 §2.4.1）。实现要点见 §6.1 | example、registry | P2 |

### 6.1 `server.cpp` 的五个实现要点

1. **fd 分发**：`Acceptor` 回调里 `io_threads_[next++ % N]->loop()->RunInLoop(...)`，
   在新 loop 线程里 `Connection::Create` 并塞进该 loop 的连接表（`unordered_map<int, shared_ptr<Connection>>`，**只在 loop 线程访问**）。
2. **业务任务的生命周期**：投递的闭包捕获 `shared_ptr<Connection>` + `Frame`（move）+ `seq` + `start_ms`。
   队列满时 `Submit` 返回 `kRejected` → 直接 `conn->Send(OVERLOAD 帧)`。
3. **请求超时**：worker 拿到任务先比 `NowMs() - start_ms > request_timeout_ms`，
   超时则回 `kTimeout` 且**不调业务**。业务已开始执行时无法中断，这是 C++ 的固有约束，文档已说明。
4. **过载闭环**：housekeeping 每 **500ms** 用**窗口内排队等待的均值**
   （`win_wait_sum_us / win_wait_count`）做反馈：超过 `fast_reject_threshold_ms`（默认 20ms）
   则 `reject_rate += fast_reject_adjust_rate`（默认 5，上限 99），否则减（下限 0）；
   窗口内无流量时也缓慢退让。拒绝判定是**概率**的（`FastRand()%100 < reject_rate`），
   不是硬阈值——硬阈值会在阈值附近整批抖动。队列满时另有一层**连接级背压**：
   回 `OVERLOAD` 后 `SetReadEnabled(false)` 暂停该连接的读，让 TCP 接收窗口自然收窄，
   由 housekeeping 的 `ResumePausedReads()` 恢复。
5. **四步停机**：`Stop()` 只设置原子状态并唤醒；真正的收敛在 `Wait()` 里做（轮询 `pending_ops` 之和）。
   **注意**：`Stop()` 必须能在 `housekeeping` 线程之外任意线程调用而不死锁。

---

## 7. `client/` + `registry/`（负责：霍承钧）

| # | 路径 | 实现要点 | 被谁依赖 | 阶段 |
| --- | --- | --- | --- | :-: |
| L-01 | `src/client/client_options.cpp` | `FromConfig` 解析 `endpoint_count` 与 `endpoint_i = ip:port`；`Validate` 允许"只有 registry_endpoint 而没有静态 endpoint" | channel | P2 |
| L-02 | `src/client/load_balancer.cpp` | RR：`atomic<uint64> counter_` 取模；Random：`std::mt19937_64` + `mutex`；ConsistentHash：`std::map<uint64_t, size_t>` 环 + `UpdateEndpoints` 用 `shared_mutex` 双缓冲切换 | channel、channel | P2 |
| L-03 | `src/client/connection_pool.cpp` | 每 endpoint 一个桶：`idle` / `busy` 两个 `vector` + `connecting` 计数；`Acquire` 三态（复用 / **在调用方线程同步建连** / 在 cv 上等归还）；**不用 `Connector`**（异步建连 + 延迟析构的坑详见 `connection_pool.h` 注释）；还连接时 `inflight()>0` 一律不进 `idle`；空闲回收定时器扫 `idle` 用 `last_used_ms()` | channel | P2 |
| L-04 | `src/client/registry_client.cpp` | 独立工作线程，每 `registry_poll_interval_ms` 拉一次；停机用 `stopping` 原子 + **50ms 粒度睡眠**（`kStopCheckGranularityMs`），最坏 50ms 退出——**不用 `cv.wait_for`**，因为只有"停"一个事件，加 cv 要多一把锁与广播；拉取用一次性短连接（`ConnectWithTimeout`），**不复用业务连接池**；`version` 未变且数量相同 → 跳过回调（省一次环重建）；失败 → **保留旧数据** + 置 `degraded` + `failure_total++` | channel | P2 |
| L-05 | `src/client/channel.cpp` | `Impl` 内含 `EventLoop + 线程`、`ConnectionPool`、`LoadBalancer`、`RegistryClient`；`Call` 的重试循环严格按 doc 02 §4.3 的判定矩阵 | example 生成代码 | P2 |
| R-01 | `src/registry/registry_client_proto.cpp` | 手写 9 个消息（`ServiceInstance` + 4 组请求/响应）的 `Clear/Encode/Decode/Fields`。**这是最枯燥但最有价值的一个文件**：它是"生成代码该长什么样"的可运行参照。写完它，codegen 的模板就基本确定了 | registry_server、registry_client | P2 |
| R-02 | `src/registry/registry_server.cpp` | `Impl` 内含 `rpc::Server` + 一个 `EchoServiceBase` 风格的 `RegistryServiceImpl` + TTL 清扫定时器；实例表用 `shared_mutex`；`version`（Impl 成员）在 注册 / 心跳刷新 / TTL 摘除 三处 `++` | example/registry_main | P2 |

### 7.1 `connection_pool.cpp` 的三个陷阱

1. **`Acquire` 超时后连接才建好** → 必须把新建的连接塞回 `idle` 而不是丢弃（否则连接泄漏）。
2. **`Release` 时连接已 `closed`** → 直接丢弃并递减计数，不进入 `idle`。
3. **`RemoveEndpoint` 与 `Acquire` 并发** → 用桶级 `shared_mutex`，`RemoveEndpoint` 取独占锁并 `swap` 出 `idle` 队列后在锁外关闭。

---

## 8. `codegen/` — 代码生成器（负责：苏映棠）

| # | 路径 | 实现要点 | 被谁依赖 | 阶段 |
| --- | --- | --- | --- | :-: |
| G-01 | `codegen/qzcodegen/diagnostics.py` | `Diagnostic{file,line,col,severity,message}` + `format()` → `path:line:col: error: msg`；`DiagnosticBag` 收集后统一输出 | 全部 .py | P1 |
| G-02 | `codegen/qzcodegen/lexer.py` | 手写字符扫描；输出 `Token(kind, text, line, col)`；块注释不支持嵌套（遇到 `/*` 嵌套直接报错） | parser | P1 |
| G-03 | `codegen/qzcodegen/ast_nodes.py` | `File/EnumDecl/EnumValue/MessageDecl/Field/ServiceDecl/RpcDecl`；全部 dataclass；字段带 `line/col` 供报错 | parser、semantic、generator | P1 |
| G-04 | `codegen/qzcodegen/parser.py` | 递归下降，严格按 doc 04 §3 的 EBNF；遇到 `import/extend/oneof/map/group/required/reserved` **立即报"not supported by qz v1"** | semantic、generator | P1 |
| G-05 | `codegen/qzcodegen/semantic.py` | 实现 doc 04 §3.1 的 C-01…C-12；**C-11（method_id 冲突）必须是全局的**（跨全部输入文件） | generator | P1 |
| G-06 | `codegen/qzcodegen/methodid.py` | 逐字复制 doc 03 §7.3 的实现；**不得优化、不得换哈希** | semantic、generator、test_methodid | P1 |
| G-07 | `codegen/qzcodegen/naming.py` | 类名/访问器名/guard 宏名/文件名规则；`snake_case ↔ CamelCase` | generator | P2 |
| G-08 | `codegen/qzcodegen/cpp_generator.py` | 输出 doc 04 §6.3/§6.4 的骨架；**必须逐字对齐文档里的签名**；先实现消息类，再实现 Stub/Service/Dispatcher | generated/* | P2 |
| G-09 | `codegen/qzcodegen/__main__.py` | CLI：`--input/--out/--cpp/--check`；`--check` 时只校验不落盘；退出码 0/1 | 构建脚本 | P2 |
| G-10 | `codegen/tests/test_*.py` | **4 个**测试文件（约 180 用例），见 doc 04 §8.1；`test_methodid.py` 与 `test_golden.py` 是**必做**。另需提供 `fixtures/kitchen_sink.qz` 供 CI 编译门使用（doc 04 §8.3） | CI | P1–P2 |

---

## 9. `example/` + `generated/` + 前端页（负责：苏映棠）

| # | 路径 | 实现要点 | 被谁依赖 | 阶段 |
| --- | --- | --- | --- | :-: |
| E-01 | `generated/echo.qz.h` `.cpp` | codegen 产物，**提交入库**（便于 example 直接编译、便于 golden 测试比对） | example | P2 |
| E-02 | `example/echo_service_impl.h` `.cpp` | 继承 `demo::EchoServiceBase`；`Echo` 回显 + 填 `served_by =` **实例的可辨识标识（demo 用实际监听端口）**——注意是 `server.port()` 而非构造时配置的端口（配置 `port = 0` 时只有 `Start()` 之后才知道真实值，要早于首个请求写进去）；`Notify` 只打日志 | echo_server | P3 |
| E-03 | `example/echo_server.cpp` | 读 conf → 构造 `ServerOptions` → `Server` → `RegisterEchoService` → 启动后向 `RegistryClient` 注册自己 → `Run()` → 处理 `SIGINT/SIGTERM` 走 `Stop()` | — | P3 |
| E-04 | `example/echo_client.cpp` | 两种模式：`--once` 单次调用打印结果；`--bench N T` 起 T 线程各调 N 次，输出 QPS/P50/P99 | — | P3 |
| E-05 | `example/registry_main.cpp` | 起 `RegistryServer`，打印监听端口与实例数 | — | P3 |
| E-06 | `example/conf/*.conf` | `echo_server.conf` / `echo_client.conf` / `registry.conf`（内容与 `ToIniText()` 一致） | E-03/E-04/E-05 | P3 |
| E-07 | `example/web/index.html` | 单文件页面：① 点按钮经 `/metrics` 画 QPS/P99；② 输入文本调 Echo 并显示往返耗时。**纯原生 JS + fetch，无第三方 CDN**（保持零依赖） | 演示 | P3 |

---

## 10. 构建 / 测试 / 脚本（负责：韩渡川）

| # | 路径 | 实现要点 | 阶段 |
| --- | --- | --- | :-: |
| B-01 | `CMakeLists.txt`（顶层） | C++17；`add_library(qzrpc STATIC ...)`；`target_include_directories(qzrpc PUBLIC src)`；**MSVC 必加 `/utf-8 /W4 /permissive-`**；Linux 加 `-Wall -Wextra`；Win 链接 `ws2_32`；`enable_testing()` | P1 |
| B-02 | `tests/CMakeLists.txt` | 每个 `test_*.cpp` 一个可执行 + `add_test` | P1 |
| B-03 | `example/CMakeLists.txt` | `add_subdirectory` 引入 `generated/`，链接 `qzrpc` | P2 |
| B-04 | `codegen` 的调用规则 | 自定义 target `gen_proto`：`python -m qzcodegen --input ../idl/echo.qz --out ../generated --cpp`，并让 `example` 依赖它 | P2 |
| T-01 | `tests/test_endian.cpp` | 大小端往返；未对齐指针读写 | P1 |
| T-02 | `tests/test_buffer.cpp` | 扩容/compaction/`Retrieve` 越界/`AppendFrom` | P1 |
| T-03 | `tests/test_writer_reader.cpp` | varint 边界（0/127/128/2^32-1）、zigzag、fixed 小端、负数 int32 的 10 字节 | P1 |
| T-04 | `tests/test_timer_queue.cpp` | 一次性/周期性/取消/同一轮多个到期 | P1 |
| T-05 | `tests/test_codec.cpp` | **doc 03 §14 里 codec 相关的 9 项逐条实现**，其中第 1 项用 37 字节 golden 串（第 10–12 项是客户端 seq 生命周期与 `keep_alive` 关闭语义，归 T-11） | P1 |
| T-06 | `tests/test_frame_metadata.cpp` | metadata TLV 往返 + 未知 key 跳过 + 扩展键 | P1 |
| T-07 | `tests/test_thread_pool.cpp` | 有界拒绝、`Stop(drain)`、任务全部执行完 | P2 |
| T-08 | `tests/test_load_balancer.cpp` | RR 均匀性（10k 次分布）、一致性哈希的"节点增减影响 < 1/n" | P2 |
| T-09 | `tests/test_service_registry.cpp` | 冲突检测（构造两个同 id 不同名）、SelfCheck | P2 |
| T-10 | `tests/test_dispatcher.cpp` | 注册/查找/fallback/并发查找 | P2 |
| T-11 | `tests/test_e2e_echo.cpp` | ① 起 `Server`（port=0）→ 起 `Channel` → 调 1000 次 → 校验返回值与 P99 → `Stop()` 后无泄漏；② **同连接并发 32 个请求 + 服务端故意乱序回包，客户端仍按 `seq` 正确配对：无串包、无超时误判**（覆盖 doc 03 §3.3 的 R-1/R-2/R-3 与连接池独占租约语义）；③ **seq 槽位生命周期**：服务端故意延迟到超过 deadline → 客户端拿 `kTimeout`；随后那条迟到响应到达 → 必须被丢弃（`late_response` 计数 +1）且**不影响后续同连接调用**；④ 在飞请求期间 `Server::Stop()` → 全部调用在**毫秒级**内以 `kCancelled` 返回（不是各自等满 deadline）；⑤ **`keep_alive = 0` 的收尾语义**：手写一个 `keep_alive=0` 的请求（绕过 `Channel`，直接用 `Codec` + socket）→ 必须**完整收到响应**且随后收到 FIN；断言"客户端没有走超时路径"，不是"服务端日志打了关闭"（doc 03 §4.1） | P4 |
| B-05 | `scripts/build.ps1` | 手工设 `INCLUDE/LIB`（**不要依赖 vcvars 里的 `reg.exe`，本机被安全策略禁用**）→ `cmake -G Ninja` 或 MSBuild | P1 |
| B-06 | `scripts/build.sh` | `cmake -DCMAKE_BUILD_TYPE=Release && cmake --build -j` | P1 |
| B-07 | `scripts/run_demo.ps1` | 起 registry → 起 2 个 server（不同端口）→ 起 client 跑 `--bench` → 收工 | P3 |
| B-08 | `scripts/bench.ps1` | 多线程 × 多轮，输出 QPS / P50 / P99 / 错误率表格（直接用于 README 的性能章节） | P4 |

### 10.1 CMake 的两条硬要求

```cmake
# 1) MSVC 必须加 /utf-8（源文件是 UTF-8 无 BOM 的中文注释）
if (MSVC)
  target_compile_options(qzrpc PRIVATE /utf-8 /W4 /permissive-)
endif()

# 2) Windows 必须链接 ws2_32，否则 WSAPoll/WSAStartup 全部链接失败
if (WIN32)
  target_link_libraries(qzrpc PUBLIC ws2_32)
endif()
```

---

## 11. 契约冻结清单（顾图南所有，任何人不得修改）

以下文件已冻结。**实现过程中若发现契约不足，必须走"提 issue → 顾图南改 → 全员同步"的流程。**

```
src/common/noncopyable.h  logger.h  time_util.h  endian.h  buffer.h
           status.h  config.h  string_util.h  metrics.h
src/net/poller.h  event_loop.h  channel.h  acceptor.h  connector.h
        connection.h  timer_queue.h  thread_pool.h  socket_util.h
        platform.h（唯一可含平台头，仅 .cpp 可包含）
src/protocol/protocol_const.h  frame.h  codec.h
src/serialize/writer.h  reader.h  message.h  field_meta.h  serializer.h
src/server/server.h  dispatcher.h  service_registry.h  server_options.h
src/client/channel.h  client_options.h  connection_pool.h  load_balancer.h
           registry_client.h
src/registry/registry_server.h  registry_client_proto.h
idl/echo.qz
docs/*.md
```

**冻结的验证**：上述 37 个公共头 + `platform.h`（共 38 个头文件）已在 MSVC 14.51
`/std:c++17 /utf-8 /W4 /WX /permissive-` 下全部编译通过、零警告。

---

## 12. P1 开工清单（三人的第一批文件）

> 不写"第一天"，是因为"第几天"取决于返工次数；**"先做这三个文件"才是稳定信息**。

| 谁 | 先做什么 | 完成标志 |
| --- | --- | --- |
| 韩渡川 | 写 `CMakeLists.txt` + `src/common/buffer.cpp` + `logger.cpp` + `time_util.cpp` | `cmake -B build` 成功；`test_buffer` 能编过（测试还没写也行） |
| 霍承钧 | 写 `net/platform.cpp` + `socket_util.cpp` + `poller.cpp` | 能写一个 10 行的 `main` 起监听、`curl`/`telnet` 能连上 |
| 苏映棠 | 写 `diagnostics.py` + `lexer.py` + `ast_nodes.py` | `python -c "from qzcodegen.lexer import tokenize; tokenize(open('idl/echo.qz').read())"` 能输出 token 流 |

三人在 P1 的第一批产出**互不依赖**，是真正的并行起步。
