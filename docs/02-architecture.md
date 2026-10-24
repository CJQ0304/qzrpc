# 轻舟 RPC 架构设计

> 本文回答三个问题：**系统由哪些模块组成、依赖往哪个方向流、一次 RPC 在哪些线程上跑了哪些代码。**
>
> 作者：顾图南（架构） · 版本：v1 · 前置阅读：[`01-phxrpc-analysis.md`](01-phxrpc-analysis.md)

---

## 1. 设计约束（不可协商）

| # | 约束 | 直接后果 |
| --- | --- | --- |
| K-01 | C++17，**零第三方依赖**（只用标准库 + 平台 API） | 序列化、网络、线程池、指标、IDL 解析器全部自研 |
| K-02 | 自研 `.qz` IDL + Python codegen，不依赖 protoc | 生成物直接是"可读的 C++"，而不是 protobuf 的 `.pb.h` |
| K-03 | payload 与 protobuf wire format 兼容 | 序列化层必须精确实现 varint/zigzag/packed/未知字段透传 |
| K-04 | Windows(MSVC 14.51) 与 Linux(GCC) 双目标 | 网络层必须抽象；公共头文件零平台依赖 |
| K-05 | 不做 ucontext 协程 | 线程模型定为「主从 Reactor + 业务线程池」 |
| K-06 | 范围锁定「核心框架 + 可跑 Demo」 | 砍掉流式 RPC、TLS、压缩算法、多进程 |

---

## 2. 分层架构

```
┌──────────────────────────────────────────────────────────────────────────┐
│ L5  应用层                                                                │
│     example/echo_server.cpp   example/echo_client.cpp                     │
│     generated/echo.qz.h        example/echo_service_impl.cpp              │
│     （业务实现只依赖 ServiceBase / Stub / Channel）                        │
└───────────────────────────────┬──────────────────────────────────────────┘
                                │
┌───────────────────────────────▼──────────────────────────────────────────┐
│ L4  服务编排层                                                            │
│   server/         server.h(Server, pimpl)  dispatcher.h  service_registry  │
│   client/         channel.h(Channel, pimpl) load_balancer  connection_pool  │
│                   registry_client.h                                       │
│   registry/       registry_server.h(RegistryServer, pimpl)                 │
└───────────────────────────────┬──────────────────────────────────────────┘
                                │
┌───────────────────────────────▼──────────────────────────────────────────┐
│ L3  协议层                                                                │
│   protocol/   protocol_const.h   frame.h(FrameHeader/Metadata/Frame)       │
│               codec.h(Codec: 粘包拆包 + 校验)                              │
│   registry/registry_client_proto.h（注册中心消息，唯一"横切"的低层文件）    │
└───────────────────────────────┬──────────────────────────────────────────┘
                                │
┌───────────────────────────────▼──────────────────────────────────────────┐
│ L2  序列化层                                                              │
│   serialize/  writer.h  reader.h  message.h  field_meta.h  serializer.h    │
│   （只认识字节与 Message，不认识帧、不认识 socket、不认识线程）              │
└───────────────────────────────┬──────────────────────────────────────────┘
                                │
┌───────────────────────────────▼──────────────────────────────────────────┐
│ L1  网络层                                                                │
│   net/  poller.h  event_loop.h  channel.h  acceptor.h  connector.h         │
│         connection.h  timer_queue.h  thread_pool.h  socket_util.h          │
│         platform.h（**唯一允许出现平台头的头文件，仅 .cpp 可包含**）         │
└───────────────────────────────┬──────────────────────────────────────────┘
                                │
┌───────────────────────────────▼──────────────────────────────────────────┐
│ L0  基础设施                                                              │
│   common/  noncopyable  logger  time_util  endian  buffer  status          │
│           config  string_util  metrics                                    │
└──────────────────────────────────────────────────────────────────────────┘
```

> 层号 ≠ 目录名。真正的分层判据是**依赖方向**：允许 `Ln → Lm` 当且仅当 `m < n`，
> 同层之间**不允许**互相依赖（`server/` 不依赖 `client/`，`net/` 不依赖 `serialize/`）。

### 2.1 依赖矩阵（✓ = 允许依赖）

| ↓依赖者 \ 被依赖→ | common | net | serialize | protocol | registry_proto | server | client | registry |
| --- | :-: | :-: | :-: | :-: | :-: | :-: | :-: | :-: |
| **common** | — | | | | | | | |
| **net** | ✓ | — | | | | | | |
| **serialize** | ✓ | | — | | | | | |
| **protocol** | ✓ | | ✓ | — | | | | |
| **registry_proto** | ✓ | | ✓ | ✓ | — | | | |
| **server** | ✓ | ✓ | ✓ | ✓ | | — | | |
| **client** | ✓ | ✓ | ✓ | ✓ | ✓ | | — | |
| **registry(server)** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | | — |
| **example** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |

**图中没有任何对称的 ✓ 对**，因此依赖图必然无环。这是可机械检查的（见 §8 的 CI 检查项）。

### 2.2 模块职责表

| 模块 | 目录 | 职责 | 明确不负责 |
| --- | --- | --- | --- |
| 基础设施 | `src/common/` | 日志、时间、字节序、缓冲区、状态码、INI 读取、字符串/哈希、指标 | 不认识 socket、不认识 RPC |
| 网络层 | `src/net/` | IO 多路复用、事件循环、定时器、连接状态机、监听/连接、线程池 | 不认识 protobuf、不认识帧格式 |
| 序列化层 | `src/serialize/` | protobuf 兼容的编解码、`Message` 抽象、字段元数据 | 不认识帧、不认识 RPC 语义 |
| 协议层 | `src/protocol/` | 帧结构、metadata TLV、粘包拆包、恶意包校验 | 不认识业务类型、不认识连接 |
| 服务端编排 | `src/server/` | 组装 Reactor/线程池/连接表、方法路由、优雅停机、服务端指标 | 不实现业务逻辑 |
| 客户端编排 | `src/client/` | 负载均衡、连接池、重试、超时、服务发现、客户端指标 | 不实现业务逻辑 |
| 注册中心 | `src/registry/` | 注册中心自身（用本框架实现）+ 注册中心消息定义 | 不做健康检查探活 |
| 代码生成 | `codegen/qzcodegen/` | `.qz` 解析、语义校验、生成 C++ 消息/Stub/Service/Dispatcher | 不生成构建脚本 |
| 示例 | `example/` | 端到端可跑的 Echo + Registry Demo | 不是库的一部分 |

### 2.3 客户端配置项的边界值（`client_options.cpp` 是唯一真相）

三处口径必须同时成立，否则会出现"配置文件写了但静默跑了默认值"这类最难查的问题：

- **`FromConfig`**：缺失 → 保留默认值；**存在但非法（非数字/越界）→ 报错，整份配置失败**。
  不夹紧、不静默、不回退默认。
- **`Validate`**：兜住"代码里直接赋值绕过 `FromConfig`"的路径，也是 `Channel::valid()` 的依据。
- **本表**：上面两者的可读镜像。**改任何一个数必须同步改本表**（否则文档就开始骗人）。

| 字段 | 默认值 | `FromConfig` 区间 | `Validate` 要求 | 备注 |
| --- | --- | --- | --- | --- |
| `service_name` | `"qzrpc-service"` | 任意字符串 | **非空** | 空串会让注册中心查询与 metrics label 全部失义 |
| `endpoints` | 空 | `endpoint_count ∈ [0,4096]`；每项 `ip:port`，`port ∈ [1,65535]` | 每项 `ip` 非空、`port != 0` | 只接受**IP 字面量**（不做 DNS），IPv6 写 `[::1]:16161`。注意：出现 `endpoint_count` 才**整体重建**列表；只写 `endpoint_i` 是**增量覆盖**（不清空代码里预设的值） |
| `registry_endpoint` | `""` | 空 或 `ip:port` | 非空时必须可解析 | 与 `endpoints` **至少有一个**非空 |
| `caller_name` | `""` | 任意字符串 | 不校验 | 空则不带 `caller` metadata |
| `connect_timeout_ms` | 200 | `[1, 600000]` | `>= 1` | |
| `request_timeout_ms` | 1000 | `[1, 3600000]` | `>= 1` | 可被 `CallOptions::timeout_ms` 逐次覆盖 |
| `max_retries` | 2 | `[0, 20]` | `>= 0` | **只放大三类失败**：请求未发出 / `kOverload` / 拿得到响应帧的 `kServiceUnavailable`；已发出且结果未知的失败**仅幂等**才重试（§4.3）。**没有"不计入次数"的故障转移预算**——换实例也占一次。默认 2 是安全的，别改 0 |
| `retry_backoff_ms` | 20 | `[0, 60000]` | `>= 0` | 第 n 次重试等 `n * retry_backoff_ms` |
| `acquire_timeout_ms` | 200 | `[1, 600000]` | `>= 1` | 借连接等待上限。**建议 `request_timeout_ms` 的 1/5**（默认 200:1000）：借不到要尽快暴露，别把整份预算耗在排队上（§5.2） |
| `connections_per_endpoint` | 2 | `[1, 1024]` | `!= 0` | **已知宽松点**：代码直接赋 `>1024` 不会被拒（只拒 0）。**这是全表最容易配错的一项**，容量账见下 |
| `idle_connection_timeout_ms` | 60000 | `[0, 86400000]` | `>= 0` | `0` = 不回收 |
| `enable_nodelay` | `true` | `1/true/yes/on`、`0/false/no/off/空` | 不校验 | 其他文本报错 |
| `max_frame_size` | 16 MiB | `[4, 64 MiB]` | `[4, 64 MiB]` | 与 decode 侧同一把尺子（doc 03 §9）；上下界都是 `protocol_const.h` 的常量 |
| `breaker_failure_threshold` | 5 | `[1, 1000]` | `>= 1` | ⚠️ **未被读取**：冷却当前是"**首次**建连失败就进"（§4.3.4），没有"连续 N 次"这个门槛。要么接上它，要么删掉 |
| `breaker_cooldown_ms` | 5000 | `[1, 600000]` | `>= 1` | ⚠️ **未被读取，且默认值与实际值不同**：冷却基数硬编码为 **3000**（`kCooldownBaseMs`），序列 3s→6s→12s→24s→30s。配 500 也是 3s（§4.3.4） |
| `registry_poll_interval_ms` | 3000 | `[100, 3600000]` | `>= 100` | 下限 100 ms，防止把注册中心拉成 DDoS |
| `registry_request_timeout_ms` | 1000 | `[1, 600000]` | `>= 1` | |
| `enable_metrics` | `true` | bool 同上 | 不校验 | |
| `auto_trace_id` | `true` | bool 同上 | 不校验 | |

四条组合层面的结论（都不在 `Validate` 里做交叉校验，理由见下）：

