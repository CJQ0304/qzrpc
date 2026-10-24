# 06 · 三分钟演示（照着敲就能跑完）

> 这份文档假设你只有 3 分钟：三条命令，三次"应该看到什么"，一屏判据，一节"这个演示没证明什么"。
> 所有输出片段都是从本机真实运行里粘出来的（Windows 10 + MSVC 19.x，2026-10-23/24），
> 数字没有美化，跑第二遍会不一样。

---

## 0. 先看这一屏

`scripts\demo.ps1` 的第 6 步，两台实例各自的 `/metrics` 增量：

```
    inst  port      connections   requests   errors     p99_us    mean_us    active
    A     16161               1      2,000        0         63         22         0
    B     16162               1      2,000        0         63         22         0

    4000 requests over 2 connection(s) -> 2,000 requests per connection
```

（`p99_us` / `mean_us` 是服务端自报的一次采样，每跑一遍都不一样，**这里不作比较**；
能引用的是 `connections=1` 和 `2,000 requests per connection` 这两个结构事实。）

**4000 次调用，总共只开了 2 条 TCP 连接**（每个实例 1 条），每条连接上跑完 2000 个请求。
这不是"连接数恰好是 1"，是连接池在设计上就这么定的：`connections_per_endpoint` 明确要求
`--conns 1`（不打这个参数时 `--loop` 模式也是 1），客户端在这个连接上把请求排队复用，
不重新握手。

这一屏要证明的是：**这个 RPC 没有"一次调用一条连接"的偷懒实现**。没有连接池的话，4000 次
调用就要付 4000 次 TCP + 握手的代价，`connection_total` 会是 4000，`p50` 会大一个数量级。

---

## 1. 三条命令

在仓库根目录（`D:\phxrpc`）开一个 PowerShell：

```powershell
# ① 构建 + 单元测试（全新目录实测 87 秒；增量 2~5 秒）
powershell -ExecutionPolicy Bypass -File scripts\build.ps1

# ② CI 闸门：起一个服务端、打 50 次、断言全部成功、退出码 0/1（实测 0.9~1.2 秒）
powershell -ExecutionPolicy Bypass -File scripts\smoke.ps1

# ③ 给人看的演示：注册中心 + 两实例 + 轮询 + 压测 + 注册心跳 + 杀掉一个实例（实测 13~20 秒）
powershell -ExecutionPolicy Bypass -File scripts\demo.ps1
```

三个脚本都默认 `-BuildDir build`，也就是 `build.ps1` 的默认输出目录，互相之间不需要传参。
如果你把构建产物放在别的目录（例如 `build_final`），三个脚本都接受 `-BuildDir build_final`。

`build.ps1` 默认会跑 ctest。如果某个用例报 `Timeout`，**别先重跑**——先看 §4 最后一行：
那个 60 秒超时曾经是 `EventLoop` 丢唤醒（已修），而"重跑就好了"对任何原因都成立，因此不排除任何原因。

`smoke.ps1` 只输出**一行** ASCII 结论，可以直接被 CI 解析：

```
SMOKE OK calls=50 ok=50 fail=0 instances=1 ms=860.3
```

| 字段 | 含义 |
| --- | --- |
| `calls` / `ok` / `fail` | 客户端请求数 / 服务端计数且非错误的请求数 / 差额 |
| `instances` | 起了几个实例（`-Instances 2` 时多一个 `split=A/B` 字段） |
| `ms` | 整个闸门的墙钟耗时（小数点固定用 `.`，不受 locale 影响） |
| `reason` | 只在失败时出现：`missing_binary` / `port_busy` / `server_not_ready` / `client_exit` / `count_mismatch` / `metrics_unreadable` |

退出码 `0` = 通过，`1` = 不通过。诊断信息只写 stderr（且只在 `-Verbose` 时），
所以 `smoke.ps1 > result.txt` 里必定只有那一行。

---

## 2. 每一步应该看到什么

### Step 2 — 注册中心

```
=== Step 2: Registry - registry_main on 16160 (/metrics 9103)

    expect: "QZRPC_READY" and /metrics answering on the metrics port
    registry       pid=27936
    OK   registry up: http://127.0.0.1:9103/metrics
```

注册中心自己就是一个 `qzrpc::rpc::Server`，只是把 `Register / Heartbeat / Fetch / Deregister`
四个方法注册进去。它跑得通 = 框架能自举，不需要第二套协议。

### Step 3 — 两个实例

```
    echo_server_A  pid=14740
    OK   A up: port 16161, /metrics 9101
    echo_server_B  pid=25152
    OK   B up: port 16162, /metrics 9102
    note: --port/--metrics-port win over the values in echo_server.conf
    (the binary loads the config first, then applies the command line on top).
```

