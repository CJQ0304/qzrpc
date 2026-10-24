# 轻舟 RPC (Qingzhou RPC)

一个 **C++17、零第三方依赖** 的 RPC 框架：自研二进制线协议 + 自研 `.qz` IDL 与代码生成器，
Windows(MSVC) 与 Linux(GCC) 双平台可跑，附带一个能端到端跑通的 Echo Demo 与指标可视化页。

> 名字取"轻舟已过万重山"：目标不是做一个功能最多的 RPC，而是一个**能被完整读懂**的 RPC。

---

## 核心特性

- **零第三方依赖**：只用标准库 + 平台 API，无 protobuf / boost / gtest / 任何包管理器。
- **自研线协议**：20 字节定长大端帧头 + TLV metadata + payload，长度前缀拆包，恶意包在头部即拒。
- **protobuf 兼容 payload**：varint / zigzag / packed / 未知字段透传，现有 pb 数据结构可直接复用。
- **自研 IDL 与 codegen**：`.qz` 文件经 Python 生成器产出**可读的** C++ 消息类 / Stub / Service / Dispatcher。
- **主从 Reactor + 有界队列线程池**：acceptor 与 IO 分离，业务阻塞只占一个 worker，不拖 IO。
- **内建指标**：Counter + 对数分桶 Histogram，直接输出 Prometheus 文本（P50/P90/P99/P999）。
- **优雅停机**：四步收敛（停止接入 → 拒绝新请求 → 排空在飞 → 收尾），不丢在飞请求。
- **可观测的失败**：统一 `Status` + 16 位线上状态码，重试策略按状态码分类而非"一律重试"。

---

## 架构

```
┌──────────────────────────────────────────────────────────────┐
│ L5 应用层   example/  generated/                              │
└───────────────────────────┬──────────────────────────────────┘
┌───────────────────────────▼──────────────────────────────────┐
│ L4 编排层   server/ (Reactor 组装·路由·停机)                  │
│             client/ (负载均衡·连接池·重试·服务发现)            │
│             registry/ (注册中心，用本框架自举)                 │
└───────────────────────────┬──────────────────────────────────┘
┌───────────────────────────▼──────────────────────────────────┐
│ L3 协议层   protocol/  frame(TLV) + codec(粘包拆包·校验)       │
└───────────────────────────┬──────────────────────────────────┘
┌───────────────────────────▼──────────────────────────────────┐
│ L2 序列化   serialize/ writer + reader + Message 抽象          │
└───────────────────────────┬──────────────────────────────────┘
┌───────────────────────────▼──────────────────────────────────┐
│ L1 网络层   net/  poller(epoll/WSAPoll) · event_loop · channel │
│                  acceptor · connector · connection · thread_pool│
└───────────────────────────┬──────────────────────────────────┘
┌───────────────────────────▼──────────────────────────────────┐
│ L0 基础设施 common/  logger · time_util · endian · buffer      │
│                     status · config · string_util · metrics   │
└──────────────────────────────────────────────────────────────┘
```

依赖方向单向向下（`Ln → Lm` 仅当 `m < n`），同层之间互不依赖，因此依赖图无环且可机械检查。
线程模型：**1 acceptor + N io + M worker + 1 housekeeping**，默认 22 个线程。

---

## 快速开始

### 1. 构建

```bat
:: Windows（MSVC + Ninja；脚本会自行设置 INCLUDE/LIB，不依赖 vcvars64.bat）
powershell -ExecutionPolicy Bypass -File scripts\build.ps1
```

```bash
# Linux / macOS
./scripts/build.sh
```

产物：`build/qzrpc.lib`（或 `libqzrpc.a`），单元测试由 `ctest` 驱动。

### 2. 生成代码

```bash
cd codegen
python -m qzcodegen --input ../idl/echo.qz --out ../generated --cpp
```

生成物 `generated/echo.qz.h` / `echo.qz.cpp` **提交入库**，因此不做这一步也能直接编译 example；
`--check` 只校验不落盘，供 CI 比对生成物与生成器是否一致。

> **开发约定**：改了 `idl/*.qz` 或 `codegen/qzcodegen/cpp_generator.py` 之后，必须重新生成
> `generated/` 并**把生成物一起提交**。`generated/` 是仓库的一部分，example 与 golden 测试
> 都直接读它，只提交生成器不提交产物会让 CI 的 `test_golden` 变红。生成器的落盘是原子的
> （同目录临时文件 → `fsync` → `os.replace`），所以生成与测试并发跑也不会读到半截文件。

### 3. 起服务