1. **唯一非法的目标组合是"`endpoints` 与 `registry_endpoint` 都为空"**。以下三种都合法：
   只有静态列表（最简用法）、只有注册中心（首跳靠发现）、两者都有（注册中心挂了走兜底）。
2. **`port = 0` 对客户端非法**。服务端 `port = 0` 是"随机端口"（`Start()` 后回填），
   客户端 `0` 则是"连不上"——所以 `Validate` 单独拒它，而不是靠 `[1,65535]` 兜。
3. **不做交叉校验**（如 `request_timeout_ms < connect_timeout_ms`）。这类组合"可疑但合法"，
   拒绝它只会让用户把两个数改成一样大来绕过检查，问题并没消失。真正的保护是
   `Channel::Call` 把**每次尝试的等待上限收敛到剩余 deadline**（§4.3 门禁 3），
   所以 `connect + acquire + 一次 RTT` 超预算时是被截断，而不是被配置校验拦下。
4. **不做 IP 字面量的合法性校验**（`999.999.999.999` 能通过 `Validate`）。`client_options.cpp`
   不引 socket 头，也不该为一个字符串检查新增 `socket_util` 的解析 API——那会把
   "读配置"和"连网络"重新耦合起来。这类输入在建连时以 `kServiceUnavailable` + OS 错误串
   暴露，**可诊断**；把错误提前到配置阶段的收益不值这个跨层依赖。

#### 2.3.1 `connections_per_endpoint` 的容量账（最容易配错的一项）

前提：**一条连接同一时刻只承载一个在飞调用**——池的借用语义是"一个 `Lease` 独占一条连接"
（`connection_pool.h` 的三步 `Acquire`，以及 `Release` 里 `inflight() > 0` 就不回空闲队列）。
所以 endpoint 的并发度就是连接数，超出的调用在池的条件变量上排队。

记线程数 `T`、连接数 `N`、单次 RTT `R`，则**最坏情况下的借连接等待**约为：

```
acquire_wait ≈ R × (⌈T / N⌉ - 1)
```

要它小于 `acquire_timeout_ms`（默认 200），否则**用户会看到 `kOverload`（"连接池饱和"），
而服务端完全空闲**——而且这个 `kOverload` 按 §4.3.2 的门禁**不重试、也不换实例**，
对非幂等调用就是一次硬失败。这是"客户端自己把自己的容量不足报成下游过载"。

用实测数据把这笔账算完（默认值 `T = 8` 个业务线程、`N = 2`、`R = 50ms`）：

| 线程数 | 每连接串几个 | 最坏 acquire 等待 | 结果 |
| --- | --- | --- | --- |
| 4 | 2 | 50 ms | 舒适 |
| 8 | 4 | 150 ms | 贴着 200 ms 上限，能过 |
| 9 | 5 | 200 ms | **溢出**，第 9 个线程开始零星 `kOverload` |

也就是说：**默认配置在 RTT ≈ 70 ms 的下游面前就已经不够用了**（`3 × 70 = 210 > 200`），
而拉美/跨洋链路或下游一次慢查询就能到这个量级。

选型的顺序应该是：
1. 先把 `connections_per_endpoint` 配到 `≥ T`（一条连接一个线程，池退化成"每线程一条"），
   这是最不会出错的配置，代价只是每个 endpoint 最多 `T` 条 fd；
2. 想省 fd 才往下压，同时按上面的公式**反算** `acquire_timeout_ms` 并同步调大；
3. `N` 压到 1 时必须有意识地接受"这条 endpoint 完全串行"——QPS 上限就是 `1 / R`，
   此时 `T` 再大也只是让更多线程在池里排队，而不是提高吞吐。

### 2.4 服务端配置项的边界值（`server_options.cpp` 是唯一真相）

与 §2.3 同构，只是这张表里的"坑"更多——**有几项 `Validate` 根本不查**（下表标注）。
"不校验"不等于"没有行为"：`registry_*` 两项虽然不校验，但**有完整的运行时行为**
（§2.4.1），这是本节最容易被误读的地方。

| 字段 | 默认值 | `Validate` 要求 | 备注 |
| --- | --- | --- | --- |
| `bind_ip` | `"0.0.0.0"` | 必须是 IP **字面量** | 域名被拒绝：绑定的必须是确定的本地地址，解析结果不可预测 |
| `port` | `kDefaultPort` | `uint16` 天然 `<= 65535` | `0` = 随机端口，`Start()` 后由 `server.port()` 回填（示例的 `served_by` 就靠它） |
| `backlog` | 1024 | `>= 1` | |
| `io_thread_count` | 4 | `[1, 256]` | 从 Reactor 数量。**不是越大越好**：每条连接固定归属一个 loop，多开只增加切换成本 |
| `worker_thread_count` | 16 | `[1, 1024]` | 业务线程数，决定真正的并发度 |
| `max_pending_tasks` | 20000 | **不校验** | 线程池有界队列容量；传 0 的行为由 `ThreadPool` 决定（不建议依赖） |
| `max_connections` | 10000 | `>= 1` | 超过后新连接被拒绝 |
| `max_frame_size` | 16 MiB | `[4, 64 MiB]` | 与客户端同一把尺子；超上限会让"恶意大 `body_len` 预分配"的防护失效（doc 03 §9） |
| `idle_timeout_ms` | 0 | `>= 0` | 0 = 不回收空闲连接 |
| `max_accept_batch` | 64 | `>= 1` | 单次可读事件最多 accept 多少个，防"疯狂 accept 饿死其它 fd" |
| `enable_nodelay` | `true` | 不校验 | RPC 场景关掉它基本没有正当理由 |
| `request_timeout_ms` | 3000 | `>= 1` | **全框架唯一的业务超时判定点**（§4.2 步骤 6） |
| `enable_adaptive_reject` | `true` | 不校验 | 关掉后 `reject_rate` 恒为 0，退化成"只靠队列满兜底" |
| `fast_reject_threshold_ms` | 20 | `>= 0` | **纯排队**均值（`queue_wait_us`，不含业务执行时间）超过它就提高拒绝率（§4.4 第 2 层、§5.3.1） |
| `fast_reject_adjust_rate` | 5 | `[1, 50]` | 每次调整的步长（百分点） |
| `enable_metrics` | `true` | 不校验 | |
| `metrics_port` | 0（关闭） | `!= 0` 时：`metrics_bind_ip` 必须是字面量，且 **不得与 `(bind_ip, port)` 相同** | 同址同端口必然 `EADDRINUSE`，提前拦住比启动时抛错好排查 |
| `service_name` | `"qzrpc-server"` | 非空 | 日志与注册用 |
| `graceful_shutdown_timeout_ms` | 5000 | `>= 0` | 排空在飞请求的上限（§5.1 步骤 3） |
| `log_level` | 2 | `[0, 6]`（`kTrace`..`kOff`） | 语义是"输出 `>= N`" |
| `registry_endpoint` | `""` | **不校验**（`FromConfig` 也不校验格式） | 空 = 不自注册。非空时必须是 `ip:port`，否则自注册线程打 `ERROR` 后**退出该线程**（服务照常提供，只是没有发现性）。实际契约见 §2.4.1 |
| `registry_heartbeat_interval_ms` | 5000 | **不校验** | 实际生效值被**夹紧到 `[100, 60000]`**；TTL 固定 15000，心跳取 TTL 的 1/3 是设计意图（§2.4.1） |

#### 2.4.1 自注册（`registry_endpoint`）的实际契约

> 这一小节的存在原因：这两个字段曾经被写成"死配置 / `Server` 不消费"——
> **那是某一时刻的快照，现在是错的**。凡是"未使用 / 零引用 / 已废弃"这类断言都有保质期，
> 写的时候必须带验证时刻，并且交付前复查。下面的每一条都对应 `server.cpp` 里的可指认代码。

自注册由**独立线程**完成（`Server::Impl::RegistryLoop`），在监听成功**之后**启动——
这正是 `port = 0` 能用的原因：只有 `Start()` 完成后才知道真实端口。

| 环节 | 实际行为 | 配错时的表现 |
| --- | --- | --- |
| 启动条件 | `registry_endpoint` 非空才起线程 | 空 = 完全不自注册（静态 endpoint 部署） |
| 地址解析 | 必须是 `ip:port`；解析失败 → `ERROR` + 线程退出 | 服务正常，但注册表里永远没有它 |
| 对外宣告地址 | 取 `bind_ip`；若为 `""` / `"0.0.0.0"` / `"::"` → **退化成 `127.0.0.1` 并 `WARN`** | 跨机部署配了通配地址，注册成功但客户端连不上 |
| 对外宣告端口 | 取实际监听端口（`port = 0` 也能正确登记） | — |
| 首次注册 | 失败**不让服务启动失败**：`WARN` + 固定 2s 后重试，直到成功 | 注册中心比服务起得晚是常态，这是刻意的 |
| 心跳 | 每 `clamp(registry_heartbeat_interval_ms, 100, 60000)` 一次；租约 TTL 固定 15s | 心跳 > TTL 会自我摘除后反复重注册 |
| 被摘除后的自愈 | 心跳返回 `not_found`（TTL 到期或注册中心重启）→ **回到未注册态并重新登记** | 不自愈的话会"自以为活着"，而客户端早已拉不到它 |
| 心跳失败 | `WARN` + 退到 2s 重试间隔（不打成高频重试） | — |
| 停机 | 发一次 `Deregister`（尽力而为）；失败只 `WARN`，由 TTL 兜底 | 停机后实例残留至多 15s |
| 退出粒度 | 等待用 100ms 切片，`Stop()` 后最迟 100ms 退出，不拖住 `Wait()` | — |
| 相关指标 | `qzrpc_server_registry_register_total` / `_heartbeat_total` / `_errors_total` | 见 §5.3.1 |

两条**部署约束**（不是 bug，是取舍）：

1. **自注册不影响本机提供服务**。注册中心全挂时服务照跑，只是新客户端发现不到它；
   已有客户端按旧 endpoint 列表继续工作。这是"控制面故障不扩大为数据面故障"。
2. **跨机部署必须显式配置 `bind_ip`**。用 `0.0.0.0` 时注册的是 `127.0.0.1`——本机客户端
   能连上、其它机器不能，这是最难查的一类"部分可用"。


---

## 3. 线程模型

一个 `rpc::Server` 实例的线程清单（默认配置 `io_thread_count=4, worker_thread_count=16`）：