最后那两行是本次演示特意打的：`echo_server.conf` 里写的是 `port = 16161`，
第二个实例能起在 16162，靠的是"**先读 conf、再套命令行**"这个顺序。
顺序反了的话 B 会静默退回 16161 并和 A 撞端口——这个 bug 我们真踩过。

### Step 4 — 轮询分发

```
    [client] connections_per_endpoint=1 source=auto(loop: single-threaded sequential) endpoints=2 total_conns=2
    [client] max_retries=0 source=cli(--max-retries 0)
    100 次调用：成功 100 / 失败 0，总耗时 19.896 ms，平均 0.199 ms

    分发统计（按响应里的 served_by = 服务端监听端口）
      端口         次数   占比
      16161              50    50.0%
      16162              50    50.0%
      合计            100
      → 2 个实例都收到了请求：轮询负载均衡在工作
    [client] retry_total=0 failover_total=0
    OK   traffic landed on both instances: A=50 B=50 of 100 (50.0% / 50.0%)
```

**这一屏的命令带 `--max-retries 0`，这是判据的一部分，不是随手加的。**
50/50 只有在一个前提下才算"分发证据"：失败的调用不能被悄悄重发到另一台。
带 2 次业务重试时，"A 收 50 / B 收 50" 既可能来自轮询，也可能来自重试把失败的那次送到了对面，
两种解释都成立 = 证据归零。压到 0 之后，A+B=100 且两边都 >0 只能来自轮询。

`[client] max_retries=0 source=cli(--max-retries 0)` 这一行就是这个前提的自证：
读者不用信文档，程序自己说了这次跑的是 0，而且是从命令行来的。
最后那行 `[client] retry_total=0 failover_total=0` 是**事后核账**：这一轮 100 次调用
一次重试都没用、也不需要换实例（两个端点都活着）。

`served_by` 是服务端在响应里回写的自己的监听端口，不额外引入协议字段。
脚本不靠这段中文判断，而是去两台实例的 `/metrics` 里读 `qzrpc_server_request_total` 的增量，
两边都 > 0 且相加等于 100 才算通过。

> 为什么"只看总数对得上"是假绿：Windows 上 `SO_REUSEADDR` 没有 `SO_EXCLUSIVEADDRUSE` 配着，
> 同端口可以被二次 bind。残留的旧实例抢走全部流量时，A=0 / 另一台=100 依然满足"总数 100"。
> 所以判据必须是"两边都 > 0 **且** 相加等于 N"。

### Step 5 / 6 — 两种连接数，同一批调用

同样 4000 次请求、同样 4 个线程，**只改每 endpoint 的连接数**：8 条（容量模式）对 1 条（复用模式）。

```
=== Step 5: Bench A - capacity mode: 4 threads x 1000 calls = 4000 calls
    [client] connections_per_endpoint=8 source=auto(bench threads=4 -> max(8,threads)) endpoints=2 total_conns=16
    [client] max_retries=2 source=default(ClientOptions)
    === bench ===
    requests   : 4000  concurrency: 4  duration: 0.090s
    qps        : 44395.6
    latency_us : p50=49  p90=69  p95=81  p99=116  max=19438  mean=77
    errors     : 0 (0.00%)  overload=0  timeout=0
    bytes      : tx=198.64KB  rx=132.81KB
    served_by  : 16161=2000  16162=2000
    [client] retry_total=0 failover_total=0

=== Step 6: Bench B - reuse mode: same 4000 calls, but --conns 1
    [client] connections_per_endpoint=1 source=cli(--conns 1) endpoints=2 total_conns=2
    [client] max_retries=2 source=default(ClientOptions)
    ...
    requests   : 4000  concurrency: 4  duration: 0.144s
    qps        : 27728.3
    latency_us : p50=90  p90=204  p95=285  p99=596  max=25133  mean=139
    [client] retry_total=0 failover_total=0

    p99: capacity mode 116us  vs  reuse mode 596us  -> 5.1x
```

上面两段是程序原样输出，**一个字都没改，包括 `qps` 那两行**。
但请把 `qps` 读成"这一次跑出来的一个观测"，不要读成容量：

> `qps 44395.6` / `qps 27728.3` 是单次观测，未做噪声控制，**不作结论，不作比较**。

`[client] retry_total=0 failover_total=0` 这两行还回答了另一个问题：**`errors: 0` 不是"重试救回来的 0"**。
这一轮 4000 次调用一次业务重试都没发生、也没有换过实例——所以那个 0 就是"一次都没失败过"。
（`echo_client` 把两个预算分开计数：`retry_total` 是业务重试，`failover_total` 是换实例的故障转移。）