```bat
:: 注册中心 + 两个实例（三个终端，或者直接用下面的脚本）
build\example\registry_main.exe example\conf\registry.conf --port 16160
build\example\echo_server.exe   example\conf\echo_server.conf --port 16161 --metrics-port 9101
build\example\echo_server.exe   example\conf\echo_server.conf --port 16162 --metrics-port 9102
```

一键脚本有**两个**，按用途分，不按"高级/简单"分：

```bat
:: 演示（给人看）：registry + 两个实例 + 轮询分发 + 压测 + 并排 /metrics + 干净收尾
powershell -ExecutionPolicy Bypass -File scripts\demo.ps1
powershell -ExecutionPolicy Bypass -File scripts\demo.ps1 -BuildDir build_lead -Iterations 500

:: 冒烟（给 CI 看）：单实例，断言 + 退出码，约 1 秒
powershell -ExecutionPolicy Bypass -File scripts\smoke.ps1
powershell -ExecutionPolicy Bypass -File scripts\smoke.ps1 -Instances 2
```

`smoke.ps1` 对它自己的一行输出负责，这也是 CI 唯一需要解析的东西：

```
SMOKE OK calls=50 ok=50 fail=0 instances=1 ms=876.4
SMOKE OK calls=50 ok=50 fail=0 instances=2 split=25/25 ms=3296.6
```

`split=25/25` 只在 `-Instances 2` 时出现。它是"请求确实被分发到两个实例"的证据——判据是
两个实例自己 `/metrics` 里的 `qzrpc_server_request_total` **增量都 > 0**，而不是"两个进程都还活着"。
（Windows 上 `SO_REUSEADDR` 允许同端口被二次 bind，只看端口通不通会被骗过。）

分工：`demo.ps1` 是演示（三进程 + 逐步骤讲解 + 星形拓扑表，失败时也能看出坏在哪一步）；
`smoke.ps1` 是闸门（无颜色、无表格，只回答"绿还是红"）。两个脚本都是**纯 ASCII**：
cmd.exe 与 PowerShell 5.1 按 OEM 码页解析 `.ps1`，脚本里写中文注释会在某些宿主上直接报语法错。

注意 `--conf` 与命令行覆盖的顺序：`--port` / `--metrics-port` / `--bind` / `--log-level`
**在配置文件之后**生效，所以 `--conf x.conf --port 16162` 就是你指定的 16162（起两个实例
不会撞端口）。`registry.conf` 里写的 `port` 会被 `--port 16160` 覆盖——注册中心必须显式给端口，
否则它会和第二个 echo_server 抢同一个端口。

### 4. 调用与压测

```bat
:: 单次调用，打印往返结果与耗时
build\example\echo_client.exe --once

:: 8 线程 × 每线程 1000 次；字段顺序固定，便于日志抓取与对比
build\example\echo_client.exe --bench 1000 8
```

`--bench` 的输出格式（**字段顺序是契约，改动前请先对齐**）：

```
=== bench ===
requests   : 8000  concurrency: 8  duration: 1.842s
qps        : 4343.1
latency_us : p50=142  p90=310  p95=402  p99=615  max=1904  mean=184
errors     : 0 (0.00%)  overload=0  timeout=0
bytes      : tx=1.24MB  rx=1.31MB
served_by  : 16161=4012  16162=3988
# bytes = payload + 24B/frame; metadata body not counted
```

`errors` 行里 `overload` / `timeout` 单独成列，其余状态码按次数降序追加在同一行的 `also:`
后面；`bytes` 只统计帧头固定开销 + payload（Channel 追加的 metadata 正文不计，见最后那行脚注）。

指标页：`example/web/index.html`（纯原生 JS，无 CDN），点一下就能看到 QPS/P99 曲线与 Echo 往返耗时。

---

## 目录结构

```
CMakeLists.txt            顶层构建：显式源文件清单（不用 GLOB），/utf-8 /W4 /permissive-
src/common/               L0 基础设施：日志、时间、字节序、缓冲区、状态码、INI、字符串、指标
src/net/                  L1 网络：poller / event_loop / channel / acceptor / connector / thread_pool
src/serialize/            L2 序列化：protobuf 兼容的 writer / reader / Message
src/protocol/             L3 协议：帧结构、metadata TLV、粘包拆包
src/server/  src/client/  L4 编排：服务端组装、客户端负载均衡与连接池
src/registry/             注册中心（用本框架自举）
codegen/qzcodegen/        `.qz` 解析、语义校验、C++ 生成器
generated/                codegen 产物（**必须入库**：它是生成器的 golden 基准，删了 CI 就无从比对）
example/                  Echo Demo、注册中心入口、web 指标页
tests/                    单元测试（每个 test_*.cpp 一个 ctest 用例）
scripts/                  build.ps1 / build.sh / ci.ps1 / ci.sh / smoke.ps1 / demo.ps1 / bench.ps1
docs/                     任务拆分、架构、线协议、IDL 规范
```