```
                      ┌──────────────────────────────────────────────┐
                      │           主 Reactor 线程 × 1                 │
                      │  EventLoop(主) + Acceptor                    │
                      │  职责：accept → 轮询选一个从 Reactor → 投递 fd  │
                      │  绝不做：读业务数据、解码、调业务                │
                      └───────────────┬──────────────────────────────┘
                                      │  new_fd（轮询分配 Round-Robin）
        ┌───────────────┬─────────────┴───────┬───────────────┐
        ▼               ▼                     ▼               ▼
┌───────────────┐ ┌───────────────┐   ┌───────────────┐ ┌───────────────┐
│ 从 Reactor #0 │ │ 从 Reactor #1 │ … │ 从 Reactor #2 │ │ 从 Reactor #3 │   × N (=4)
│ EventLoop     │ │ EventLoop     │   │ EventLoop     │ │ EventLoop     │
│ Poller(epoll/ │ │               │   │               │ │               │
│  WSAPoll)     │ │               │   │               │ │               │
│               │ │               │   │               │ │               │
│ 一批连接：     │ │               │   │               │ │               │
│  读字节→拆帧   │ │               │   │               │ │               │
│  →反序列化     │ │               │   │               │ │               │
│  →投递业务任务 │ │               │   │               │ │               │
│  →写回响应     │ │               │   │               │ │               │
│               │ │               │   │               │ │               │
│ 独占访问：     │ │               │   │               │ │               │
│  Connection   │ │               │   │               │ │               │
│  的 in/out buf │ │               │   │               │ │               │
└───────┬───────┘ └───────────────┘   └───────────────┘ └───────────────┘
        │ ThreadPool::Submit(闭包)
        ▼
┌──────────────────────────────────────────────────────────────────────┐
│ 业务 Worker 线程池 × M (=16)                                          │
│  固定线程数 + **有界**队列（max_pending_tasks）                        │
│  队列满 → Submit 返回 kRejected → 从 Reactor 立即回 OVERLOAD 帧         │
│  职责：反序列化已完成，只跑业务逻辑 + 序列化响应                        │
│  约束：**允许**阻塞（磁盘/下游 RPC），但不得访问 Connection 的 buffer     │
└───────────────────────────────┬──────────────────────────────────────┘
                                │ conn->Send(resp_bytes)（线程安全，内部转投）
                                ▼
                     回到该连接所属的从 Reactor 线程写回
┌──────────────────────────────────────────────────────────────────────┐
│ 后台线程（housekeeping）× 1  每 500ms 醒一次，做三件事（顺序固定）   │
│  ① AdjustRejectRate()   自适应拒绝比例调节（排队时延闭环）           │
│  ② ReapIdleConnections() 空闲连接回收（idle_timeout_ms，默认 0=关）  │
│  ③ ResumePausedReads()  恢复被过载背压暂停的读（§4.4 第 3 层）       │
└──────────────────────────────────────────────────────────────────────┘
```

**合计线程数 = 1 (acceptor) + N (io) + M (worker) + 1 (housekeeping)**，默认 `4 + 16` 时是 **22 个常驻**。
另外三个是**按需**的，不计入常驻：

| 线程 | 何时存在 |
| --- | --- |
| 停机协调线程 ×1 | 只在 `Stop()` 期间创建，收尾完成即退出（§5.1 的 D-10 决策） |
| metrics loop ×1 | `metrics_port != 0` 时 |
| 自注册线程 ×1 | `registry_endpoint` 非空时（§2.4.1） |

没有协程，没有 `select`，没有每个连接一个线程。

### 3.1 客户端线程模型

```
业务线程（调用方，N 个，任意多）
   │ Channel::Call(...)       ← 阻塞等待响应
   ▼
ConnectionPool（每 endpoint 若干条 ClientConnection）
   │ 借出 / 归还
   ▼
客户端 EventLoop 线程 × 1
   │ 所有 socket 的读写都在这个线程
   │ 收到响应帧 → 按 seq 找到 PendingCall → 填数据 + notify cv
   ▼
业务线程被唤醒，反序列化，返回
```

关键点：

1. **socket 永远是非阻塞的，所有 IO 都在那一个 loop 线程**。所以「同步调用」不会退化成
   「阻塞 socket」——这不是 phxrpc 那种"每条连接一个阻塞 socket"的模型。
2. **但业务线程确实会在两处阻塞**（诚实地写出来，便于算容量）：
   - `Acquire` 无空闲连接且未达上限时，**在调用方线程里同步 connect**，最长
     `connect_timeout_ms`（再被剩余 deadline 截断）；
   - 等响应、或等别人归还连接时，阻塞在 `condition_variable` 上（`cv.wait_for`，睡眠不忙等）。
   所以最坏情况下一个业务线程被占用 ≈ `connect_timeout_ms + request_timeout_ms`。
   这是所有池化 RPC 客户端共有的成本，不是本实现的缺陷。
3. **推论（配错参数会长这样）**：`connections_per_endpoint` 太小 + 业务线程太多 →
   大量线程堵在 `Acquire` 的 cv 上（用 `ConnectionPool::pending_acquire_count()` 观测，
   它长期 > 0 就说明池小了）；`connect_timeout_ms` 设得过大 → 每次建连失败都会长时间
   占住一个业务线程，把"下游不可达"放大成"上游线程池耗尽"。

### 3.2 为什么主/从 Reactor 要分开

- 如果 acceptor 与 IO 同线程，`accept()` 的系统调用开销（含新连接初始化、TLS 握手等）
  会直接打断已建立连接的收包延迟；
- 分开后 acceptor 只做一件事，可以把它 pin 到一个核上（Linux 可用 `sched_setaffinity`，
  Windows 可用 `SetThreadAffinityMask`），实测 accept 抖动显著降低；
- 代价是多一个线程与一次跨线程 fd 传递（一次 `RunInLoop` 投递）。

### 3.3 为什么不做协程（D-04 详解）

| 维度 | 协程方案（phxrpc） | 线程池方案（本项目） |
| --- | --- | --- |
| Windows 可用性 | `ucontext` 不可用，只能引入 boost.context（违反零依赖） | 完全可用 |
| 编程模型 | 同步写法、异步执行，最舒服 | 业务代码在 worker 线程里同步执行，也舒服 |
| 上下文切换成本 | ~100ns（用户态） | ~2–5μs（内核态，含调度） |
| 阻塞调用容忍度 | **不能容忍**：一个业务里调 `sleep` 会卡住整个调度器上的所有协程 | 只卡住一个 worker 线程，其余照常 |
| 调试难度 | 栈是分离的，gdb 里 call stack 断层 | 普通线程栈，调试与 profiling 工具全部适用 |
| 实现风险 | 栈溢出、协程泄漏、定时器与 socket 生命周期耦合（phxrpc 的 700 行 `IOFunc` 就是代价） | 低 |
| 吞吐量级 | 长连接 echo 峰值 ~500k qps（24 核） | 同量级（瓶颈在 syscall 与内存，不在上下文切换） |

**结论**：协程优化的是"上下文切换"，但 RPC 服务端的成本结构是
「syscall(40%) + 内存拷贝(30%) + 业务(30%)」，上下文切换占比很低。
为了那 5% 去承担"零依赖失效 + 跨平台失效 + 实现风险"是明显不划算的交易。
**这是一个可以被面试官追问并站得住的取舍**，所以我们把它写清楚。

---

## 4. 一次 RPC 的完整生命周期

### 4.1 客户端侧

```
业务线程                                  客户端 EventLoop 线程
────────────────────────────────────────────────────────────────────────────
Channel::Call(method_id, req, resp, opts)
 │
 ├─ 1. 计算 deadline = NowMs() + timeout
 ├─ 2. Serializer::SerializeToString(req) ── 失败 → return kSerializeError
 │
 ├─ 3. for attempt in 0..max_retries:
 │      │
 │      ├─ LB.Select(hash_key, attempt) ── 无可用 → return kServiceUnavailable
 │      │
 │      ├─ pool.Acquire(ep, min(acquire_timeout_ms, 剩余预算))
 │      │        │
 │      │        ├─ 有空闲连接 → 校验（closed/unhealthy/stale）后立即借出
 │      │        ├─ 未达上限   → **在调用方线程同步 connect**
 │      │        │              （ConnectWithTimeout，封顶 min(connect_timeout_ms, 剩余)）
 │      │        │               不是"扔给 IO 线程异步建连再回填"——那个方案会
 │      │        │               撞上"共享连接对象的最后一个引用必须在 loop 线程释放"
 │      │        └─ 已达上限   → 在条件变量上等归还（睡眠，不是忙等）
 │      │                       直到有人 Release / 池停 / 超时
 │      │
 │      ├─ ClientConnection::Call(...)
 │      │   ├─ seq = next_seq_++
 │      │   ├─ 构造 Frame{seq, method_id, Request|KeepAlive}
 │      │   │   metadata{trace_id, timeout_ms, idempotent, ...}
 │      │   │   payload = 已序列化字节
 │      │   ├─ Codec::Encode(frame, &out_buf)
 │      │   ├─ pending_[seq] = PendingCall
 │      │   ├─ conn->Send(out_buf)  ─────────────► Channel 可写 → ::send
 │      │   │                                        （部分写则注册写事件续发）
 │      │   └─ cv.wait_until(deadline)  ──┐
 │      │                                  │
 │      │                                  │  ◄── 可读事件
 │      │                                  │      Codec::Decode 循环拆帧
 │      │                                  │      seq 查 pending_
 │      │                                  │      填入 status/payload
 │      │                                  │      cv.notify_all()
 │      │   ◄──────────────────────────────┘
 │      │
 │      ├─ 超时 → 槽位标 abandoned（**不摘除**，doc 03 §3.4 S-1）
 │      │         唤醒调用方返回 kTimeout；abandoned 累计 > 256 → 关连接重建（S-3）
 │      │
 │      ├─ status.ok() ──► pool.Release(conn)
 │      │   └─ Serializer::ParseFromArray(resp_payload, resp)
 │      │   └─ return ok
 │      │
 │      └─ 失败 → 判断是否可重试（§4.3）：
 │            可重试 → 退避（被剩余 deadline 截断）→ continue（下一轮换 endpoint）
 │            不可重试 → return status
 │
 │   连接的归还由栈上的 RAII `Lease` 析构决定，不看"是否重试"，只看健康度：
 │     healthy 且未关闭 → pool.Release（回空闲队列）
 │     否则            → pool.Invalidate（关闭 + 下次重建）
 │   kTimeout / kCancelled 会把连接标为不健康；kOverload **不会**——
 │   服务端只是拒绝执行，连接本身是好的，丢弃它纯属浪费。
 │
 └─ 重试用尽 → return 最后一次的 status
```

**这张图里有三处不显眼但必须写死的契约**（详见 doc 03 §3.3 与 §3.4）：

1. **响应按 `seq` 匹配，不按到达顺序匹配。** 服务端由业务线程池并发执行，先出队的任务
   不一定先完成，所以**同一连接上的响应会乱序返回**——这是设计而不是 bug。
   时序图里 `Codec::Decode` 之后那步是"`seq` 查 `pending_`"，不是"取下一个等待者"。
   若把它实现成 FIFO 唤醒，压测时就会出现"偶发返回了上一次请求的结果"，且只在并发下复现。