理由是实测出来的：同一份代码、同样配置，在这台机器上复跑，绝对值能差 60% 到 2 倍
（做 `--conns 1/2/4/8/16` 五点扫描时，`--conns 4` 有一轮 63.5k、另一轮 31.5k，
中间没有任何代码改动，是机器上别人的构建在抢核）。
**能引用的只有形状**：p50/p90/p95/p99 随连接数**单调下降**，两轮扫描完全一致；
绝对值差多少，两轮差得就不一样。

所以这一屏的主张是下面这句，不是上面那两个 QPS：

> **同样的 4000 次调用、同样的 4 个线程，`--conns 1` 的 p99 是 `--conns 8` 的 3~6 倍。**
>
> 实测三轮：3.6×（207µs vs 744µs）、6.1×（116µs vs 705µs）、5.1×（116µs vs 596µs）。
> 绝对倍数与绝对微秒数都随机器噪声变化，所以这一组**标"待复测"**——
> 独占窗口统一测一次再入档。方向与量级（个位数倍数、不是百分比差异）三轮一致。

为什么 p99 比 QPS 可信：一条 TCP 上的请求是**串行**的，4 个线程挤 1 条连接时，第 4 个请求
必须等前 3 个在同一个 socket 上写完才能被读；这种排队在 p99 上是**结构性**的，
在 QPS 上只表现为"快一点/慢一点"，而快慢恰好是这台机器最能骗人的部分。

**一条配套的结构事实**（来自 Step 6 的 `/metrics` 增量，不受机器快慢影响）：

```
    inst  port      connections   requests
    A     16161               1      2,000
    B     16162               1      2,000

    4000 requests over 2 connection(s) -> 2,000 requests per connection
```

2000 个请求压在 1 条连接上 —— 这就是 p99 变差的原因，也是"有连接池、没有一调用一连接"的证据。

`[client] connections_per_endpoint=N source=...` 这行是硬要求：**连接数从哪来必须能一眼看出来**。
`source` 有四种取值——`cli(--conns 4)`（命令行显式）、`conf(<路径>)`（配置文件）、
`auto(bench threads=4 -> max(8,threads))`、`auto(loop: single-threaded sequential)`。
这条日志存在的原因是"用户设的值被无声改掉"这类缺陷（连接数曾被硬编码成 1、`--port` 曾被 conf 覆盖），
它们共同特征是程序不报错、行为却是另一个。

`max=22541` 是每条连接上的第一次调用（TCP + 握手），`p50`/`p99` 不含它。

> **同机压测的口径限制**：`echo_server` 和 `echo_client` 跑在同一台机器、抢同一批核。
> 所以这两段测出来的不是"服务端容量上限"，而是**客户端 + 服务端在这台机器上的端到端联合吞吐**。
> `--conns` 继续加大时只涨一点，很可能先饱和的是客户端，不是服务端。
> 服务端容量的真实数字必须在**两台机器**上测，本节没有这个条件，也就不给这个数字。

### Step 7 — 注册中心看到几个实例

```
    qzrpc_registry_register_total  2
    qzrpc_registry_heartbeat_total 2
    OK   2 instances registered, and both are still heartbeating (not just written once)
```

`register_total = 2` 只证明"登记过"，`heartbeat_total > 0` 才证明"还活着"。
心跳周期是 `ttl/3 = 5s`，所以这一步固定等 6 秒；等不足 5 秒时 `heartbeat_total` 理应 0，
脚本会把这条判据跳过而不是判失败（否则等于训练人忽略告警）。

### Step 8 — 现场杀掉一个实例：故障转移

脚本在第 8 步**自己动手**把 B 杀掉了，然后带 `--max-retries 0` 打 20 次（所以这一屏不用手动敲）：

```
=== Step 8: Failover - kill instance B, then 20 calls with --max-retries 0
    killed echo_server_B (pid 29140) on purpose
    [client] connections_per_endpoint=1 source=auto(loop: single-threaded sequential) endpoints=2 total_conns=2
    [client] max_retries=0 source=cli(--max-retries 0)
    20 次调用：成功 20 / 失败 0，总耗时 210.023 ms，平均 10.501 ms

      端口         次数   占比
      16161              20   100.0%
    [client] retry_total=0 failover_total=19
    OK   20/20 served by A with retry_total=0, failover_total=19 (switches, not retries); A request_total +20
    restarting B so the next step (and -KeepRunning) leaves a healthy pair
```

**这一屏是"证明"而不是"主张"，靠的是三个自己打印出来的事实：**

1. `[client] max_retries=0 source=cli(--max-retries 0)` —— 业务重试预算是 0。
   失败之后**不允许再发一次**。
