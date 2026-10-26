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

当前 14 个 ctest 用例（括号里是断言数，`ctest` 只报通过/失败，断言数要看用例自己的输出）：

| 用例 | 断言 | 覆盖什么 | 依赖外部工具 |
| --- | ---: | --- | --- |
| `test_endian` | 78 | 大端/小端读写、定长类型 | 无 |
| `test_buffer` | 92 | 环形缓冲、扩容、`Retrieve` 语义 | 无 |
| `test_writer_reader` | 233 | varint / zigzag / packed / 定长（protobuf 兼容层） | 无 |
| `test_codec` | 458 | 整帧编解码、粘包半包、任意位置切分、`PeekFrameSize` | 无 |
| `test_frame_metadata` | 510 | 帧头逐字节、flags 真值表、metadata TLV 与扩展键边界 | 无 |
| `test_service_registry` | 95 | 方法目录、method_id 重算比对（L2）、冲突与幂等 | 无 |
| `test_load_balancer` | 7460 | 轮询/随机/一致性哈希：分发均匀性（200 线程并发）、重试换实例、短 key 分散、并发刷新 | 无 |
| `test_thread_pool` | 796 | 有界队列拒绝、调用者运行、Stop 排空/丢弃、计数一致性、**任务抛异常不拖垮进程** | 无 |
| `test_timer_queue` | 124 | 一次性/周期性定时器、惰性取消、回调内重入、到期顺序 | 无 |
| `test_dispatcher` | 111 | 路由注册与冲突、未知 method_id、截断 payload、错误码透传、deadline 判定 | 无 |
| `test_event_loop` | 47 | **`Quit()` 早于 `Loop()` 调度必须能停**（曾有 `join()` 永久挂死）、停止不可重启、退出前排空任务队列、`RunInLoop` 内联 vs `QueueInLoop` 延迟 | 无 |
| `test_e2e_echo` | 38 | **进程内真实 Server + Channel 往返**：并发不串号、超时与迟到响应、错误码透传、300 KiB 载荷 | 无（端口用 0） |
| `codegen_python` | 180 | 生成器：AST、语义校验、golden 输出、method_id 一致性 | 无（标准库 `unittest`） |
| `interop_protobuf` | 3 | 本框架编出的字节能被**官方 protobuf** 解析（13 字段逐值比对） | `protoc` + `google.protobuf`，缺任一个自动 `***Skipped`（退出码 2） |

合计：**C++ 10,042 条断言**（12 个可执行）+ 生成器 180 + 互通 3。断言数不是多多益善——
`test_load_balancer` 那 7,460 条里绝大部分来自"200 次循环各断言一次"这类分布检查，
它们存在的意义是让"偶发偏差"变成必现失败，而不是凑数。

`test_event_loop` 的 47 条数不出它的价值：它守的是**进程级**缺陷。`Quit()` 早于 `Loop()` 到达时，
旧实现会抹掉退出标志、让 `join()` 永久挂死；在 e2e 里这条是偶发红（8 次挂 5 次），
只有钉成"`Quit()` 一定发生在 `Loop()` 之前"才是确定性用例。它的看门狗还让失败有指纹：
`kLateQuit`（补一次 Quit 才停 = 标志被抹掉）与 `kStuck`（彻底叫不醒），
而不是 180 秒后一句 `Timeout`。

`interop_protobuf` 是唯一可能被跳过的用例，跳过时 ctest 会把它列进 `The following tests were not run`，
**不要**把 `***Skipped` 读成通过——这也是它用退出码 2 + `SKIP_RETURN_CODE` 而不是直接返回 0 的原因。
`QZRPC_REQUIRE_INTEROP=ON` 可以把"缺工具就跳过"变成"缺工具就 configure 失败"，用于发版前的强校验。

---

## 可复现性

「clone 下来就能跑」这句话在本项目里是**测过的**，不是承诺。

```bash
git clone . /tmp/qzrpc-verify            # 全新目录：不带 build*、不带 .scratch/、不带工作区任何改动
cd /tmp/qzrpc-verify && scripts\ci.ps1   # 从零 configure + build + ctest + 三道 codegen 闸门
```