2. **连接在存在 in-flight 请求期间不得归还到池中。** 图上 `pool.Release(conn)` 只在
   `pending_` 为空（本次调用已完成且无其它在飞）时才有意义；否则迟到的响应会被下一位
   借用者收到，形成跨调用的串包。这就是 `ConnectionPool` 用"独占租约"而不是
   "共享计数"的原因。
3. **超时不等于"这个 seq 可以回收"。** 图上超时那一步写的是"槽位标 `abandoned`"，
   而不是"从 `pending_` 摘除"。迟到的响应会落到这个空槽上，被就地丢弃（S-2）——
   如果超时就删槽，下一个用同 seq 的调用会收到别人的响应。
   `abandoned` 累计超过 256 个说明这条连接已经不正常，**主动关连接重建**（S-3）；
   连接关闭时全部在飞调用由 `HandleClosed` 一次性以 `kCancelled` 唤醒（S-4），
   不能让它们各自等满 deadline，否则对端一崩业务线程池就被拖满。

### 4.2 服务端侧

```
从 Reactor 线程                                              业务 Worker 线程
────────────────────────────────────────────────────────────────────────────────
Epoch: fd 可读
 │
 ├─ 1. Connection::HandleReadable
 │      ::recv 循环 → 追加到 input buffer（读到 EAGAIN 为止）
 │
 ├─ 2. 循环 Codec::Decode(&input, &frame, max_frame_size)
 │      ├─ kNeedMore → 退出循环（半包，等下次可读）
 │      ├─ kError    → conn->ForceClose(); metrics.protocol_error++; return
 │      └─ kOk       → 进入 3
 │
 ├─ 3. 帧级校验
 │      ├─ 请求帧 status 必须 0，否则回 kInvalidArgument
 │      ├─ flags 合法（§doc03 §4）
 │      ├─ stopping 置位 → 回 kServiceUnavailable（响应头带 kFlagDraining）
 │      └─ method_id 查 Dispatcher
 │            未命中 → 立刻回 kNotFound（**不投递线程池**，避免占队列）
 │
 ├─ 3.5 自适应限流（enable_adaptive_reject）：ShouldReject() → 回 kOverload
 │      概率拒绝（`FastRand()%100 < reject_rate`），rate 由 housekeeping 每 500ms
 │      按**窗口内排队均值**调整（超过 fast_reject_threshold_ms 就 +step，否则 -step，0..99）
 │      ⚠️ 「排队均值」必须取自 `queue_wait_us`（收帧 → worker 开始执行）**且不含业务执行时间**。
 │         一旦把业务耗时算进去，这个闭环就从"对积压反应"变成"对慢反应"：
 │         一个 20ms 的慢方法足以把拒绝率推向 99%，且不区分方法（§5.3.1 三指标模型）。
 │
 ├─ 4. TrySubmit(业务任务)  ── 失败（队列满）→ 回 kOverload
 │      │                    ├─ rejected_total++，连接**保持**（不关，见 D-07）
 │      │                    └─ **暂停这条连接的读**（SetReadEnabled(false)）：
 │      │                       让内核接收窗口收窄，压力回到对端 TCP 栈；
 │      │                       housekeeping 的 ResumePausedReads() 恢复
 │      ▼
 │   worker_pool_->Submit([conn, frame=std::move(frame), seq, start_ms] {
 │       │
 │       ├─ 5. 反序列化：Serializer::ParseFromArray(frame.payload, &req)
 │       │      失败 → 回 kSerializeError
 │       ├─ 6. **业务超时判定（全框架唯一一处）**：
 │       │      NowMs() - start_ms > request_timeout_ms
 │       │      → 回 kTimeout 且**不调用业务**（start_ms 是收到帧的时刻，
 │       │        所以排队时间已被这一处覆盖，不需要第二处判断）
 │       ├─ 7. handler(payload, len, &resp_payload, &ctx)
 │       │      └─ 生成的 handler：Decode → service->Echo(req,&resp) → Encode
 │       │         注意：ctx **不传给业务**，见 doc 04 §6.7.3
 │       ├─ 8. 构造响应 Frame{同 seq, Response, status, payload}
 │       ├─ 9. conn->Send(bytes)   ← 线程安全，内部 RunInLoop 到从 Reactor
 │       └─ 10. metrics: latency / error_code / qps
 │   });
 │
 │   （从 Reactor 线程继续处理下一个可读事件，完全不等待业务）
 │
 └─ Epoch: fd 可写（业务回包触发的写事件）
        ::send 尽可能多的字节；未发完则保留写关注
        发完 → DisableWriting + write_complete_cb（连接池可复用）
```

### 4.3 错误分类与重试判定矩阵（按"字节有没有发出去"分段）

重试的安全边界不是状态码，而是**请求字节是否已经离开本进程**。同样是 `kTimeout`：
"建连超时"重试一百次也不产生副作用；"等服务端响应超时"重试一次就可能把一次扣款做两遍。
所以矩阵必须按两段写，一列到底的 ✅/❌ 表必然有一半是错的。

| 状态码 | before-send（未发出） | after-send（可能已执行） | 说明 |
| --- | :-: | :-: | --- |
| `kOverload` | — | ✅ 无条件 | 服务端明确拒绝（队列满 / 自适应限流），业务没执行；**退避时间加倍** |
| `kOverload`（**本地产生**） | ❌ **不重试** | — | `AcquireFailure::kPoolFull`：本地连接池饱和。重试只会加剧争抢，见 §4.3.2 |
| `kServiceUnavailable` | ✅ 无条件 | ✅ 无条件 | 拿到响应帧 = 对端声明"我这不收"，同样没执行 |
| `kTimeout` | ✅ 无条件 | ⚠️ 仅 `idempotent` | 建连超时一个字节都没发；等待响应超时则可能已执行 |
| `kCancelled` | ✅ 无条件 | ⚠️ 仅 `idempotent` | 借连接期间被停机 vs 中途断连 |
| `kInvalidArgument` / `kNotFound` / `kSerializeError` / `kInternal` | ❌ | ❌ | 换谁都一样，重试纯属浪费 |
| `kOk` | — | — | 成功，不重试 |

四条门禁**全部**满足才重试：

1. 上表对应格是 ✅，或 ⚠️ 且 `CallOptions::idempotent == true`；
2. **业务重试轮次** `attempt < max_retries`——注意"换实例再试"还有一层**故障转移**，
   它不占这个预算（§4.3.3）；
3. **剩余 deadline 足够**：退避睡眠被 `deadline` 截断，截断后若已到点就直接退出；
4. **候选集非空且不是全部试过**：`Select` 返回 false → 立刻 `kServiceUnavailable`；
   单轮内已失败过的 endpoint 会被跳过，单轮扫描上限 `min(3, 实例数)`。

#### 4.3.1 共享 deadline：为什么"用户配 1000 就应该能用满 1000"

整次调用只有**一个** deadline（`opts.timeout_ms`，缺省 `request_timeout_ms`），
每次尝试的等待上限 = **剩余时间**（`deadline - now`），退避睡眠同样被它截断。

推论（也是这一节存在的理由）：

- **本地等待超时会把预算耗尽**，于是下一轮循环在顶部的 `now >= deadline` 处退出——
  **不会**出现"超时了再全量重试一轮"。配 1000 就是最多花 1000，P99 上限不会被切成 400。
  这是刻意的取舍：*把预算按次数均分*看似"每次都有机会"，实际是"每次都不够用"，
  用户只会看到一堆莫名其妙的超时。
- **唯一例外是服务端显式回 `kTimeout`**（它有响应帧，预算通常还有剩）。
  此时幂等调用可以换实例再试一次——这正是我们想要的行为。
- 因此"`kTimeout` 要不要重试"**没有单一答案**，答案取决于"超时发生在哪一段 + 是否幂等"。
  把它简化成一句"超时不重试"，会让实现把 after-send 的幂等重试也一起砍掉。

#### 4.3.2 「借不到连接」的四种含义，必须分开处理（`AcquireFailure`）

**这是本框架里唯一一处"同一个失败、两种相反处置"的地方**，所以专门给了枚举
（`connection_pool.h` 的 `AcquireFailure`），而不是让上层去解析错误文本：

| `AcquireFailure` | 含义 | 处置 | 最终状态码 |
| --- | --- | --- | --- |
| `kNone` | 借到了 | 正常发请求 | — |
| `kPoolFull` | 本进程连接数已达 `connections_per_endpoint` 上限且全部被占，等到 `acquire_timeout_ms` 无人归还 | **不重试，立即返回** | `kOverload` |
| `kConnectFailed` | connect 明确失败（拒绝 / 不可达 / 地址非法） | 按 before-send 重试（换实例） | `kServiceUnavailable` |
| `kTimeout` | 建连把预算吃光了 | 预算已尽，事实上不会再试 | `kTimeout` |
| `kStopped` | 池已停 / 无 loop / 建连期间被 `Stop()` | **不重试** | `kCancelled` |

**为什么 `kPoolFull` 绝对不能走"重试"这条路**：默认 `connections_per_endpoint = 1` 时，
8 个业务线程里 7 个在 cv 上等；`acquire_timeout_ms` 一到点，池返回"饱和"——
如果上层把它当 `kServiceUnavailable` 去重试，**重试把争抢放大 3 倍 → 池更满 → 超时更多**，
标准正反馈拥塞。而且重试根本不可能成功：连接数上限是本进程自己的配置，
不会因为"再试一次"变大。

**为什么 `kPoolFull` 映射 `kOverload` 而不是 `kServiceUnavailable`**：
"本地资源不够"与"下游不可用"对调用方是两件事。前者应该**退避**（等自己的连接释放），
后者应该**换实例**（那个实例坏了）。混成一个码，调用方只能二选一，两种都会错。

> 因此 `kOverload` 有**两个来源**：服务端拒绝（after-send，自动重试 + 退避加倍）
> 与本地池饱和（before-send，**不重试**）。见 doc 03 §5 的补注。

#### 4.3.3 故障转移与业务重试是**两层**，不能合成一个计数器

| | **故障转移**（failover） | **业务重试**（retry） |
| --- | --- | --- |
| 触发条件 | 请求**一个字节都没发出去**：建连失败、endpoint 在冷却中 | 已发出但失败 / 服务端明确拒绝 |
| 消耗 `max_retries` | **否** | 是 |
| 退避 | **不睡**，立即换下一个实例 | `n × retry_backoff_ms`；`kOverload` 加倍 |
| 单次调用上限 | `max_candidates = min(3, 实例数)` | `max_retries` |
| `max_retries = 0` 时 | **仍然会做** | 不做 |
| 指标 | `qzrpc_client_failover_total` | `qzrpc_client_retry_total` |