2. `channel.cpp` 的 `created: ... retries=0` 同一次运行里也说了一遍，两处一致。
3. **`[client] retry_total=0 failover_total=19`** —— 事后核账：这一轮**换了 19 次实例、
   用了 0 次业务重试**。19 次切换解释了 20 次调用为什么都活下来；`retry_total=0` 排除了"是重试救的"
   这个竞争解释。**两个计数器分开计**，就是为了让这句话能被机器验证，而不是靠讲解。

**口播（这是这一屏的全部台词，约 30 秒）**：

> "`--max-retries 0` 关掉的是**重试轮次**，不是**故障转移**。故障转移发生在**同一轮之内**：
> 一个实例建连失败、**一个字节都没发出去**，立刻换另一个还没试过的实例，不睡、不计预算。
> 所以 20/20 全部来自候选切换——`retry_total=0` 排除了"是重试救的"，`failover_total=19` 正面证明了换过。"

代价只有那次失败的建连：**约 210ms 里约 200ms 是给死实例烧的一次连接超时**
（`acquire_timeout_ms=200`），剩下 10ms 才是业务往返。

> 分层的完整定义（每层计什么单位、上限多少、触发条件）只有一处出处：**`docs/02-architecture.md` §4.3 的四层表**。
> 本节不重述，只给这一屏的实测——重复描述过一次，后来改契约时两边就漏改过一次。

> 顺手一个对照实验（同机、同场景，只改重试预算）：`--max-retries 2` 与 `--max-retries 0`
> 得到的都是 `retry_total=0 failover_total=4`（那是 `--loop 5` 的一轮）。
> **重试预算改了，故障转移次数一次没变**——两个预算确实是两回事。

#### `failover_total=19` 不是统计错了：双实例的游标会被奇偶锁住

直觉上 20 次调用里应该只有一半先撞上死的那个（≈10 次切换）。实测是 **19**，原因是确定的：

- 轮询游标**成功一次 +1**（奇偶翻转），**失败一次 +2**（先撞死实例、再换到活的那个，**奇偶不变**）；
- 所以只要游标落到奇数（正指向死实例），之后**每次调用都从死实例开始**，每次恰好 1 次切换；
- `19 = 20 − 1`，只有第一次落在 A。

实测对照（同一台机器，只改实例数，都带 `--max-retries 0`）：

| 端点列表 | 活/死 | 调用数 | `failover_total` | 分发 |
| --- | --- | --- | --- | --- |
| A, 死 | 1/1 | 20 | **19** | 全部落 A |
| A, 死, C | 2/1 | 10 | **5** | A=5, C=5 |
| A, 死, C, 死 | 2/2 | 10 | **9** | 锁住 |

**奇数实例不锁**（失败推进 2，模 3 会轮转），**偶数且挂掉一半会锁**。
所以 `size == 2` 是这一整族现象的共同放大器——这也是为什么我们两次都只在双实例下才看出来。

这是**选择器的局部偏好被固定**，不是重试失控：代价只有一次原子读 + 一个判断
（冷却保证它不必真的发起 TCP），不影响正确性，也不影响 20/20 的结论。
演示时如果被问到"为什么不是 10"，照上表回答即可；三实例那行是最便宜的对照。

> ⚠️ **反过来也成立，而且更要紧：三实例验收通过，不能当成"选择器正确"的证据。**
> 今天这一族问题已经踩了两次（`(ticket + attempt)` 抵消、奇偶锁定），**两次都是三实例测绿、
> 双实例才撞出来**——而双实例恰好是最常见的部署形态。做验收的人请至少跑一次双实例。

> 为什么这个前提必须写在命令行上：`example/conf/echo_client.conf` 里是 `max_retries = 2`。
> 不带 `--max-retries 0` 时，那一屏同样会 20/20，但"是故障转移还是重试"两种解释都成立，**证明力归零**。
> 同一个理由适用于 Step 4 的 50/50 —— 所以两步都显式传 0，前提出现在调用点上，评审者能自己复现。

**为什么 `max_retries` 不做"多端点就自动压成 0"的推导**：它是**语义**参数，决定"失败后会不会再发一次"，
对非幂等调用，重试就是重复扣款。推导错了是正确性问题，不是快慢问题，所以只允许两个来源：
`conf` 与显式 `--max-retries`。性能参数（`connections_per_endpoint`）可以推导，语义参数不行。

> 手动复现这一屏：见 §2.5 屏 A。脚本做的是同一件事，区别只是它还会把 B 重新拉起来。

---

## 2.5 额外三屏：现场制造故障（**屏 B/C 不在脚本里，要手动敲**）