`ci.ps1` 的原始输出（2026-10-24，验证对象 **`56c9435`**，宿主 i9-14900HX / 32 逻辑核 / Win11 26200）。
此后只改文档/注释的提交按同一条命令重跑结果不变——最近一次重跑：**14/14**、`CI_EXIT=0`、32.4s。

```
==> Stage 1/4: build + ctest
==> Configuring (Release) -> D:\qzrpc_clone_final\build
==> CI gates enabled: /WX + codegen compile gate
-- Configuring done (2.6s)
[1/76] ... [76/76]
    ctest knows 12 C++ case(s), tests\ has 12 test_*.cpp
==> Running ctest
 1/14 Test  #1: test_endian ......................   Passed    0.05 sec
 5/14 Test  #5: test_thread_pool .................   Passed    0.25 sec
 6/14 Test  #6: test_load_balancer ...............   Passed    0.07 sec
11/14 Test #11: test_event_loop ..................   Passed    0.07 sec
12/14 Test #12: test_e2e_echo ....................   Passed    0.77 sec
14/14 Test #14: interop_protobuf .................   Passed    0.29 sec
100% tests passed, 0 tests failed out of 14
Total Test time (real) =   2.48 sec
    build time: 9.75s
==> Stage 2/4: qzcodegen --check on idl/*.qz
qzcodegen: 1 file(s) checked, protocol v1, 0 warning(s)
==> Stage 3/4: generated/ is in sync with idl/
    generated/ matches the generator output
==> Stage 4/4: compile gate [idl]       1 TU(s) compiled in 0.47s
==> Stage 4/4: compile gate [fixtures]  1 TU(s) compiled in 0.75s
==> CI OK
CI_EXIT=0
```

| 检查项 | 结果 |
| --- | --- |
| 提交态文件数 | **139**（`git ls-files \| wc -l`） |
| 克隆工作区 | `git status --short` → **0 行**，构建跑完之后再查仍是 0 行（构建不往工作区丢文件） |
| 编译 | **76 个 TU**，`/utf-8 /W4 /WX /permissive-`，零告警，9.75s |
| ctest | **14/14**（12 个 C++ 用例 + `codegen_python` + `interop_protobuf`），2.48s |
| `generated/` | 与现场重建**逐字节相同**（SHA256 比对）；前提是 `.gitattributes` 里的 `eol=lf` |
| 四阶段 | 全绿，`CI_EXIT=0`，端到端 19.3s |

这一步不是仪式，它抓到过一个真问题：**提交态与团队机器上编的代码不是同一份**。当时工作区
6 个 `M` 的修复（`%d` 收 `size_t`、线程池异常未收口、轮转算术、`Quit` 早于 `Loop`）让本地全绿，
而克隆出来的仓库 Stage 1 直接红：编译失败一处、`test_thread_pool` 以 `0xC0000409` 打死进程、
`test_load_balancer` 报 `differed == 100 (actual=0)`。
**「本地全绿」与「仓库全绿」是两个判据**，后者才是别人拿到的东西——所以「任何修复必须落地为
一次提交」是硬规矩（见「设计约定」第 4 条）。

已验证范围与**未**验证范围，分开写：

- **已验证**：Windows + MSVC 19.5x，从干净克隆到 `ci.ps1` 四阶段全绿（本仓库的默认路径）。
- **未验证**：POSIX 路径（`scripts/build.sh`、`scripts/ci.sh`）—— 代码已写、经逐条评审，
  但**未在本环境的任何 gcc / clang 上编译过**（本机只有 MSVC）。**跨平台不是本项目的验证范围**，
  具体到哪几条已知会被绕过、哪几条没验证，见下方「已知边界」第一条。

---

## 性能

压测脚本：`scripts/bench.ps1`（`线程数 × 连接数` 二维扫描，每点多轮取**中位数**并附 min..max
与 `spread` 列，机器画像一起写进 `build/bench.md`）。

先看清楚每一个数字是在什么口径下测出来的，再看数字本身：

- **每一行都必须带 `conns`（每 endpoint 连接数）。** 一条连接上的请求/响应是串行的，所以
  `conns=1` 测的是**单连接往返吞吐**（延迟指标），`conns ≥ 线程数` 才开始测服务端。同一份
  二进制、同一分钟、只改这一个参数：`conns=1` 24.1k QPS / p99 1339µs，`conns=8` 43.1k QPS /
  p99 277µs（1.79×）。**没有 conns 列的 QPS 表是错的表**，不是"不够精确"的表。