**为什么必须分开**：把两者合成一个计数器，"两个实例挂一个"会被算成业务重试——
用户配 `max_retries = 0` 就只能拿到一半成功率，而监控上完全看不出原因。
更根本的是：**没发出去的请求不存在"重复执行"风险**，它压根不属于"重试"这个概念的语义范围。
把它强行并进去，是用一个错误的抽象换一个好看的计数。

**尝试次数上界仍然可计算**（这才是"可预测"的真正含义）：

```
最坏尝试次数 ≤ max_candidates × (max_retries + 1)      默认 3 × 3 = 9
最坏耗时     ≤ max_candidates × connect_timeout_ms     默认 3 × 200 = 600ms
              + max_retries × (backoff + 一次 request_timeout)
```

两条上界都是**常量乘积**，不是"看运气"。所以"给 failover 单独留预算会让最坏情况不可预测"
这个担心，在**两层各自封顶**之后并不成立——关键是每一层都要有明确的 `min(3, N)`。

**代价与运维建议**：

- **代价**：`max_retries = 0` 时**没有业务重试能力**（但仍有故障转移）。
  这是有意的：要么接受"不重试"，要么接受"可能重复执行"，两者不可兼得。
- **运维建议**：业务重试覆盖的实例数是 `max_retries + 1`，**实例数 > `max_retries + 1` 时**
  应相应提高 `max_retries`，否则尾部实例在"重复轮次"里永远轮不到
  （`max_candidates` 的 3 只是单轮扫描上限，不等于覆盖全部实例）。

`max_retries = 2`（默认值）之所以安全，是因为它放大的是"没发出去 / 被明确拒绝"的调用，
而不是"业务已经跑过一遍"的调用。**不要**为了"更保守"改成 0——那只会把建连抖动
直接冒到业务层，并不会减少任何一次真实的重复执行。

#### 4.3.4 endpoint 冷却：让死实例不进入候选，而不是靠重试绕过它

冷却在 `ConnectionPool` 里（`Entry::cooldown_until_ms`），语义是**被动健康检查**：

| 事件 | 行为 |
| --- | --- |
| 建连失败 | `connect_failures++`，冷却时长 3s → 6s → 12s → 24s → **30s 封顶** |
| 冷却期内 `Acquire` | **立即返回** `kConnectFailed`，**一个 TCP 连接都不发起**——省掉的正是 `connect_timeout` 那一整段 |
| 建连成功 | `cooldown_until_ms = 0`、退避清零 |
| 预算不足 50ms 的临期调用 | **不进冷却**（`kMinFairConnectBudgetMs`）：否则一次"钱快花完"的调用会把好实例拉黑 3 秒，**故障转移反而制造故障** |

最后那条是这段实现里最容易被"优化掉"的正确性细节：冷却的触发条件必须排除
"这次失败是因为我没给它足够时间去连"，否则越接近 deadline 越容易误杀健康实例。

> ⚠️ **冷却参数目前是硬编码的**：`kCooldownBaseMs = 3000` / `kCooldownMaxMs = 30000`（`connection_pool.cpp`）。
> `ClientOptions` 里的 `breaker_cooldown_ms`（默认 **5000**）与 `breaker_failure_threshold`
> **都没有被读取**——也就是说用户把 `breaker_cooldown_ms` 调成 500，
> 实际仍是 3s。这是**配置项与实际行为不一致**，必须在交付前收敛成一种：
> 要么让冷却读这两个字段（`breaker_failure_threshold` 还能表达"连续失败 N 次才进冷却"，
> 比现在"首次失败即冷却"更温和），要么把这两个字段从 `ClientOptions` 删掉。
> 保留一个"配了没反应、默认值还和实际值不同"的旋钮，是最坏的一种。


---

### 4.4 背压的三层（从"礼貌"到"强硬"）

过载时框架不说"我很忙"就完了，它按代价从小到大依次施压：

| 层 | 手段 | 触发条件 | 代价 / 为什么是这个顺序 |
| :-: | --- | --- | --- |
| 1 | 响应头带 `kFlagDraining` | 停机中 | 最轻：只提示"换节点"，不改变任何数据流 |
| 2 | 回 `OVERLOAD` 帧（**不关连接**） | 自适应限流命中 / 业务队列满 | 客户端能区分"服务端主动拒绝"与"网络故障"，于是**退避加倍重试**而不是换实例乱撞。见 D-07 |
| 3 | **暂停该连接的读**（`SetReadEnabled(false)`） | 只在**队列满**时 | 让内核接收窗口自然收窄，压力由 TCP 流控回传给上游——不靠应用层协议，也不需要读进内存再丢。housekeeping 每 500ms 的 `ResumePausedReads()` 恢复 |

**为什么不用"直接关连接"**（phxrpc 的做法）：在客户端看来，`close` 等价于网络故障 →
换实例 + 重试 → 把单点过载扩散成集群雪崩。而 `OVERLOAD` 帧是**可归因的拒绝**，
客户端能做出正确反应。

**为什么不用"无界队列"**：队列是内存。过载时把队列当缓冲，等于用 OOM 换延迟，
而且排队越久超时越多，最后所有请求都白发一遍。

**为什么第 3 层只在队列满时用**：限流（第 2 层）是概率性的、可自愈的；
暂停读会直接影响那条连接上的所有请求（包括本来能成功的）。
把它当成"队列满"这种**已经确定处理不过来**的场景的专用手段，才不会误伤。


---

### 4.5 服务发现的一次刷新（注册中心是控制面，不是数据面）

```
RegistryClient worker 线程（每 registry_poll_interval_ms，默认 3000ms）
   │  也可由 Channel::RefreshEndpoints() 手动立即触发一次
   ▼
① 建一条**一次性短连接**（SocketUtil::ConnectWithTimeout）
   │  为什么不复用业务连接池：注册中心是控制面。抢业务连接、或被业务连接的
   │  故障连坐（池满 / 冷却），都会让"发现"在最需要它的时候失效
   ▼
② FetchRequest{service_name} → FetchResponse{version, instances[]}
   │  响应里带 version
   ▼
③ 版本短路：version 非 0 且与当前相同、且实例数也相同 → 直接返回
   │  省掉一次 160*N 的环重建（CH 策略下这是最贵的动作）
   ▼
④ 应用：替换本地列表 → 回调 Channel → LoadBalancer::UpdateEndpoints
       （静态 endpoints + 发现结果**并集去重**；静态在前，作为注册中心不可用时的兜底）
   ▼
⑤ 失败：**保留旧列表** + degraded = true + failure_total++
       "最后已知可用集合"永远优于"空集合"——空集合会让所有调用立刻变成
       kServiceUnavailable，把一次控制面抖动放大成一次全局故障
```

停机：`stopping` 原子 + **50ms 粒度睡眠**，最坏 50ms 退出。
没有用 `cv.wait_for(interval)`——这里只有"停"一个事件，加条件变量要多一把锁和一次广播，
收益为零。这与服务端 housekeeping 用 `cv.wait_for` 的选择相反，因为那边**需要**被停机立刻叫醒
且同时有多个等待点。

**为什么用短连接而不是长连接**：拉取是秒级的低频操作，长连接要额外处理重连、心跳、半开检测；
短连接把故障模型压成"一次失败的 RPC"，代码量少一个数量级。


---

## 5. 关键设计决策

| # | 决策 | 备选方案 | 选择理由 |
| --- | --- | --- | --- |
| D-01 | **连接对象归属**：`Connection` 由持有者以 `shared_ptr` 持有；`Channel`/fd 由 `Connection` 独占；回调里禁止 reset 自己的那个 shared_ptr | 裸指针 + 手工 delete（phxrpc） | phxrpc 用 `void* args` 在 IO 与 worker 间传 socket，超时/正常两条路径都要正确 free，是它最难维护的部分 |
| D-02 | **粘包拆包**：长度前缀 + 「读缓冲即状态」的隐式状态机，半包时**不消费任何字节** | 显式 per-connection 解码状态机 | 显式状态机要处理"状态与缓冲不一致"，是 bug 温床；重解 20 字节头是纳秒级开销 |
| D-03 | **seq 路由**：32 位 seq，`unordered_map<seq, PendingCall>`，一条连接可同时有多个在飞请求 | 每连接串行（一问一答）、或 stream id + 流控 | 串行会浪费 RTT 利用率；完整多路复用需要流控/优先级，超出范围。seq 是两者的最佳中间点 |
| D-04 | **不做协程**，用「主从 Reactor + 有界队列线程池」 | ucontext / boost.context | 见 §3.3 |
| D-05 | **零拷贝边界**：只在三处允许大块拷贝 —— ① socket→input buffer；② 反序列化到业务对象；③ output buffer→socket。**帧编解码本身零拷贝**（payload 用指针+长度传递） | 全链路零拷贝（iov / 环形缓冲 / 引用计数分片） | 全零拷贝需要引用计数的 buffer 分片，代码量翻倍而收益在 v1 规模下不可测 |
| D-06 | **业务线程不允许碰 Connection 的 buffer** | 允许 worker 直接读 input buffer | input buffer 只在从 Reactor 线程被访问，这个约束让"无锁"成为可能；代价是 payload 要拷一份到 worker（即 D-05 的第 ② 处） |
| D-07 | **过载拒绝回帧，不关连接** | phxrpc：`break` 关连接 | 客户端能拿到 `OVERLOAD` 并做退避；关连接会让客户端误判为网络故障并放大重试 |
| D-08 | **方法的 handler 是无类型的 `std::function<Status(const char*, size_t, std::string*, ServerContext*)>`** | 让框架认识具体消息类型 | 框架不认识任何 IDL 生成类 → 换 IDL/换序列化只需换 codegen，框架一行不动 |
| D-09 | **错误码是 `StatusCode` 枚举，数值即线协议 status 字段** | 每层自定义错误码 + 映射表 | 少一层映射就少一处不一致；`Status::ToString` 一次成型 |
| D-10 | **优雅退出四步**（§5.1） | `exit()` / `kill -9` | 要让在飞请求有机会完成，且要让客户端明确知道"是停机不是故障" |
| D-11 | **配置是 POD 结构体 + `Config` 读取器** | 配置类层级 + `DoRead` 虚函数（phxrpc） | 加字段只改一处；构造后 `const`，线程安全不需要"约定" |
| D-12 | **公共头零平台依赖，`net/platform.h` 是唯一例外且仅能被 .cpp 包含** | 头文件里 `#ifdef _WIN32` 到处开花 | 上层代码（含 example）能做到"完全不感知平台"；也便于将来加第三个平台 |
| D-13 | **Server/Channel/ConnectionPool/RegistryServer 用 pimpl** | 全部暴露在头文件 | 内部组件互相引用多（10+），暴露出来会让公共头不可读；RPC 场景下一次指针跳转可忽略 |
| D-14 | **codegen 只生成 C++，不生成 CMake/Makefile** | 像 phxrpc 一样连 Makefile 一起生成 | 构建描述属于项目，不属于代码生成器；生成 Makefile 会让"换个构建系统"变成改生成器 |
| D-15 | **注册中心用本框架自己实现** | 引入 etcd / consul | 自举是端到端可用性的最强证据，也让 Demo 零外部依赖 |