上面 1–8 步只演"脚本自己安排的剧情"。面试/评审时真正值钱的是下面三屏——它们证明"**失败时的行为**"，
而这正是 RPC 框架最容易糊弄过去的部分。

> **屏 A（杀一个实例）脚本的第 8 步已经自动做了**，这里的命令是留给你在提问时**当场手动复现**用的：
> 评审者更愿意相信自己敲的命令。屏 B（全杀）和屏 C（浏览器）仍需手动，脚本没做。

**准备**：用 `demo.ps1 -KeepRunning` 跑一遍，三个进程留着别杀（脚本会打印每个 pid）。
下面所有客户端命令都在**另一个窗口**里敲。

### 屏 A（约 30 秒）：杀掉 B —— 故障转移

```powershell
Get-Process echo_server | Select-Object Id, StartTime   # 后起的那个是 B
Stop-Process -Id <B 的 pid> -Force

build\example\echo_client.exe --servers 127.0.0.1:16161,127.0.0.1:16162 --loop 20 --max-retries 0
```

**应该是**：

```
    [client] max_retries=0 source=cli(--max-retries 0)
20 次调用：成功 20 / 失败 0，总耗时 212.xxx ms，平均 10.6xx ms
    分发统计（按响应里的 served_by = 服务端监听端口）
      端口         次数   占比
      16161              20   100.0%
    [client] retry_total=0 failover_total=19
```

**口播（25 秒）**：

> "B 已经死了，但 20 次**全部成功**。请先看这两行，都是客户端自己打的：
> `[client] max_retries=0 source=cli(--max-retries 0)`——**这一屏的客户端是用 `--max-retries 0` 跑的**，
> 也就是"关掉的是重试轮次、不是故障转移"；
> `[client] retry_total=0 failover_total=19`——**换了 19 次实例、用了 0 次重试**。
> 所以 20/20 全部来自候选切换：客户端在建连阶段就发现 B 连不上、一个字节都没发出去，直接换 A。
> 这两百多毫秒里，约 200ms 是给那个死实例的一次连接超时（`acquire_timeout_ms=200`），
> 剩下的十几毫秒才是真正的业务往返。"

**这屏的分量在一个 before/after 上**（实测，同一台机器）：修故障转移之前，同样的场景是
**10/20 成功、7388ms**——一半请求白等一轮超时；修完是 **20/20 成功**（两次运行分别是
210ms 与 212ms，同一量级）。
失败率从 50% 到 0%，是用户唯一真正能感知的那个指标。

### 屏 B（约 25 秒）：全杀掉 —— 快速失败，而不是挂死

```powershell
Stop-Process -Name echo_server -Force      # A 也杀掉，现在一个都不剩
build\example\echo_client.exe --servers 127.0.0.1:16161,127.0.0.1:16162 --loop 20 --max-retries 0
```

**应该是**（退出码 1，这是对的；下面是本机真实输出，只删了中间的重复行）：

```
  [  0] FAIL Timeout: connect to 127.0.0.1:16162 timed out: connect(127.0.0.1:16162) timeout after 200ms (after 206ms, budget 200ms)
  [  1] FAIL ServiceUnavailable: acquire connection to 127.0.0.1:16162 failed: endpoint in cooldown, retry after 3000ms
  [  2] FAIL ServiceUnavailable: acquire connection to 127.0.0.1:16162 failed: endpoint in cooldown, retry after 3000ms
  ...
20 次调用：成功 0 / 失败 20，总耗时 407.756 ms，平均 0.000 ms
```

**口播（25 秒）**：

> "全死了。**第一次** 206ms——一个死端点烧掉一次连接超时（`acquire_timeout_ms=200`）；
> **从第二次开始每次不到 1 毫秒**：失败会把 endpoint 放进冷却（3s 起、连续失败翻倍、上限 30s），
> 冷却期内 `Acquire` 立即失败，一个 TCP 都不发起。所以 20 次全失败只花了 408ms，
> 而不是 20 × 200ms 的连乘。
> 注意失败原因从 `Timeout: connect ...` 变成了 `endpoint in cooldown`——
> **这两个字符串是同一个设计的两半**：前者是"试过，不通"，后者是"不问，已知不通"。"

**这一屏也要带 `--max-retries 0`，两个理由**（上面的数字因此和以前贴的不一样，是重测的）：

1. **让第一手错误可见**。带默认的 2 次重试时，同样的命令第一行报的是
   `ServiceUnavailable: ... endpoint in cooldown`——真实原因（`connect timeout`）被重试盖掉了，
   冷却反而成了结论。失败诊断里，"最终报错"经常不是"真实原因"，这是同一类问题。