- `bench.ps1` 因此**总是显式传 `--conns`**，并且解析 `echo_client` 打出的
  `[client] connections_per_endpoint=8 source=cli(--conns 8) ...` 这行做校验：值不是
  `source=cli(...)` 或与实际不符，这一轮直接算失败。理由是"参数看起来传进去了"和"实际不同"
  是两件事——`-Threads 1,4` 曾经被 PowerShell 收成单个值 `14`，扫描跑了一个不存在的点。
- **此前出现过的 QPS 数字全部作废**，原因有两个，而且叠加：① 那些数字是 `conns` 被硬编码
  成 1 时测的（口径是"单连接往返"却当成容量在说）；② 同一台机器、相隔两分钟，1 线程 QPS
  实测 19,178 与 4,657，差 4 倍——当时有别人的构建和 ctest 在跑。"加线程 QPS 不升反降"这个
  旧结论主要来自 ①，不是框架不伸缩。
- **同机压测**：`echo_server` 与 `echo_client` 是同一颗 CPU 上的两个进程，在抢同一批核。
  所以表里的数值是**端到端联合吞吐**，**不是服务端容量上限**；服务端容量的真实数字需要在两台
  机器上测。不写这句，四万会被读成"服务端只能跑四万"——那是对我们自己不利的误读。
- **重试必须被关掉**：每一点都传 `--max-retries 0`，并要求客户端报出的 `retry_total` 为 0，
  否则这一轮判失败。原因很直接：带默认重试时，`errors : 0` 可能是"一次都没失败"，也可能是
  "失败的都被第二次救回来了"，而表上看不出区别。表里因此还有 `retry / failover / late` 三列，
  `late > 0` 意味着这一行已经发生过超时，它的 p99 是乐观值。
- **噪声必须单独报，不能靠中位数吸收。** 取中位数本身不构成保护：机器持续噪声时三轮一样慢，
  中位数照样好看。所以每个点都打印 `spread = (max-min)/median`，超过 30% 直接黄字告警并写进
  `bench.md`：那些行只能读作"大概这个量级"，不能进表格。

机器画像（`bench.ps1` 每次运行都会打印同样几行，换机器请一并带上）：

```
cpu     : Intel(R) Core(TM) i9-14900HX
cores   : 24 physical / 32 logical
memory  : 15.7 GB
os      : Microsoft Windows 11 10.0.26200 (build 26200)
topology: echo_server 与 echo_client 是同机两个进程，走 loopback，中间没有网络
shared  : 两个进程抢同一批核 → 端到端联合吞吐，不是服务端容量上限
```

这台 CPU 是**混合架构**：8 个 P 核（带超线程，16 线程）+ 16 个 E 核（无超线程，16 线程）。
线程轴因此有两个区制，`threads=16` 是分界：

- `threads ≤ 16`：每个 worker 都还能落在 P 核上，曲线反映的是框架的伸缩性；
- `threads > 16`：新线程落在 E 核上，单核性能约为 P 核一半且没有 SMT 兄弟，"加线程反而更慢"
  里有一部分是**架构代价**，与框架无关。

所以建议的线程点是 `8 / 16 / 24`——两个 P 核内的点 + **一个**超订点作为代价证据。铺满 1..32
得到的是"架构代价曲线"，把它印成"框架伸缩性曲线"是两个不同的结论共用一张表。
默认 worker 数的建议同样是这条：**延迟敏感场景不要超过 P 核线程数（本机 16）**，超订只抬
延迟不抬吞吐。

<!-- BENCH -->

---

## 设计约定（硬规矩，不是风格）

这三条都是被 bug 教出来的，每条后面都跟着它自己的事故。

**1. 性能旋钮可以推导，语义旋钮不可以推导。**

| 类别 | 例子 | 推错了会怎样 | 允许的来源 |
| --- | --- | --- | --- |
| 性能 | `connections_per_endpoint` | 只影响快慢 | conf / 显式 CLI / **按模式推导**（必须打一行来源日志） |
| 语义 | `max_retries` | **影响正确性：对非幂等调用，重试就是重复扣款** | 只有 conf 与显式 CLI，永不推导 |