### 5.1 优雅退出四步（D-10 展开）

```
调用 Server::Stop()（任意线程）
   │
   ├─ 步骤 1：停止接入
   │    主 Reactor：Acceptor::Stop()（把监听 fd 从 Poller 摘除，不 close）
   │    → 内核仍在 backlog 里完成握手，客户端不会立刻收到 RST
   │
   ├─ 步骤 2：声明停机
   │    设置 draining 标志 → 之后收到的**新请求帧**直接回 kServiceUnavailable
   │    （不再投递线程池，避免把已经排队的活再插队）
   │    对每个连接设置 flags |= kFlagDraining 的响应，让客户端主动换节点
   │
   ├─ 步骤 3：排空在飞请求（在独立的 coordinator 线程里）
   │    轮询（每 **5ms**）`global_pending` —— 全局在飞计数，不是遍历每个连接的
   │    `pending_ops()`（遍历要锁 N 个连接表，停机路径上没必要）
   │    直到为 0 或超过 graceful_shutdown_timeout_ms（默认 5s）
   │    超时未排空 → 打 WARN（带剩余在飞数）后继续收尾，**不无限等**
   │
   └─ 步骤 4：收尾（顺序不能换）
        ① 关闭全部连接（ForceClose；先关连接再 Quit，避免半开连接占着 fd 进
           "loop 已停但 fd 还活着"的状态）
        ② 停 housekeeping（notify + join）
        ③ **关掉监听 fd**：`Acceptor::Stop()` 只是把 fd 从 Poller 摘除（这是步骤 1 的
           优雅降级，故意不 close）；收尾必须在主 loop 线程里析构 Acceptor
           （走 RunInLoop，靠 Loop() 退出前的队列排空保证执行）——
           否则 fd 泄漏，进程重启时 bind 会因端口被自己占着而失败
        ④ 所有 loop（主 / 每个从 Reactor / metrics loop）`Quit()` + join
        ⑤ `ThreadPool::Stop(drain = true)` ← **drain 是 true**：已入队的任务跑完再退
           （它们会往已关闭的连接上写，`Connection::Send` 对 kClosed 是安全空操作），
           比"直接丢弃"更容易保证"客户端不会收到半截响应"
        Run() 返回，Wait() 返回 true
```

**为什么步骤 2 要"拒绝新请求"而不是"继续接"**：
如果继续接，队列永远有新任务进来，步骤 3 的"排空"就永远不会收敛。
必须先把入口关上，才能把水位降下来。这是所有优雅停机的共性结构。

### 5.2 已知限制（主动限制清单）

下面这些不是"还没做"，而是**评估后决定不做**。列出来有两个用处：
一是让使用者不抱错误期待；二是每条限制背后都有一个明确的所有权/复杂度取舍，
面试时被追问"为什么"能直接照着答。

| 限制 | 为什么主动限制 | 换取到的东西 |
| --- | --- | --- |
| **不支持业务侧协作式取消（v1）**：服务基类签名固定 `Status Echo(const Req&, Resp*)`，业务拿不到 deadline、无法中途放弃 | 一旦把 `ServerContext`（请求生命周期）暴露给业务，业务把它存到回调之外（异步日志 / 线程池再投递 / 缓存）就是悬垂引用，编译期拦不住 | 一个**不会出错的所有权模型**：请求上下文只在框架内部流转，永不逃逸到业务 |
| **不在 EventLoop 线程做 DNS 解析** | `getaddrinfo` 是阻塞调用，一次超时能卡住整个 loop 上的所有连接 | IO 线程的延迟可预测性 |
| **不做传输层加密（TLS）** | 零第三方依赖下自研 TLS 不现实；引入 OpenSSL 违反 K-01 | 编译零依赖、部署无额外动态库 |
| **不实现压缩算法**（帧头保留 `kFlagCompressed` 与 `metadata.compression`） | 自写 deflate 的收益远小于维护成本；收到压缩位置位即报协议错 | 不需要为"正确性"担心一个自己实现的压缩器 |
| **不支持流式 RPC / 双向流** | 需要连接级多路复用 + 流控 + 半关闭语义，工作量与"核心框架 + 可跑 Demo"的范围不匹配 | 范围可控，能在有限时间内做深而不是做宽 |
| **`acquire_timeout_ms` 与 `request_timeout_ms` 的比例要靠人配**：两者没有交叉校验（§2.3 结论 3），配成 1:1 就等于"每次借连接都可能耗掉整个请求预算" | 交叉校验拦不住"可疑但合法"的组合（§2.3 结论 3）；真正要做的是给一个能执行的比例 + 一个能算的容量账（§2.3.1） | 推荐 **1:5**（默认值正是 200ms : 1000ms）：借连接失败要尽快暴露，把时间留给真正的请求。两者相等时，`kPoolFull` 会在业务几乎没跑的情况下把 P99 直接顶到上限 |
| **不做业务代码的强制中断** | C++ 无法安全终止一个正在跑的线程；`request_timeout_ms` 只决定"是否还调用业务/是否回超时"，不试图杀死它 | 不引入 `pthread_cancel` 这类不可预测机制 |
| **不做主动健康检查（探活）** | 注册中心只做心跳 TTL 过期，不做主动 TCP 探测 | 注册中心实现量减半，且不引入探活连接池 |
| **不支持多进程模型** | 共享监听、reuseport、跨进程指标聚合的成本远大于收益（phxrpc 同样不支持） | 单进程内的资源调度可以做到极致简单 |

#### 5.2.1 已知缺陷与取舍（实现层面，明确接受）

上面 §5.2 是"决定不做"，这里列的是**做了但有已知缺口**的三条。放在这里的理由：
**主动列出已知取舍，比被评审者问出来强一个数量级**——而且每一条都写清了"为什么现在可接受"，
将来真要修也有明确的判据（什么条件下它不再可接受）。

| 已知缺陷 | 具体表现 | 为什么现在可接受 | 什么条件下必须修 |
| --- | --- | --- | --- |
| **指标快照不是同一时刻的原子切片** | `DumpText()` 逐个读取各 Counter/Histogram 的原子值；一个 histogram 的 `_bucket{le="+Inf"}`、`_count`、`_sum` 是三/多次独立读取，抓取端拿到的是一份"拼接快照"：`_sum / _count` 求出的均值有微小噪声，极端情况下 `+Inf` 与 `_count` 对不上 | 每个值本身都是**原子的、不会读撕裂**（`std::atomic`），只是彼此不同步。Prometheus 的抓取模型本来就容忍这一点（它自己也是采样），且这些指标的用途是趋势与分位，不是精确对账 | 需要"某瞬间的精确总量"做计费/审计时。那时应当另开一个导出接口，在注册表锁内一次性拷贝全部值再格式化，而不是继续复用 `DumpText()` |
| **对端半关闭（FIN）时，写缓冲里未发出的响应会被丢弃** | 读侧收到 FIN 后，处理完已读数据就整体关闭连接，`CloseInLoop` 里会 `RetrieveAll()` 清掉输出缓冲。一个"发完请求就 `shutdown(SHUT_WR)`"的客户端会**收不到响应**（或收到半截） | v1 明确不做半关闭长连接：本框架的客户端与生成的 Stub 都不会半关闭（连接由连接池管理，谁都不会单方面关写方向），所以这条路径只对外部/手写客户端生效。为它引入"半关闭状态机"会让 `Connection` 的状态从 4 个变成 6 个，收益与复杂度不成比例 | 要对外提供"HTTP 式一问一答 + 半关闭"的接入方式时。修法是 `HandleReadable` 在 `peer_closed` 分支只关**读方向**（`shutdown(SHUT_RD)`）并把连接标成 `kDisconnecting`，让写缓冲自然排空后再关 |
| **停机窗口内可能泄漏 fd** | 步骤 4 之后仍有极少量的 fd 正通过 `RunInLoop` 投递给某个从 Reactor；如果那个 loop 已经 `Quit` 且队列已排空，携带 fd 的闭包永不执行，fd 无人关闭（进程随后退出，由操作系统回收） | 窗口是"两次投递之间"的极短区间，且**进程即将退出**——fd 由 OS 回收，不会影响下一次启动（不涉及端口占用：监听 fd 走的是另一条确定路径，见步骤 4 ③） | 进程需要在不停机的情况下**反复**启停 `Server` 实例（如测试里循环跑 1 万次）时。修法是给"待投递 fd"加一个原子计数，`JoinThreads` 前等它归零，或直接改成在 acceptor 线程里 `close` 未送达的 fd |

> 另外两处已知但不同性质的问题，不在本表：
> ① `Connection::Send` 的注释写着"只拷贝这一次"，实际是 2~3 次拷贝（性能问题，非正确性问题）；
> ② `/metrics` 端点没有连接数上限与空闲回收——那是缺陷不是取舍，按 bug 处理（见交付清单）。

---

### 5.3 指标与日志的命名约定

这些看着琐碎，但它们直接决定"看板能不能取到值"，属于契约级别，不是风格偏好。

| 约定 | 内容 | 违反的后果 |
| --- | --- | --- |
| **注册名 vs 导出名** | 代码里注册用**点分**名（`qzrpc.server.request_total`）便于阅读；`DumpText()` 导出时把 `.` / `-` 统一换成 `_`（`qzrpc_server_request_total`） | Prometheus 的 metric name 只允许 `[a-zA-Z0-9_:]`。**前端/抓取端必须按下划线名配置**，按点分配会全部取不到值——而且不会报错，只是空图 |
| **label 形式** | 支持 `"name{code=\"OK\"}"` 这种自带 label 的注册名；清洗只作用于 `{` 之前的部分 | 若整串都清洗，label 里的引号/等号会被破坏，抓取端直接解析失败 |
| **直方图单位** | `Histogram::Observe` 的单位固定是**微秒**；注册名建议带 `_us` 后缀 | 名字不带单位，看板读者无法判断是秒还是毫秒，P99 差 1000 倍都没人发现 |
| **分位数导出** | 除 `_bucket/_count/_sum` 外额外导出一组便捷分位数值：`_p50/_p90/_p99/_p999/_max_us/_mean_us`（在 `DumpText()` 里以 `# TYPE ... gauge` 行输出；`metrics.h` 本身只有 `Counter`/`Histogram` 两种类，不为此新增第三种） | 让告警规则不必从 `le` 边界反推分位数（反推要假设置信区间，极易算错）。注意这些值与同名 histogram **不属于同一指标族** |
| **分位数封顶** | `Percentile()` 取 `min(桶上界, max_us())` | 不封顶会出现 `P99 > max`：单样本 5us 落在 `[4,8)` 桶被报成 7us；10s 长尾落末尾桶被报成 4.29s。运维看到只会怀疑指标坏了 |
| **日志级别语义** | 级别 N 生效时输出**所有 `>= N`** 的日志；`kOff` 关闭全部 | 若写成 `<= N`，`kOff` 反而输出一切、`kInfo` 连 TRACE 一起出，级别旋钮退化成全开/全关 |
| **日志宏短路** | `QZ_LOG_*` 在构造 `LogMessage` **之前**判 `ShouldLog`（用 `LogMessageVoidify` 实现） | 否则即使关掉级别，`<<` 的参数拼接与 `ostringstream` 构造照付，"每帧一条 debug"的热路径上开销实打实 |