2. **让"重试换来了什么"这件事被看见**。同样的 20 次、同样的全死，**成功次数都是 0**（20 次全失败），
   但耗时是 **408ms（retries=0）vs 1963ms（retries=2）**。重试在这里没换来任何一次成功，
   只换来了约 5 倍等待。这也正是 `max_retries` 必须显式给出的理由：**它会改变你观测到的现象。**

> ⚠️ 如果这一屏第一次也是秒回（没有 200ms 那一下），说明屏 A 留下的冷却（3 秒）还没到期。
> 别慌，这恰好是加分的说法："冷却还在生效，所以连第一次尝试都省了。"

> **别和 §2 的 Step 8 混，两屏证明的是相反的东西**：
> 屏 A / Step 8 是**杀一个实例 → 20/20 成功**（走故障转移，换到活着的那个）；
> 屏 B 是**全杀 → 20/20 失败**（没有候选可换，走冷却快速失败）。
> 一个证明"能换"，一个证明"换不了就别拖"。拿屏 B 的失败数据去证 Step 8 的成功结论，
> 是这组演示里最容易被一句话打穿的地方。

### 屏 C（约 25 秒）：浏览器里看指标

把两个实例重新起起来（重跑 `demo.ps1 -KeepRunning`），然后**双击** `example\web\index.html`。

**应该是**：左上状态点变绿、`每 3 秒` 自动刷新；四张 KPI 卡（QPS / P99 / 错误率 / 活跃连接）；
两条折线（QPS 最近 60 个采样点、P50 / P99）；一张六行的计数器表，每行带"比上次涨了多少"。

**口播（25 秒）**：

> "这个页面没有后端，就是 `fetch()` 两个 `/metrics`。页面能打开就说明三件事：
> ① 端点说的是标准 Prometheus 文本格式；② 它带 `Access-Control-Allow-Origin: *`，
> 所以 `file://` 的 `Origin: null` 也能读；③ 数字都是进程内原子量，抓取不需要锁。
> 表里那列 `+N` 是两次采样的差分——这也解释了为什么**活跃连接是 gauge 不是 counter**：
> counter 只会涨，给不出'当前几条'。"

---

## 3. 四条判据（验收看这四个就够）

| # | 判据 | 从哪读 | 期望 |
| --- | --- | --- | --- |
| 1 | **连接复用**：4000 次调用只用 2 条连接 | 两台实例的 `qzrpc_server_connection_total` 增量 | 各 1，`requests/connection = 2000` |
| 2 | **轮询真的在分发** | 两台实例的 `qzrpc_server_request_total` 增量 | 都是 `N/2`，相加等于 `N` |
| 3 | **注册中心能拉到实例且没死** | 注册中心 `/metrics` 的 `qzrpc_registry_register_total` / `_heartbeat_total` | `>= 2` / `>= 1` |
| 4 | **故障转移不是重试**（Step 8） | `[client] max_retries=0 source=cli(...)` + `[client] retry_total=0 failover_total>=1` + 保留实例的 `request_total` 增量 | 杀掉 B 后 20 次全部成功、都落在 A，且**切换次数 ≥ 1、重试次数 = 0**（本机双实例实测 19：游标奇偶锁，见 Step 8 末） |

第 4 条的前半句比后半句重要：**如果那一屏没有 `--max-retries 0`，20/20 就不能当证据**——
它既可能是故障转移，也可能是业务重试把失败的调用送到了对面，两种解释都成立。
`retry_total` / `failover_total` 这两个计数器就是用来**排掉那个竞争解释**的：
分开计数之后，"换了几次实例"和"重发了几次"不再是一个数，读者不需要相信任何解释。

四条都失败会明确报红色 `FAIL` 并给退出码 1，不会"看起来跑完了"。

---

## 4. 失败时怎么办