---

## 测试

```bash
ctest --test-dir build --output-on-failure
```

- 测试框架是 `tests/test_main.h` 里的百余行断言宏，**不引入 gtest/catch2**（零依赖是硬约束）。
- 每个 `test_*.cpp` 编译成独立可执行 + 独立 ctest 用例：一个用例崩了不会拖垮其余用例。
- CI 建议额外打开 `-DQZRPC_WERROR=ON`，把"零警告"变成构建的硬门槛。

当前 9 个 ctest 用例（括号里是断言数，`ctest` 只报通过/失败，断言数要看用例自己的输出）：

| 用例 | 断言 | 覆盖什么 | 依赖外部工具 |
| --- | ---: | --- | --- |
| `test_endian` | 78 | 大端/小端读写、定长类型 | 无 |
| `test_buffer` | 92 | 环形缓冲、扩容、`Retrieve` 语义 | 无 |
| `test_writer_reader` | 233 | varint / zigzag / packed / 定长（protobuf 兼容层） | 无 |
| `test_codec` | 458 | 整帧编解码、粘包半包、任意位置切分、`PeekFrameSize` | 无 |
| `test_frame_metadata` | 510 | 帧头逐字节、flags 真值表、metadata TLV 与扩展键边界 | 无 |
| `test_service_registry` | 95 | 方法目录、method_id 重算比对（L2）、冲突与幂等 | 无 |
| `test_e2e_echo` | 38 | **进程内真实 Server + Channel 往返**：并发不串号、超时与迟到响应、错误码透传、300 KiB 载荷 | 无（端口用 0） |
| `codegen_python` | 180 | 生成器：AST、语义校验、golden 输出、method_id 一致性 | 无（标准库 `unittest`） |
| `interop_protobuf` | 3 | 本框架编出的字节能被**官方 protobuf** 解析（13 字段逐值比对） | `protoc` + `google.protobuf`，缺任一个自动 `***Skipped`（退出码 2） |

`interop_protobuf` 是唯一可能被跳过的用例，跳过时 ctest 会把它列进 `The following tests were not run`，
**不要**把 `***Skipped` 读成通过——这也是它用退出码 2 + `SKIP_RETURN_CODE` 而不是直接返回 0 的原因。
`QZRPC_REQUIRE_INTEROP=ON` 可以把"缺工具就跳过"变成"缺工具就 configure 失败"，用于发版前的强校验。

---

## 性能

压测脚本：`scripts/bench.ps1`（线程数 × 多轮扫描，报**中位数**与 min..max，并把机器画像一起写进
`build/bench.md`）。

先看清楚这张表在测什么，再看数字：

- **它是"单连接往返吞吐"**，不是服务端容量。`example/echo_client.cpp` 目前把
  `connections_per_endpoint` 固定成 1，所以所有客户端线程**共用一条连接**，请求在连接上串行。
  加线程只会加排队——实测线程数从 1 增加到 8，QPS 不升反降，这正是单连接串行的必然结果。
- 因此这里的数字衡量的是**一条连接的往返延迟上限**（loopback 上单次往返 ~50µs 量级），
  是一个有意义的指标，但它**不是**"这个框架能扛多少 QPS"。
- 真实容量需要 `连接数 × 线程数` 的二维扫描才谈得上。在拿到那个数之前，任何"3 万 QPS"
  都只能读作"延迟受限下的吞吐"，不能读作容量。

机器画像（`bench.ps1` 每次运行都会打印同样几行，换机器请一并带上）：

```
cpu     : Intel(R) Core(TM) i9-14900HX
cores   : 24 physical / 32 logical
memory  : 15.7 GB
os      : Windows 10.0.26200 (build 26200)
topology: echo_server 与 echo_client 是同机两个进程，走 loopback，中间没有网络
```

<!-- BENCH -->

---

## 已知边界

- `scripts/ci.sh`（Linux / macOS）按设计编写并做过逐条评审，但**未在 GCC 上实机跑过**——
  本机只有 MSVC。`scripts/ci.ps1` 的四阶段在 Windows 上是逐次跑绿的。这一点如实写在这里，
  而不是用一个没跑过的脚本冒充"跨平台已验证"。
- `bench.ps1` 的数字是 loopback 上的相对比较基准，不能当跨网络性能估计。

---

## License

MIT，见 [LICENSE](LICENSE)。