#### 5.3.1 全框架已注册的指标清单（23 个指标族 + 1 个手写 gauge）

这张表是**核对用**的：改指标名/加指标必须同步这里，否则看板会静默取不到值。
`DumpText()` 导出时点分注册名会被清洗成下划线名，下表给的是**导出名**。

> 计数口径（避免"总数"这个词含糊）：**服务端 11**（9 个 Counter + 2 个 Histogram，含 3 个
> `qzrpc_server_registry_*`）+ **客户端 8**（4 个 Counter + 2 个 Histogram + `error_total{code}`
> 与 `endpoint_cooldown_total{endpoint}` 两个 label 族）+ **注册中心 4** = **23 个指标族**；
> 另有 1 个不在注册表里的手写 gauge。带 `{label}` 的按一个族算，不按 state 数算。

| 导出名 | 类型 | 单位 | 注册位置 | 说明 |
| --- | --- | --- | --- | --- |
| `qzrpc_server_connection_total` | Counter | 个 | `server.cpp` | 累计 accept 数 |
| `qzrpc_server_connection_active` | **gauge** | 个 | `server.cpp`（`/metrics` 手写行） | 当前活跃连接数。**不在注册表里**——Counter 只能 `Inc()`，给不出会下降的值 |
| `qzrpc_server_request_total` | Counter | 次 | `server.cpp` | 累计请求数 |
| `qzrpc_server_response_total` | Counter | 次 | `server.cpp` | 累计响应数 |
| `qzrpc_server_rejected_total` | Counter | 次 | `server.cpp` | 过载拒绝数（回 `OVERLOAD` 的那部分） |
| `qzrpc_server_error_total` | Counter | 次 | `server.cpp` | 非 `OK` 响应数 |
| `qzrpc_server_request_latency_us` | Histogram | **微秒** | `server.cpp` | 收帧 → 响应写入发送缓冲（见下方三指标模型） |
| `qzrpc_server_queue_wait_us` | Histogram | **微秒** | `server.cpp` | 收帧 → worker 开始执行，**纯排队** |
| `qzrpc_server_worker_busy_us_total` | Counter | **微秒** | `server.cpp` | **累计**业务执行耗时（handler 进入 → 返回）。名字必须带 `_us_total`：`_busy` 在 Prometheus 语义里读起来像 Gauge，看板会按瞬时值画成一条无意义的锯齿线 |
| `qzrpc_server_registry_register_total` | Counter | 次 | `server.cpp` | 自注册成功次数（§2.4.1） |
| `qzrpc_server_registry_heartbeat_total` | Counter | 次 | `server.cpp` | 心跳成功次数 |
| `qzrpc_server_registry_errors_total` | Counter | 次 | `server.cpp` | 注册/心跳失败次数。**持续增长 = 注册中心有问题**，此时本机仍在服务，只是发现性在退化 |
| `qzrpc_client_request_total` | Counter | 次 | `channel.cpp` | 累计调用数（含重试） |
| `qzrpc_client_response_total` | Counter | 次 | `channel.cpp` | 累计成功数 |
| `qzrpc_client_error_total{code}` | Counter | 次 | `channel.cpp` | 按 `StatusCodeName` 分桶 |
| `qzrpc_client_retry_total` | Counter | 次 | `channel.cpp` | 累计**业务重试**次数（不含故障转移） |
| `qzrpc_client_failover_total` | Counter | 次 | `channel.cpp` | 累计**故障转移**次数（§4.3.3：换实例但不占 `max_retries`） |
| `qzrpc_client_latency_us` | Histogram | **微秒** | `channel.cpp` | 端到端（含重试与退避） |
| `qzrpc_client_pool_acquire_us` | Histogram | **微秒** | `channel.cpp` | 借连接耗时（含同步 connect；**长期偏高 = 池太小或下游慢**，配 §2.3.1 的容量账一起看） |
| `qzrpc_client_endpoint_cooldown_total{endpoint}` | Counter | 次 | `connection_pool.cpp` | 某 endpoint 进入冷却的次数。**持续增长 = 该实例真的坏了** |
| `qzrpc_registry_register_total` | Counter | 次 | `registry_server.cpp` | 注册请求数 |
| `qzrpc_registry_heartbeat_total` | Counter | 次 | `registry_server.cpp` | 心跳数 |
| `qzrpc_registry_fetch_total` | Counter | 次 | `registry_server.cpp` | 实例列表拉取数 |
| `qzrpc_registry_expired_total` | Counter | 次 | `registry_server.cpp` | TTL 到期被摘除的实例数 |

##### 服务端延迟的三指标模型（互不重叠、近似可加）

这三个指标是**拆分**关系，不是"三个角度看同一件事"。看板/告警必须按这个口径配，
否则会把"业务慢"误判成"排队积压"（这正是审计里 B1 那个 bug 的症状）。

| 指标 | 起止 | 含义 |
| --- | --- | --- |
| `qzrpc_server_queue_wait_us` | 收帧 → worker 开始执行 | 纯排队时间。**它才是自适应拒绝的输入**（§4.4 第 2 层） |
| `qzrpc_server_worker_busy_us_total` | handler 进入 → handler 返回 | 纯业务耗时（累计量，按 `rate()` 看） |
| `qzrpc_server_request_latency_us` | 收帧 → 响应写入发送缓冲 | 端到端 ≈ 排队 + 业务 + 编码 + 入队 |

两条必须写死的口径：

1. **`request_latency_us` 不含"内核真正把字节发出去"的时间**。`Connection::Send` 只是把数据
   追加进用户态发送缓冲（能立即 `send` 掉的就地发，发不完的等可写事件），**异步写出的那一段
   不在测量范围内**。所以它**不是**"客户端观测到的延迟"，两者对账时差值就是内核缓冲 + 网络。
   不写这句，将来一定有人拿它当客户端延迟来对账。
2. **`queue_wait_us` 不含业务执行时间**。反过来说：如果哪天的实现让 `queue_wait_us ≈ 业务耗时`，
   那说明指标退化成了"排队 + 执行"，自适应拒绝会开始对"慢"而不是对"积压"反应——
   一个 20ms 的慢方法就足以把拒绝率推向 99%，且不区分方法。

**两个已定契约、尚未接线的指标**（`channel.h` 注释里已列，实现待补，见 doc 03 §3.4）。
它们的**定性完全不同**，处置也不同：

| 导出名 | 类型 | 触发点 | 定性 |
| --- | --- | --- | --- |
| `qzrpc_client_late_response_total` | Counter | 迟到响应命中 `abandoned` 槽被丢弃时 `Inc()`（S-2） | **正常路径会发生**（每次"慢于 deadline"的服务端都会产生）。值得接线，也值得看 `rate()` |
| `qzrpc_client_abandoned_overflow_total` | Counter | `abandoned` 槽 > 256 主动关连接重建时 `Inc()`（S-3） | **防御性上限，正常路径不可达**（见 doc 03 §3.4 的推导）。接不接线都可以，但**绝不能进告警规则** |

> 处置：`late_response_total` 交付前必须**接线或删掉**（两行 `GetCounter` + 一处 `Inc()`）。
> 留一个"文档写了、指标查不到"的空名字是最坏选项——看板会空白，而排查的人会以为是自己配错了。
>
> `abandoned_overflow_total` 的教训相反：它是一个**不会触发的计数器**。把它放进告警规则，
> 等于给自己造一个永远绿着的假告警——比没有这条告警更危险，因为它会让人以为"这一层有监控"。
> 一个正常路径不可达的保护机制，正确的表述是"防御性代码 + 注释"，不是一个指标。

---

## 6. 目录结构