| 症状 | 原因 | 处理 |
| --- | --- | --- |
| `port(s) still in use after 3s: 16161` | 上一次演示的子进程还没退，或另一个演示在跑 | 脚本已等 3 秒；仍占用就 `Get-NetTCPConnection -LocalPort 16161 \| Select-Object OwningProcess` 找 PID 后 `Stop-Process -Id <pid>`，或换端口：`-ServerPortA 16261 -MetricsPortA 9201` |
| `server_not_ready` / `exited with code N before serving /metrics` | 端口被占、配置非法、二进制过期 | 脚本会立刻停下来并打子进程最后的 stderr；先看那几行，再重跑 `scripts\build.ps1` |
| 浏览器打开页面一片"连接失败" | 端点没起，或起的不是 9101/9102 | 页面默认抓 `http://127.0.0.1:9101/metrics` 与 `:9102/metrics`；起服务端时确认 `--metrics-port` 是这两个值 |
| 演示结束后页面就空了 | `demo.ps1` 默认会把两个实例停掉 | 用 `demo.ps1 -KeepRunning`，它会打印两个 pid 和停止命令 |
| Safari 里 `fetch` 被拦 | Safari 禁止 `file://` 页面发 `fetch` | 用静态服务器：`python -m http.server -d example/web 8080`，然后开 `http://127.0.0.1:8080/` |
| 只起了一个实例 | 客户端只能连上一个端点 | `--loop` 仍会成功（100/100），但分发那一步会判失败并提示"A=100 B=0"；这是**预期行为**，不是 bug |
| Step 8 报 `succeeded N/20`（N < 20）或 `A served only N` | 杀掉 B 之后 16162 上还有别的进程在听（Windows 的 `SO_REUSEADDR` 允许同端口二次 bind，残留实例会分走流量），流量被分走 | 先看那几行 `[client]` 输出里 `max_retries=0 source=cli(--max-retries 0)` 在不在；在的话用 `Get-NetTCPConnection -LocalPort 16162 \| Select-Object OwningProcess` 找出残留进程杀掉再重跑 |
| 防火墙弹窗 / 连不上 | `--metrics-bind-ip` 默认 `127.0.0.1`，只有本机能访问 | 本机演示不用改；要从别的机器看，显式改 `metrics_bind_ip` 并放行该端口 |
| 脚本报"无法加载文件，未对文件进行数字签名" | 默认执行策略是 `Restricted` | 按本文给的 `-ExecutionPolicy Bypass` 调用，或 `Unblock-File scripts\*.ps1` |
| **e2e 用例偶发挂死（约 60s 后 `Timeout`）：已定位并修复** | 真因是 `EventLoop::Loop()` 进循环时把退出标志清零，**丢掉了早到的 `Quit()`**（`src/net/event_loop.cpp:146` 附近，已修）。诊断方式是加 trace 看到 `[loop] START` 出现在 `QUIT requested` **之后**、`iter#1 quit=0` 把退出标志抹掉；修复前 8 次里挂 5 次，修复后 20/20、该用例 0.99s 通过 | 如果你在旧提交上遇到同一个现象，先查 `quit_` 的写入点，别先怀疑环境。**注意："重跑一次就好了"对任何候选原因都成立（杀软扫描、丢唤醒、机器被压着），所以它什么也没排除**——这是当时走过的一条弯路。另：刚链接的二进制首跑可能确实很慢（实测过 >70s 零输出），但那个观察**没有被证实为任何一次 CI 超时的原因**，不要再拿它当结论 |

---

## 5. 这个演示证明了什么、没证明什么

**证明了的**

- 线协议在真实 `Channel` + 真实 `Server` 之间跑通：请求/响应帧、`served_by`、`method_id` 都对得上。
- 连接池确实在工作：4000 次调用 / 2 条连接（判据 1，可复现）。
- 轮询分发在两端都成立：客户端按 `served_by` 统计 50/50，两台服务端自己的计数器也是各 50。
- 注册中心能自举：它自己就是一个 `Server`；两个 `echo_server` 用同一个 `Channel` + 同一套
  `Codec` 完成 `Register` 与 `Heartbeat`，`register_total=2`、`heartbeat_total>0`。
- `/metrics` 是真实的 Prometheus 文本端点：`HTTP/1.0 + Connection: close`，带
  `Access-Control-Allow-Origin: *`，`OPTIONS` 回 204，非 `GET` 回 405。
- **故障转移与业务重试是两件事**（Step 8）：`--max-retries 0` 下杀掉一个实例，20/20 仍全部成功，
  且全部落在活着的那个实例上；客户端自己报的 `retry_total=0 failover_total=19` 说明
  "换实例 19 次、重发 0 次"——两个预算是分开计的，前提也写在命令行里。
- `connections_per_endpoint` 是可解释的变量，不是玄学：**同 4000 次调用、同 4 线程**，
  `--conns 1` 的 p99 是 `--conns 8` 的 3~6 倍，形状（p99 随连接数单调下降）两轮一致（第 5/6 步 A/B）。

**没证明的（别拿去当结论）**

- **没验浏览器渲染**。`example/web/index.html` 只有一个内联 `<script>`，脚本能确认的是
  "页面要抓的两个端点真的在返回可解析的文本"，**不是**"图表画对了"。CSS/布局/图表库行为
  在本次演示里完全没有验证。
- **没有跨机器网络**。所有数字都是同一台机器上的 loopback：没有真实 RTT、没有丢包、
  没有 MTU/分片、没有跨机房带宽限制。任何"生产环境能跑多少"的推算都不成立。