事故：`max_retries` 曾按"端点数 > 1"自动置 0。同一份逻辑配置，`--servers a,b`（2 个端点）
得到 `retries=0`，而 `--conf`（里面也是这 2 个）得到 `retries=2`——行为只因为"怎么指定端点"
而不同。**这是唯一一类会让正确性悄悄变差、又完全不报错的推导**，所以提交前请扫一遍
`example/` 与 `src/`：凡是"根据别的参数推出一个语义参数"的地方，都要在评审里说明理由。

**2. 凡是"要多个环境同时满足才成立"的前提，必须钉死或写进文档（钉死优先）。**

| 前提 | 钉死方式 | 不钉死的后果 |
| --- | --- | --- |
| 行尾 | `.gitattributes` 的 `* text=auto eol=lf` | CI 逐字节比对 `generated/`，CRLF 机器上"干净仓库跑一次 CI"必然红 |
| 时钟精度 | `ci.sh` 里 `date +%s%N` 带 fallback | BSD `date` 打印字母 `N`，`set -e` 下直接打死脚本 |
| socket 语义 | 连接超时按"每次失败连接的确定成本"记账 | Windows 上失败的 `connect` 既不报可写也不报异常，`SO_ERROR` 一直是 0 |
| 路径分隔符 / locale | 脚本一律纯 ASCII、显式 `--out` | cmd.exe 按 OEM 码页解析 `.ps1`，中文注释变语法错 |

文档会被跳过，`eol=lf` 不会。这三行的共同点是：**不成立时没有任何报错，只是行为悄悄变成
另一个样子。**

**3. 验证要看实际清单，不看"看起来对"。**

- 提交后不看 `git status` 干净，看 `git ls-files` 的实际清单；
- 参数传完不看命令行，看程序自己打出来的来源日志；
- 跑完 ctest 不看"100% passed"，看**用例数对不对**（只跑 `cmake --build` 不会发现新加的用例：
  `configure` 之后才存在的文件，ninja 不会为它重跑 configure，于是"全绿"其实少跑了 4 条、
  8035 条断言）。

**4. 任何修复必须落地为一次提交。**

只存在于工作区的修复，**等于不存在**：它会在克隆里、在 CI 里、在任何人手里消失，而在作者机器上
永远"通过"。干净克隆验收是**检测**，提交习惯是**预防**；两者都要，但后者不该靠检查来保证。

事故：`git status` 里 6 个 `M src/**` 曾是"团队机器上全部测试都过"的唯一原因，而克隆出来的
仓库 Stage 1 直接红（编译失败 + `0xC0000409` + `differed=0`）。同一天里"绿"出现了两种含义，
而交付物只有一种。

**5. 挂死型缺陷的用例不能只靠 ctest 的 `TIMEOUT` 报红。**

`std::thread t(...); t.join();` 在回归时永不返回，ctest 在 180 秒后只给一句 `Timeout`——
**前面所有断言结果全丢**，你也永远不知道"它是在哪一步挂的"。要在用例内部设看门狗，
并让"怎么挂的"本身成为可断言的输出。

做法（`tests/test_event_loop.cpp`）：等 10s → 超时后**再 `Quit()` 一次**（回归形态下这一刀才叫得醒，
因为标志是被 `Loop()` 抹掉的）→ 仍不回才 detach。于是同一个回归有两种**可分辨指纹**：
`kLateQuit`（补第二刀才停 = 标志被抹掉）与 `kStuck`（彻底叫不醒），都判失败，但看日志就知道
是哪一种。同理，**成对用例**比单独的用例强：`test_event_loop` 的 A（`Quit` 早到也能停）与
G（停止后不能重启）互相牵制，任何一半被单方面改掉，另一半会红。

---

## CI 红了先看这里

`scripts/ci.ps1` 的四个阶段各自的失败指纹。先把"偶发"这个词从你的判断里去掉：**一次红就是一次红**，
"重跑就绿"对任何原因都成立，所以它什么都不能证明（下面第 3 行就是被这个想法坑过的实例）。