```
D:/phxrpc/
├── CMakeLists.txt                  # 顶层构建（韩渡川）
├── README.md                       # 项目门面：一句话定位 + 架构图 + 快速开始
├── .gitignore
├── docs/
│   ├── 00-file-map.md              # 任务拆分表
│   ├── 01-phxrpc-analysis.md       # phxrpc 解剖与借鉴矩阵
│   ├── 02-architecture.md          # 本文
│   ├── 03-wire-protocol.md         # 线协议规范
│   └── 04-idl-spec.md              # IDL 与生成物契约
├── idl/
│   ├── echo.qz                     # 示例服务 IDL（黄金测试输入）
│   └── registry.qz                 # 注册中心 IDL（与手写 proto 语义一致，用于自举校验）
├── src/
│   ├── common/
│   │   ├── noncopyable.h
│   │   ├── logger.h                logger.cpp
│   │   ├── time_util.h             time_util.cpp
│   │   ├── endian.h
│   │   ├── buffer.h                buffer.cpp
│   │   ├── status.h                status.cpp
│   │   ├── config.h                config.cpp
│   │   ├── string_util.h           string_util.cpp
│   │   └── metrics.h               metrics.cpp
│   ├── net/
│   │   ├── platform.h              platform.cpp
│   │   ├── poller.h                poller.cpp
│   │   ├── event_loop.h            event_loop.cpp
│   │   ├── channel.h               channel.cpp
│   │   ├── acceptor.h              acceptor.cpp
│   │   ├── connector.h             connector.cpp
│   │   ├── connection.h            connection.cpp
│   │   ├── timer_queue.h           timer_queue.cpp
│   │   ├── thread_pool.h           thread_pool.cpp
│   │   └── socket_util.h           socket_util.cpp
│   ├── serialize/
│   │   ├── field_meta.h            field_meta.cpp
│   │   ├── writer.h                writer.cpp
│   │   ├── reader.h                reader.cpp
│   │   ├── message.h               message.cpp
│   │   └── serializer.h            serializer.cpp
│   ├── protocol/
│   │   ├── protocol_const.h
│   │   ├── frame.h                 frame.cpp
│   │   └── codec.h                 codec.cpp
│   ├── server/
│   │   ├── server_options.h        server_options.cpp
│   │   ├── service_registry.h      service_registry.cpp
│   │   ├── dispatcher.h            dispatcher.cpp
│   │   └── server.h                server.cpp
│   ├── client/
│   │   ├── client_options.h        client_options.cpp
│   │   ├── load_balancer.h         load_balancer.cpp
│   │   ├── connection_pool.h       connection_pool.cpp
│   │   ├── registry_client.h       registry_client.cpp
│   │   └── channel.h               channel.cpp
│   └── registry/
│       ├── registry_client_proto.h registry_client_proto.cpp
│       └── registry_server.h       registry_server.cpp
├── codegen/
│   ├── qzcodegen/
│   │   ├── __init__.py
│   │   ├── __main__.py             # CLI 入口：--input/--out/--cpp/--check
│   │   ├── diagnostics.py          # 统一的 path:line:col 诊断
│   │   ├── lexer.py
│   │   ├── ast_nodes.py
│   │   ├── parser.py               # 递归下降
│   │   ├── semantic.py             # C-01..C-12 校验
│   │   ├── methodid.py             # FNV-1a64（必须与 C++ 完全一致）
│   │   ├── naming.py               # C++ 命名规则（类名/访问器/guard 名）
│   │   └── cpp_generator.py        # 生成 .qz.h / .qz.cpp
│   └── tests/
│       ├── _support.py             # repo_root() 等测试辅助
│       ├── test_parser.py          # lexer + EBNF 正反例（60 例）
│       ├── test_semantic.py        # C-01..C-12（61 例）
│       ├── test_methodid.py        # 与 C++ 侧交叉验证（17 例）
│       ├── test_golden.py          # 生成物逐字节比对 + 形状契约（38 例）
│       └── fixtures/
│           └── kitchen_sink.qz     # 打满分支的压力 IDL（CI 编译门的输入之一）
├── generated/                      # codegen 产物（提交入库，example 直接编译）
│   ├── echo.qz.h
│   └── echo.qz.cpp
├── example/
│   ├── echo_service_impl.h         echo_service_impl.cpp   # 业务实现
│   ├── echo_server.cpp             # 启动 Server + RegistryClient 注册
│   ├── echo_client.cpp             # 命令行压测/调用客户端
│   ├── registry_main.cpp           # 启动 RegistryServer
│   └── web/index.html              # 可视化 Demo 页：发请求 + 看 metrics
├── tests/
│   ├── test_endian.cpp             test_buffer.cpp
│   ├── test_writer_reader.cpp      test_codec.cpp          # 含 doc03 §14 全部用例
│   ├── test_frame_metadata.cpp     test_timer_queue.cpp
│   ├── test_thread_pool.cpp        test_load_balancer.cpp
│   ├── test_service_registry.cpp   test_dispatcher.cpp
│   └── test_e2e_echo.cpp           # 回环端到端：起 server，起 client，调 1000 次
└── scripts/
    ├── build.ps1                   # MSVC 一键构建（vcvars + cmake + ninja/msbuild）
    ├── build.sh                    # Linux 一键构建
    ├── run_demo.ps1                # 起 registry + server + client 跑通 Demo
    └── bench.ps1                   # 简易压测（多线程 + QPS/P99 输出）
```

### 6.1 include 约定

- 编译单元统一 `-I src`，因此所有 include 写成 `"common/status.h"` / `"protocol/frame.h"` 这种
  **相对 src 的路径**，不出现 `../`；
- `generated/` 单独 `-I generated`，生成物之间用裸文件名互引（`#include "echo.qz.h"`）；
- **禁止**任何头文件使用 `../` 相对路径。

### 6.2 编译要求（MSVC）

```
/std:c++17 /utf-8 /EHsc /W4 /permissive- /WX
```

`/utf-8` 不是可选项：所有源文件是 **UTF-8 无 BOM** 的中文注释，缺了它会以代码页 936 解读，
导致中文字符串被截断、甚至把后面的标识符吞掉而报出离奇的语法错误（本项目实测踩过）。

---

## 7. 扩展点

### 7.1 自定义序列化

**要做的事**：实现一个新的 `serde::Message` 子类（或用别的格式），并让 `Channel::Call` /
`Dispatcher` 认识它。

```cpp
// 只要满足 Message 的 5 个纯虚函数，就能被框架承载
class MyJsonMessage : public qzrpc::serde::Message {
 public:
  const char* TypeName() const override { return "my.Json"; }
  void Clear() override { /* ... */ }
  bool Encode(qzrpc::serde::Writer* w) const override { /* 写 JSON 字节 */ }
  bool Decode(qzrpc::serde::Reader* r) override { /* 解析 JSON 字节 */ }
  const qzrpc::serde::FieldMeta* Fields(std::size_t* n) const override { *n = 0; return nullptr; }
};
```

框架侧**无需改动**：`Serializer` 只调 `Encode/Decode`，`Codec` 只看 `payload` 是
"一段不透明字节"。这就是 D-08 的直接收益。

若要彻底换掉 wire 格式（例如换成 FlatBuffers），则实现一个新的 `protocol::Codec` 等价物，
把 `Connection` 的 `MessageCallback` 指向它即可 —— `Connection` 不认识帧结构。

### 7.2 自定义负载均衡

继承 `rpc::LoadBalancer` 三个方法即可：

```cpp
class LeastLoadedBalancer : public qzrpc::rpc::LoadBalancer {
 public:
  void UpdateEndpoints(const std::vector<qzrpc::net::Endpoint>& eps) override { /* 原子换表 */ }
  bool Select(const std::string& key, std::size_t attempt, qzrpc::net::Endpoint* out) override {
    /* 选在飞请求最少的；attempt>0 时选次少的 */
  }
  const char* name() const noexcept override { return "least_loaded"; }
  std::size_t size() const noexcept override { /* ... */ }
};

channel.SetLoadBalancer(std::make_unique<LeastLoadedBalancer>());
```

注意实现约束：`Select` 会在多个业务线程上并发调用，**必须自己保证线程安全**，
且不能在里面做 IO。

#### 7.2.1 内置三策略的退化行为（契约，不是"不确定性"）

| 策略 | 触发条件 | 行为 |
| --- | --- | --- |
| `round_robin` | 候选集只剩 1 个 | 永远选它（`attempt` 偏移取模天然收敛） |
| `random` | 候选集只剩 1 个 | 直接返回唯一项，**不消耗随机数** |
| `random` | `attempt > 0` 时抽回上一次的实例 | 在 `[0, n-2]` 里抽再映射跳过上次，**确定性地避开**（不是"多抽几次碰运气"） |
| `consistent_hash` | **`key` 为空** | **退化为轮询**（内部 cursor + `attempt` 偏移）。这是契约：给了空 key 就没有亲和性可谈，硬算一个固定位置只会让所有调用挤到同一实例 |
| `consistent_hash` | 环为空（`UpdateEndpoints({})`） | `Select` 返回 false → `Channel` 回 `kServiceUnavailable`，**不回退到"随便选一个"** |
| 任意策略 | `UpdateEndpoints` 收到非法/重复项 | 入口清洗：丢弃 `port == 0` / `ip` 为空的，按 `ip:port` 去重。因此 `size()` 可能**小于**传入长度 |
| 配置写了不认识的策略名 | 解析时 | 回退 `round_robin` 并写 err，**不静默失败** |

一句话原则：**"退化"只允许发生在"退化后仍然正确"的地方**（无 key、单实例）；
一旦"没有可用实例"，一律返回失败而不是乱指一个。前者不是 bug，后者绝不能发生。

### 7.3 拦截器 / 中间件

框架提供两个天然的挂点，且都**不需要改框架代码**：

| 位置 | 挂点 | 用途 |
| --- | --- | --- |
| 客户端 | 包一层 `Channel`：`class LoggingChannel { Channel& inner; Status Call(...) { 打日志; auto st = inner.Call(...); 打耗时; return st; } }` | 日志、链路追踪注入、鉴权 token 注入、mock |
| 服务端 | 生成的 `ServiceBase` 与业务实现之间插入一层装饰器：`class AuthService : public EchoServiceBase { EchoServiceBase* inner; ... }` | 鉴权、限流、审计、幂等去重 |
| 服务端（更底层） | `Dispatcher::SetFallback()` | 全局兜底：未知 method、网关转发、统一错误格式 |

**为什么不内置"拦截器链"**：一个链式框架需要定义 `Interceptor` 接口、注册顺序、
短路语义、异常安全边界。在没有真实需求驱动的情况下，这层抽象 90% 会写错。
用装饰器（组合）拿到同样的能力，且零框架侵入 —— 这是刻意的"不做"。

### 7.4 其他预留扩展点

| 扩展点 | 现状 | 如何扩展 |
| --- | --- | --- |
| 压缩 | 帧头有 `kFlagCompressed` 与 `metadata.compression`，v1 收到即报错 | 在 `Connection` 的消息回调前插一个解压步骤，并放开 `IsValidFlags` 的压缩位检查 |
| TLS | 无 | `Connection` 的 `::recv/::send` 换成 TLS 读写；`platform.h` 之外无需改动 |
| 指标导出 | `MetricsRegistry::DumpText()`（Prometheus 文本格式） | 加一个 `DumpJson()` 给前端页；加 OTel exporter 只需实现一个新的 dump |
| 服务发现后端 | 内置 RegistryServer | `RegistryClient` 的 `RefreshNow()` 可换成查 etcd/consul，接口不变 |
| 多协议共存 | 只有 QZRP | `Codec` 里加一段"首字节嗅探"来分派（HTTP 以 `GET/POST` 开头，QZRP 以 `QZRP` 开头） |

---

## 8. 架构约束的机械检查（CI 项）

这些规则不能只靠人自觉，必须由脚本检查：

| 检查 | 实现方式 |
| --- | --- |
| 公共头文件不得出现平台头 | `grep -rn "#include <\(winsock2\|sys/socket\|unistd\|sys/epoll\|windows\)" src/ --include=*.h` 只允许命中 `net/platform.h` |
| 分层依赖不得反向/成环 | 解析每个 `.cpp/.h` 的 `#include` 得到模块边，跑 DFS 找环；或直接检查"被依赖模块层号必须更小" |
| 头文件可独立编译 | 对 `src/**/*.h` 每个文件生成 `#include "x"` 的空 TU 并编译 |
| 生成物与生成器一致 | `python -m qzcodegen --input idl/*.qz --out /tmp/gen && diff -r /tmp/gen generated` |
| method_id 两侧一致 | `test_methodid.py` 用 100 条随机全名与 C++ 侧交叉验证 |
| 无第三方依赖 | 检查 `CMakeLists.txt` 里没有 `find_package`/`FetchContent`；`#include <...>` 白名单 |

> **已完成**：`src/` 下 37 个公共头文件 + `net/platform.h`（共 38 个头文件）在本机 MSVC 14.51
> （`/std:c++17 /utf-8 /W4 /WX /permissive-`）下**全部编译通过、零警告**。
> 这是"头文件契约没有空想 API"的实证。