- **同机压测 ≠ 服务端容量**。`echo_server` 与 `echo_client` 在同一台机器上抢同一批核，
  测出来的是**端到端联合吞吐**，不是服务端容量上限；`--conns` 继续加大只涨一点时，
  先饱和的可能就是客户端。服务端容量的真实数字只能在**两台机器**上测——本次没有这个条件。
- **性能数字只代表本机、本时刻**。这台机器上同时有别的构建/测试在跑，同一配置复跑能差
  2 倍。**唯一可以引用的性能数字在 `scripts/bench.ps1` 的输出里**，本节不抄数字，
  以免同一个数字出现两个出处；能跨机器引用的是**比值与形状**（例如 p99 的 3~6 倍），
  不是绝对值。
- **没有 TLS、没有鉴权、没有多租户隔离**。`/metrics` 端点无鉴权，只应绑在回环/内网。
- **没有长稳压测**：最长的跑法是 4000 次调用、0.1 秒量级。没有跑过分钟级/小时级，
  因此没有内存增长、连接泄漏、句柄耗尽、`TIME_WAIT` 堆积方面的任何结论。
- **只在 Windows + MSVC 上跑过**。Linux/macOS 未验证（`scripts/ci.sh` 存在，但本演示没跑）。
- **`bytes` 行是保守估计**：统计的是 `payload + 24B/帧`（20B 帧头 + 4B metadata 长度前缀），
  Channel 追加的 metadata 正文没算进去——宁可少算并注明，也不编一个更漂亮但假的精确值。

---

## 6. 性能（数字待 `scripts/bench.ps1` 定稿）

> 这一节只留标题：单机吞吐/延迟的权威数字由 `scripts/bench.ps1`（含 `conns × threads` 二维表）
> 产出，本项目里同一个数字只允许有一个出处，所以这里不复制。
>
> 引用性能时请一并写出四个前提：连接数（`--conns`）、线程数（`-T`）、实例数、是否同机 loopback。
> 少写任何一个，这个数字就不可复现。
>
> **同机压测：客户端与服务端共享同一颗 CPU。** 表中数值是**端到端联合吞吐**，不是服务端容量上限；
> 服务端容量的真实数字需要在两台机器上测。不写这句，四万会被读成"服务端只能跑四万"——
> 那是被我们自己的文档造成的误读。

---

## 附：演示脚本自身的设计约束（给维护者看）

- **`[client]` 那几行是机器可读契约**（`scripts/bench.ps1` 和本文的断言都按它解析，改字段前先通知）：
  ```
  [client] connections_per_endpoint=<N> source=<来源> endpoints=<M> total_conns=<K>
  [client] max_retries=<N> source=<来源>
  [client] retry_total=<N> failover_total=<N>
  ```
  两条 `source=` 行在启动时打（说明"这次跑的到底是什么配置"），`retry_total` / `failover_total`
  在退出前打（事后核账：重试了几次、换了几个实例）。它们都走 `printf` 到 stdout，
  **不受日志级别控制**——证据行不能因为 `--log-level` 被过滤掉。
  `source=` 的取值只有四种：`cli(--conns N)` / `conf(<路径>)` / `auto(bench threads=…)` /
  `auto(<mode>: single-threaded sequential)`；解析时非贪婪收到 ` endpoints=` 为止（路径里可能有空格和括号）。
- 两个脚本**必须纯 ASCII**。cmd.exe 和 PowerShell 5.1 用 OEM 代码页（本机 936）解析 `.ps1`，
  脚本里写中文轻则乱码、重则语法错误。子进程照样打印 UTF-8 中文，脚本在第 0 步把控制台切到
  65001，并且**从不用中文做匹配**——断言只读 `/metrics` 里的 ASCII 数字。
- **不用 `Start-Process`**。Windows 环境变量大小写不敏感、PowerShell 的 `env:` 不敏感，
  当 `HTTP_PROXY` 和 `http_proxy` 同时存在时 `Start-Process` 会在子进程启动前抛
  "已添加项。字典中的关键字: HTTP_PROXY"。改用 `[System.Diagnostics.Process]::Start`。
- **不用 `Invoke-WebRequest`** 抓指标。同样的环境变量问题 + IE 解析引擎依赖；改用裸 TCP
  `GET /metrics HTTP/1.0`，顺带证明端点确实是 `HTTP/1.0 + close`。
- **不用 `2>&1`**。原生命令的 stderr 在 PowerShell 里会变成 `ErrorRecord`，
  配上 `$ErrorActionPreference='Stop'` 会把健康运行变成终止错误。两个流分别捕获。
- 端口探测用 bind 而不是 connect：本机连一个关闭的回环端口要 ~2 秒才被拒绝（实测），
  6 个端口探一遍比整个闸门还慢。