| 现象 | 第一个动作 | 已知的真凶 |
| --- | --- | --- |
| `error C2220: 以下警告被视为错误` | `git status` | 修复只在**工作区**没提交（本仓库实测的第一号原因）；其次才是真有新警告 |
| 用例 `Exit code 0xc0000409`、几秒即死、**零输出** | 看是不是线程函数里抛了异常 | `std::terminate` → 进程 fail-fast。`0xC0000409` 是它的指纹 |
| `Timeout 180s`，或某用例跑很久后被杀 | 看挂住前最后一行 `== TestXxx` 是谁 | 真挂死。**不要**归因到"重跑一次就好"：本项目唯一一次 60s 超时的真因是 `EventLoop::Loop()` 抹掉早到的 `Quit()` → `join()` 永久挂死，不是环境 |
| ctest 全绿但**条数变少** | 看 `ctest knows N / tests\ has M` 这行 | 只 `cmake --build` 没重新 configure；`build.ps1` 现在直接判红 |
| Stage 2 `codegen --check` 拒绝 | `codegen/tests` 里对应的用例 | IDL 语法或语义非法（`--check` 不落盘） |
| Stage 3 `content differs: echo.qz.h` | 重新生成一次并 `git diff` | 改了 `idl/` 或生成器却没重新生成；或工作区被换成 CRLF（`git config core.autocrlf`） |
| Stage 3 文件数/文件名不符 | `ls generated/`（**只应有两个文件**） | 有人把中间产物或临时文件落进了 `generated/` |
| Stage 4 `generated source does not compile` | 看报的 TU 名 | `idl` → 主 IDL 的字段形态；`fixtures` → `kitchen_sink.qz` 覆盖的边界形态（11 标量 × 4 label、packed、嵌套、INT64_MIN） |

另外两条与环境相关、**不报错只会卡住**的：

- **不要把 `ci.ps1` 接进不排空的管道**：`... | Tee-Object` 会让 `cl.exe` 在 `/showIncludes` 输出写满
  4KB 管道后阻塞（实测卡 10 分钟、CPU 累计 0.1s）。直接 `*>` 落文件，或让消费端持续读。
- **不要在别人构建/测试时跑 bench**：QPS 会差 4 倍，而中位数吸收不掉（见「性能」末尾）。

---

## 已知边界

- `scripts/ci.sh`（Linux / macOS）按设计编写并做过逐条评审，但**未在 GCC/Clang 上实机跑过**——
  本机只有 MSVC。已经验证过的是：脚本语法（`bash -n`）、缺编译器时的前置失败提示、
  以及 Stage 2/3 在两个**不同 Python 解释器**（3.x anaconda 与 3.13）下都产出与 `generated/`
  逐字节相同的文件。未验证的是 GCC 分支的 `-Werror` 行为与 macOS BSD userland 的边角
  （`date +%s%N`、`sort -z` 两处已知陷阱已在脚本里绕开）。
  这一点如实写在这里，而不是用一个没跑过的脚本冒充"跨平台已验证"。
- 这台构建机上 **`Start-Process` 不可用**（环境里同时有 `HTTP_PROXY` 与 `http_proxy`，
  PowerShell 5.1 的 `env:` 驱动大小写敏感，`Start-Process` 枚举时报
  "An item with the same key has already been added"）。`scripts/` 下的脚本一律用
  `[System.Diagnostics.Process]` + `ProcessStartInfo`，不要改成 `Start-Process`。
- `bench.ps1` 的数字是 loopback 上的相对比较基准，不能当跨网络性能估计；输出里的 `spread` 列
  与 `!! noisy` 告警要一起读——机器被别的任务占着时，中位数也会失真。**QPS 只在独占机器的
  窗口里测**，混在构建/测试里跑出来的数一个都不要引用。
- ctest 的 `TIMEOUT` 是 **180s**，它是**护栏**：整套用例正常 5 秒内跑完，所以这个数只在真挂死时
  才会触发。曾经把它设成 60s、并解释为"首次执行被杀软扫描挡住"—— **那个归因是错的，已撤回**。
  真因是 `EventLoop::Loop()` 抹掉早到的 `Quit()`（见 `tests/test_event_loop.cpp` 的文件头）。
  留在这里的一句话：**错误的环境归因是并发 bug 熬过一次重试的方式**；"重跑一次就好了"对任何
  原因都成立，因此它证明不了任何事。要证明，就像 `test_event_loop` 那样把窗口**构造**出来。

---

## License

MIT，见 [LICENSE](LICENSE)。
