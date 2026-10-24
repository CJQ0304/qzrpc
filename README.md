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

当前 13 个 ctest 用例（括号里是断言数，`ctest` 只报通过/失败，断言数要看用例自己的输出）：

| 用例 | 断言 | 覆盖什么 | 依赖外部工具 |
| --- | ---: | --- | --- |
| `test_endian` | 78 | 大端/小端读写、定长类型 | 无 |
| `test_buffer` | 92 | 环形缓冲、扩容、`Retrieve` 语义 | 无 |
| `test_writer_reader` | 233 | varint / zigzag / packed / 定长（protobuf 兼容层） | 无 |
| `test_codec` | 458 | 整帧编解码、粘包半包、任意位置切分、`PeekFrameSize` | 无 |
| `test_frame_metadata` | 510 | 帧头逐字节、flags 真值表、metadata TLV 与扩展键边界 | 无 |
| `test_service_registry` | 95 | 方法目录、method_id 重算比对（L2）、冲突与幂等 | 无 |
| `test_load_balancer` | 7460 | 轮询/随机/一致性哈希：分发均匀性（200 线程并发）、重试换实例、短 key 分散、并发刷新 | 无 |
| `test_thread_pool` | 796 | 有界队列拒绝、调用者运行、Stop 排空/丢弃、计数一致性 | 无 |
| `test_timer_queue` | 124 | 一次性/周期性定时器、惰性取消、回调内重入、到期顺序 | 无 |
| `test_dispatcher` | 111 | 路由注册与冲突、未知 method_id、截断 payload、错误码透传、deadline 判定 | 无 |
| `test_e2e_echo` | 38 | **进程内真实 Server + Channel 往返**：并发不串号、超时与迟到响应、错误码透传、300 KiB 载荷 | 无（端口用 0） |
| `codegen_python` | 180 | 生成器：AST、语义校验、golden 输出、method_id 一致性 | 无（标准库 `unittest`） |
| `interop_protobuf` | 3 | 本框架编出的字节能被**官方 protobuf** 解析（13 字段逐值比对） | `protoc` + `google.protobuf`，缺任一个自动 `***Skipped`（退出码 2） |

合计：**C++ 9,995 条断言**（11 个可执行）+ 生成器 180 + 互通 3。断言数不是多多益善——
`test_load_balancer` 那 7,460 条里绝大部分来自"200 次循环各断言一次"这类分布检查，
它们存在的意义是让"偶发偏差"变成必现失败，而不是凑数。

`interop_protobuf` 是唯一可能被跳过的用例，跳过时 ctest 会把它列进 `The following tests were not run`，
**不要**把 `***Skipped` 读成通过——这也是它用退出码 2 + `SKIP_RETURN_CODE` 而不是直接返回 0 的原因。
`QZRPC_REQUIRE_INTEROP=ON` 可以把"缺工具就跳过"变成"缺工具就 configure 失败"，用于发版前的强校验。

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

**噪声必须单独报，不能靠中位数吸收。** 取中位数本身不构成保护：机器持续噪声时三轮一样慢，
中位数照样好看。所以每个点都打印 `spread = (max-min)/median`，超过 30% 直接黄字告警并写进
`bench.md`：那些行只能读作"大概这个量级"，不能进表格。

机器画像（`bench.ps1` 每次运行都会打印同样几行，换机器请一并带上）：

```
cpu     : Intel(R) Core(TM) i9-14900HX
cores   : 24 physical / 32 logical
memory  : 15.7 GB
os      : Windows 10.0.26200 (build 26200)
topology: echo_server 与 echo_client 是同机两个进程，走 loopback，中间没有网络
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
- 这台构建机上，**刚链接出来（或刚被复制成新名字）的 exe 第一次执行会被实时杀毒扫描挡住**：
  实测 `>70s` 且**零输出**（连 `main()` 都没进），同一文件一分钟后跑 `0.9s` 通过。所以 ctest 的
  `TIMEOUT` 是 **180s**——它的职责是抓"真挂住"，不是抓"慢"。遇到偶发超时/零输出的排查顺序
  是**先环境后代码**：直接跑一次 exe，再立刻跑一次；两次都过就是扫描，不是回归。

---

## License

MIT，见 [LICENSE](LICENSE)。
