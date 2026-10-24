# 轻舟 RPC IDL 规范与代码生成契约（`.qz` v1）

> 本文定义 `.qz` 语法、类型映射，以及**生成代码必须长成什么样**。
> 最后一节是硬性自检表：生成代码用到的每一个运行时符号都必须真实存在于 `src/` 的公共头文件里。
>
> 作者：顾图南（架构） · 版本：v1

---

## 1. 设计目标与取舍

| 目标 | 做法 | 与 phxrpc 的对比 |
| --- | --- | --- |
| 零依赖解析 | 自己写 lexer + 递归下降 parser（Python，纯标准库） | phxrpc 用 `google::protobuf::compiler::Importer`，protobuf 一升级就可能编不过 |
| 语法足够小，30 分钟能学会 | 只有 6 种顶层声明，无 option 嵌套、无扩展、无 map、无 oneof | `.proto` 的 option/extend/group 是学习负担 |
| 生成物就是**唯一**的序列化真相 | 不生成 `.pb.h`，字段访问器、编解码、元数据全部由 codegen 写出 | phxrpc 依赖 protobuf 生成的 `.pb.h`，框架层被迫认识 `google::protobuf::Message` |
| 生成物可读、可 diff、可 debug | 不用模板引擎，直接字符串拼接，输出经 clang-format 风格对齐 | phxrpc 用 `$Var$` 占位符模板，生成结果难追踪 |
| 线上兼容 protobuf | 字段编码严格按 protobuf wire format（见 doc 03） | 同上 |

**明确不支持**（且必须在解析时报错而不是静默忽略）：`import`、`extend`、`oneof`、`map`、
`group`、`required`、`reserved`、`option` 嵌套、自定义 option、负数枚举值。

对这些语法，codegen 的行为是：**认出关键字 → 报 "`<feature>` is not supported by qz v1" → 退出码 1**。
静默忽略是最坏的选择——它会让用户以为配置生效了。

---

## 2. 词法

| token | 规则 |
| --- | --- |
| 空白 | 空格 / `\t` / `\r` / `\n`，仅用于分隔 |
| 行注释 | `//` 到行尾 |
| 块注释 | `/* ... */`，**不支持嵌套** |
| 标识符 | `[A-Za-z_][A-Za-z0-9_]*` |
| 限定标识符 | `ident ( "." ident )*`，用于 package 与跨消息引用 |
| 整数字面量 | 十进制 `[0-9]+`，十六进制 `0x[0-9a-fA-F]+` |
| 浮点字面量 | `[-+]?[0-9]+(\.[0-9]+)?([eE][-+]?[0-9]+)?` |
| 字符串字面量 | `"..."`，支持 `\\ \" \n \t \r \0` 转义，**不支持**跨行与 `\xNN` |
| 布尔字面量 | `true` / `false` |
| 标点 | `{ } ( ) [ ] = ; , .` |
| 关键字 | `syntax package import option enum message service rpc returns optional repeated default packed deprecated idempotent method_id timeout_ms` 以及全部标量类型名 |

### 2.1 关键字在哪些位置是禁止的

关键字不是一刀切的保留字。**判据是"该位置是否会产生语法歧义"**：

| 位置 | 允许关键字？ | 判据 |
| --- | :-: | --- |
| message 名 / enum 名 / service 名 / package 各段 | ❌ 禁止 | 紧跟在 `message` / `enum` / `service` / `package` 之后；若允许，`message message {` 这类声明会让 parser 无法区分"声明"与"类型引用" |
| **字段名** | ✅ 允许 | 字段名前面必有类型，后面必有 `=` 与 tag；`string message = 1;` 语法上无歧义 |
| **枚举值名** | ✅ 允许 | 值名后面必有 `=` 与整数字面量 |
| **rpc 名** | ✅ 允许 | rpc 名后面必有 `(` |

这条规则不是"松口"，而是**去掉一个不必要的限制**：`string message = 1;` 是最自然的写法
（protobuf 生态里大量存在），禁掉它只会逼开发者去写 `msg` / `message_` 这种次优命名。
`idl/echo.qz` 本身就用了 `string message = 1;`，`docs` 里 §9 的完整示例也一样。

声明名（message / enum / service / package）继续拒绝关键字，用到了直接报错，**不做自动改名**——
自动改名会让"IDL 里叫什么、C++ 里叫什么"变成需要查表的第二套规则。

> 注意：字段名允许关键字，**不等于**允许与 C++ 关键字冲突。`int32 class = 1;` 能过 parser
> 但生成 `class_` / `class()` 会编不过，见 §3.1 的 C-13。

---

## 3. 语法（EBNF）

```ebnf
(* ---------- 顶层 ---------- *)
file            = { top_level } EOF ;
top_level       = syntax_decl | package_decl | import_decl | option_decl
                | enum_decl | message_decl | service_decl ;

syntax_decl     = "syntax" "=" string_lit ";" ;                     (* 必须是文件第一条语句 *)
package_decl    = "package" qualified_ident ";" ;                   (* 至多一条 *)
import_decl     = "import" string_lit ";" ;                         (* v1 解析后报"不支持" *)
option_decl     = "option" qualified_ident "=" literal ";" ;        (* 仅允许 qzrpc.* 系列 *)

(* ---------- 枚举 ---------- *)
enum_decl       = "enum" ident "{" { enum_value } "}" ;
enum_value      = ident "=" int_lit ( ";" | "," ) ;                 (* 第一个必须为 0 *)

(* ---------- 消息 ---------- *)
message_decl    = "message" ident "{" { message_member } "}" ;
message_member  = field | message_decl | enum_decl ;
field           = [ label ] type_ref ident "=" int_lit [ attr_list ] ";" ;
label           = "optional" | "repeated" ;                         (* 无 label = 单数 *)
attr_list       = "[" attr { "," attr } "]" ;
attr            = "default" "=" literal
                | "packed" "=" bool_lit                             (* 仅 repeated 数值类型可写 *)
                | "deprecated" ;

type_ref        = scalar_type | qualified_ident ;
scalar_type     = "int32" | "int64" | "uint32" | "uint64"
                | "sint32" | "sint64"
                | "bool" | "float" | "double"
                | "string" | "bytes" ;

(* ---------- 服务 ---------- *)
service_decl    = "service" ident "{" { rpc_decl } "}" ;
rpc_decl        = "rpc" ident "(" type_ref ")" "returns" "(" type_ref ")"
                  [ rpc_attr_list ] ";" ;
rpc_attr_list   = "[" rpc_attr { "," rpc_attr } "]" ;
rpc_attr        = "idempotent" "=" bool_lit
                | "method_id"  "=" int_lit                          (* 覆盖自动计算值 *)
                | "timeout_ms" "=" int_lit ;                        (* 建议的服务端超时 *)

(* ---------- 词法 ---------- *)
qualified_ident = ident { "." ident } ;
ident           = IDENT ;
int_lit         = DECIMAL | HEX ;
literal         = int_lit | float_lit | string_lit | bool_lit ;
```

### 3.1 语义约束（parser 之后必须校验）

| # | 约束 | 违反时的报错 |
| --- | --- | --- |
| C-01 | `syntax` 必须是第一条语句，值必须是 `"qz1"` | `unsupported syntax version` |
| C-02 | 枚举第一个值必须为 `0` | `enum <E> must start at 0` |
| C-03 | 字段 tag 必须 `1 ≤ tag ≤ 536870911`，且**同一消息内唯一** | `duplicate field tag 3 in message X` |
| C-04 | tag 在 `19000..19999` 范围内禁止使用（protobuf 保留区） | `tag 19000-19999 are reserved` |
| C-05 | 类型引用必须能解析到同文件内的 message/enum | `unknown type "Foo"` |
| C-06 | 消息之间不得形成循环包含（A 含 B，B 含 A） | `circular message dependency: A -> B -> A` |
| C-07 | `repeated` 才能写 `packed`，且类型必须是数值型 | `packed is only allowed on repeated numeric fields` |
| C-08 | `default` 只能出现在非 `repeated` 字段上 | `repeated field cannot have default` |
| C-09 | `default` 的类型必须与字段类型匹配 | `default value type mismatch` |
| C-10 | `method_id` 显式值必须 `1 ≤ v ≤ 0xFFFFFFFF`，且**全局唯一** | `method_id 0x... already used by ...` |
| C-11 | 全部 `full_name` 计算出的 method_id 必须全局唯一 | `method_id collision: A and B both map to 0x...` |
| C-12 | 服务方法的请求/响应类型必须解析到 message（不能是标量/enum） | `rpc parameter must be a message type` |
| C-13 | 字段名 / 枚举值名 / rpc 名与 **C++ 关键字**冲突时必须报错（`class`、`new`、`delete`、`operator`、`template` 等） | `field name "class" is a C++ keyword` |

**关于 C-13（诚实说明）**：v1 **不实现** C-13，理由是"要做对"需要维护一份完整的
C++17 关键字表 + 每个位置（成员名 vs 自由函数名 vs 枚举值名）的合法性差异，
而收益只是把一个"编不过"提前成"生成时报错"。

但这**不等于装作它不存在**。v1 的兜底手段是 **§8 的 CI 编译门**：
每个 `.qz` 生成的 `.cpp` 都必须过一遍 `cl /c`，`class` 这种名字会被编译器挡住。

这条兜底是**实测验证过的**，不是推断。用一个 5 行的 IDL：

```qz
syntax = "qz1";
package t;
message M {
  int32 class = 1;      // 能过 lexer/parser/semantic，check 报 0 warning
}
```

`--check` 通过（0 warning），但生成后 `cl /c` 立刻报：

```
b.qz.h(54): error C2236: 意外的标记"class"。是否忘记了";"?
b.qz.h(54): error C2332: "class": 缺少标记名
b.qz.h(54): error C2059: 语法错误: ")"
```

所以这个缺口是"被编译门覆盖"，不是"被忽略"。等真有人在 IDL 里写了 `class`，
CI 会红在编译门上，届时再补 C-13 即可——**先有了兜底，才有资格说"暂不实现"**。

**关于 C-10 / C-11 的覆盖来源**：显式 `method_id` 通过 rpc 属性
`rpc Foo(Req) returns (Resp) [method_id = 0x...];` 指定（EBNF 见 §3 的 `rpc_attr`）。
它和自动计算的 id 一起进入同一个全局字典，因此 C-10（显式值范围与唯一性）与
C-11（`full_name` 计算值唯一性）必须**在同一个 pass 里检查**，否则
"显式值撞上别人的自动值"这种情况会漏掉。`codegen/tests/fixtures/kitchen_sink.qz` 里
`service Beta { rpc Shared(...) [method_id = 0x0000ABCD, idempotent = false]; }` 就是这条路径的回归用例。

> **`[method_id]` 是正式支持的逃生口**，语义已在契约层定案（`rpc::MethodInfo::method_id_overridden`，
> 见 `server/service_registry.h`）：
> - `false`（默认，哈希派生）→ L2 重算 `MethodIdOfName(full_name)` 必须相等，不等即 ERROR；
> - `true`（IDL 显式指定）→ L2 **跳过该比对**，只记一条 INFO，**不阻断 `Server::Start`**；
>   但仍参与"同 id 不同名"的冲突检测（这条永远不放宽）。
>
> 因此 codegen 的硬性责任是：生成物里的**三段**都带上这个标志 ——
> `MethodDecl` 结构体字段、`decls[]` 初值、`info.method_id_overridden = decl.method_id_overridden;`
> （见 §6.4 骨架）。少设它，合法覆盖会被当成 codegen bug；多设它，真实 bug 会被放过去。
> 完整判定规则与"为什么只豁免比对、不豁免冲突"见 doc 03 §7.5。

### 3.2 关于 `import`（诚实说明 + 演进路径）

v1 **不实现** `import`。理由：
- 需要 include 路径解析、循环 import 检测、跨文件类型可见性、生成物的跨文件引用 —— 这是一整套
  "模块系统"，工作量与整个 codegen 相当；
- 单文件 IDL 足以支撑「核心框架 + 可跑 Demo」的目标；
- 而"多文件"的实际收益只有在团队协作规模下才显现。

演进路径（v2）：
1. `-I` 参数提供搜索路径；`import "foo.qz";` 解析后把 `foo.qz` 的 AST 合并进同一编译单元；
2. 生成物按文件切分，跨文件引用通过 `#include "foo.qz.h"` 解决；
3. 循环 import 在 C-06 的检查里一并处理（展开成有向图做环检测）。

现在写 `import` 会得到明确报错，而不是奇怪的编译失败——这是刻意的。

---

## 4. 类型映射表

### 4.1 基本类型

| IDL 类型 | C++ 类型（单数/optional） | C++ 类型（repeated） | wire_type | 编码 |
| --- | --- | --- | ---: | --- |
| `int32` | `std::int32_t` | `std::vector<std::int32_t>` | 0 | varint（负数输出 10 字节） |
| `int64` | `std::int64_t` | `std::vector<std::int64_t>` | 0 | varint |
| `uint32` | `std::uint32_t` | `std::vector<std::uint32_t>` | 0 | varint |
| `uint64` | `std::uint64_t` | `std::vector<std::uint64_t>` | 0 | varint |
| `sint32` | `std::int32_t` | `std::vector<std::int32_t>` | 0 | zigzag varint |
| `sint64` | `std::int64_t` | `std::vector<std::int64_t>` | 0 | zigzag varint |
| `bool` | `bool` | `std::vector<bool>`（编码时按 `char` 遍历） | 0 | varint 0/1 |
| `float` | `float` | `std::vector<float>` | 5 | 小端 4 字节 |
| `double` | `double` | `std::vector<double>` | 1 | 小端 8 字节 |
| `string` | `std::string` | `std::vector<std::string>` | 2 | varint 长度 + UTF-8 字节 |
| `bytes` | `std::string` | `std::vector<std::string>` | 2 | varint 长度 + 原始字节 |
| `enum E` | `E`（`enum class : std::int32_t`） | `std::vector<E>` | 0 | varint（按底层整型） |
| `message M` | `M` | `std::vector<M>` | 2 | varint 长度 + 消息体 |

补充规则：
- **packed 默认开启**仅对 `repeated` 的数值类型（含 enum）生效；`string`/`bytes`/`message` 永远逐元素带 tag。
  `[packed = false]` 可显式关闭。
- `optional` 字段在 C++ 侧额外生成一个 `bool has_xxx_` 与 `has_xxx()`；非 optional 字段没有 presence 语义
  （等于默认值时不编码，这是 proto3 的行为）。
- `message` 类型字段**总是**有 presence（`has_body()` / `mutable_body()`），因为"空消息"与"不存在"必须能区分。
- `repeated` 字段在 C++ 侧额外生成 `_size() / _(i) / add_() / set_(i,v) / clear_()` 五个方法。

### 4.2 生成的字段访问器命名规则

| 字段声明 | 生成的访问器 |
| --- | --- |
| `string message = 1;` | `const std::string& message() const` / `void set_message(std::string)` / `std::string* mutable_message()` |
| `int32 repeat = 2;` | `std::int32_t repeat() const` / `void set_repeat(std::int32_t)` |
| `optional string lang = 5;` | 上述 + `bool has_lang() const` / `void clear_lang()` |
| `EchoBody body = 6;` | `const EchoBody& body() const` / `EchoBody* mutable_body()` / `bool has_body() const` / `void clear_body()` |
| `repeated int32 ids = 3;` | `std::size_t ids_size() const` / `std::int32_t ids(std::size_t) const` / `void add_ids(std::int32_t)` / `void set_ids(std::size_t, std::int32_t)` / `const std::vector<std::int32_t>& ids() const` / `std::vector<std::int32_t>* mutable_ids()` / `void clear_ids()` |

`mutable_xxx()` 语义：返回可写指针，并把该字段标记为"已设置"（对 message 字段置 `has_xxx_ = true`）。

---

## 5. codegen 用法与产物

### 5.1 包的位置与推荐调用方式

代码生成器是个 Python 包，**位于 `codegen/qzcodegen/`，不在仓库根**。因此 `python -m qzcodegen`
必须在 `PYTHONPATH` 里能找到 `codegen`。**推荐写法**（下面所有命令均已在仓库实测通过）：

```bash
# 推荐：从仓库根执行，用 PYTHONPATH 指向包目录
PYTHONPATH=codegen python -m qzcodegen --input idl/echo.qz --out generated --cpp

# 只做语法/语义检查，不落盘（CI 用）
PYTHONPATH=codegen python -m qzcodegen --input idl/echo.qz --check

# 一次校验多个文件，做全局 method_id 冲突检测（L1 层）
PYTHONPATH=codegen python -m qzcodegen --input idl/*.qz --check
```

另有两种等价写法，视场景选用：

```bash
# 写法 B：先 cd 进 codegen/，用 --source-root 指回仓库根，
#         这样文件头里的 "源文件:" 仍然是 idl/echo.qz 而不是 ../idl/echo.qz
cd codegen && python -m qzcodegen --input ../idl/echo.qz --source-root .. --check

# 写法 C：设置一次环境变量，后面所有命令都不用再写
export PYTHONPATH=codegen        # Windows PowerShell: $env:PYTHONPATH="codegen"
python -m qzcodegen --input idl/echo.qz --out generated --cpp
```

`--source-root` 决定文件头注释里 `源文件:` 的基准目录（默认当前目录）。
**不要**在 `cd codegen` 之后省略它，否则生成物里会出现 `../idl/echo.qz`，
既难看又会让"换个工作目录就 diff 全变"。

### 5.2 命令行参数

```
-i, --input FILE [FILE ...]   输入 .qz（可多个，支持 shell glob；多个文件一起做冲突检测）
-o, --out DIR                 生成物输出目录（默认 generated）
    --cpp                     生成 C++（.qz.h + .qz.cpp）
    --check                   只做语法/语义检查，不写任何文件（CI 用）
    --source-root DIR         源文件相对路径的基准目录（默认当前目录）
    --timestamp ISO8601       写入文件头的生成时间（默认不写，见 §6.1）
-q, --quiet                   只输出错误
    --version                 版本号
```

> **关于生成器内部的变量命名（防止后来者发明一层包装）**：
> CLI 层只处理"路径 + 选项"，它的产物是 AST 节点；AST 相关代码用
> `from . import ast_nodes as ast` 这个别名（读作 `ast.MessageDecl`、`ast.Field`）。
> **不存在 `Input.package` / `Input.service` 这类名字**——本文档也从未规定过。
> 如果后续要重构，请沿用 `ast.*`，不要为了"看起来结构化"再包一层 `Input` 对象：
> 那会让"IDL 里的名字"和"Python 里的名字"之间多一次映射，正是 §2.1 明确要避免的东西。

### 5.3 产物

| 产物 | 内容 |
| --- | --- |
| `generated/<base>.qz.h` | 枚举、消息类、Stub、ServiceBase、Dispatcher、注册工厂 |
| `generated/<base>.qz.cpp` | 上述全部实现 + 匿名 namespace 里的方法处理器 |

`<base>` = 输入文件名去掉 `.qz`。`idl/echo.qz` → `generated/echo.qz.h` / `generated/echo.qz.cpp`。

退出码：`0` 成功；`1` 有错误（诊断写到 stderr）。

### 5.4 ⚠️ 多文件必须一次性传入（否则 L1 冲突检测静默退化）

L1 层的 method_id 冲突检测**只在一个进程内、对本次传入的全部文件生效**——它没有全局知识。

```bash
# ✅ 正确：一次传入全部 IDL，跨文件冲突能被发现
PYTHONPATH=codegen python -m qzcodegen --input idl/*.qz --check

# ❌ 错误：逐个文件调用，跨文件冲突永远发现不了，C-10/C-11 退化成"单文件内唯一"
for f in idl/*.qz; do
  PYTHONPATH=codegen python -m qzcodegen --input "$f" --check   # 这一步不会报冲突
done
```

**构建脚本必须用通配形式**（`--input idl/*.qz` 或显式列出全部路径），不得写在 for 循环里。
这条约束没法在工具内部兜住（工具不知道还有别的文件存在），只能靠调用方遵守，
所以它是 **CI 检查项**而不是运行时保障：`scripts/ci_*.ps1` 里应当**只出现一处**
`--input <全部文件>` 的调用。

---

## 6. 生成物契约

### 6.1 文件头注释格式（含**可复现性**规则）

```cpp
// =============================================================================
// Generated by qzcodegen. DO NOT EDIT.
// 由 qzcodegen 1.0.0 生成，请勿手工修改。
// 源文件   : idl/echo.qz
// 生成时间 : <reproducible>（默认不写时间；可用 --timestamp 或 SOURCE_DATE_EPOCH）
// 协议版本 : 1
// 运行时   : qzrpc >= 0.1.0（见 docs/03-wire-protocol.md）
// =============================================================================
```

**生成时间默认不写真实时间**，而是写固定占位 `<reproducible>` 并自带说明。这不是偷懒，
而是**必须**的：生成器要对同一输入产出**逐字节相同**的结果，否则会有两个真实故障：

1. `test_golden.py` 会把每次生成都判为"不一致"，测试永远红；
2. code review 时 `generated/*.cpp` 的 diff 全是时间行噪音，真正的逻辑变更被淹没。

需要真实时间时，两条显式通路（都不影响默认行为）：

```bash
# 通路 1：命令行显式指定（写进文件头）
PYTHONPATH=codegen python -m qzcodegen --input idl/echo.qz --out generated --cpp \
    --timestamp 2024-05-01T12:00:00Z

# 通路 2：可复现构建标准的环境变量（打包/发布时由构建系统注入）
SOURCE_DATE_EPOCH=1714564800 PYTHONPATH=codegen python -m qzcodegen \
    --input idl/echo.qz --out generated --cpp
```

> `SOURCE_DATE_EPOCH` 是 [reproducible-builds.org](https://reproducible-builds.org/specs/source-date-epoch/)
> 约定的标准变量（Unix 秒）。这里沿用它而不是自造 `QZ_BUILD_TIME`，是为了让生成物能直接
> 接进已有的可复现构建流水线。

**三行元信息各自的作用**：
- `源文件` 让人能从产物反查 IDL；
- `生成时间` 在显式指定时用于发布溯源；默认占位则保证幂等；
- `协议版本` 让"生成物与运行时版本不匹配"这类问题一眼可见。

**`--check` 与时间戳无关**：`--check` 只做语法/语义校验，**不写任何文件**，
因此 CI 里跑 `--check` 既不产生时间戳问题，也不需要关心可复现性。

**不含**绝对路径、不含当前用户名、不含机器名——否则每次生成 diff 都会抖动。
这也是 §5.1 强调 `--source-root` 的原因：`源文件:` 必须是相对路径。

### 6.2 include guard

同时使用两者：

```cpp
#pragma once
#ifndef QZRPC_GENERATED_DEMO_ECHO_QZ_H_
#define QZRPC_GENERATED_DEMO_ECHO_QZ_H_
...
#endif  // QZRPC_GENERATED_DEMO_ECHO_QZ_H_
```

- `#pragma once` 是 MSVC/GCC/Clang 都支持的事实标准，且能正确处理同一物理文件的多路径引用；
- 宏 guard 是给"不支持 `#pragma once` 的老编译器 + 网络文件系统上 inode 判重失效"留的保险；
- 宏名规则：`QZRPC_GENERATED_` + `<package 全大写, '.'→'_'>` + `_` + `<文件名全大写, 非字母数字→'_'>` + `_H_`。
  用文件名而不是类型名，避免"一个文件里多个类"时 guard 名冲突。

### 6.3 `generated/echo.qz.h` 完整骨架

```cpp
// =============================================================================
// Generated by qzcodegen. DO NOT EDIT.
// 由 qzcodegen 1.0.0 生成，请勿手工修改。
// 源文件   : idl/echo.qz
// 生成时间 : <reproducible>（默认不写时间；可用 --timestamp 或 SOURCE_DATE_EPOCH）
// 协议版本 : 1
// 运行时   : qzrpc >= 0.1.0（见 docs/03-wire-protocol.md）
// =============================================================================

#pragma once
#ifndef QZRPC_GENERATED_DEMO_ECHO_QZ_H_
#define QZRPC_GENERATED_DEMO_ECHO_QZ_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>   // std::move（内联 setter 使用）
#include <vector>

#include "client/channel.h"          // qzrpc::rpc::Channel / CallOptions
#include "common/status.h"           // qzrpc::Status
#include "serialize/message.h"       // qzrpc::serde::Message / FieldMeta
#include "server/dispatcher.h"       // qzrpc::rpc::Dispatcher / ServerContext
#include "server/service_registry.h" // qzrpc::rpc::ServiceRegistry / MethodInfo

namespace demo {

// ---------------------------------------------------------------------------
// 枚举
// ---------------------------------------------------------------------------
enum class SiteType : std::int32_t {
  UNKNOWN = 0,
  BLOG = 1,
  NEWS = 2,
  VIDEO = 3,
};

// 枚举值名；未知值返回 "UNKNOWN"。
const char* SiteType_Name(SiteType value);

// 按名解析；失败返回 false 且不修改 *out。
bool SiteType_Parse(const std::string& name, SiteType* out);

// ---------------------------------------------------------------------------
// 消息 demo.EchoBody
// ---------------------------------------------------------------------------
class EchoBody final : public qzrpc::serde::Message {
 public:
  EchoBody();
  ~EchoBody() override;

  EchoBody(const EchoBody&) = default;
  EchoBody& operator=(const EchoBody&) = default;
  EchoBody(EchoBody&&) = default;
  EchoBody& operator=(EchoBody&&) = default;

  // 返回 "demo.EchoBody"。
  const char* TypeName() const override;
  // 全部字段恢复默认值，并清空 unknown fields。
  void Clear() override;
  // 按字段号升序编码；返回 false 表示编码失败。
  bool Encode(qzrpc::serde::Writer* writer) const override;
  // 从 reader 解析（范围恰好是消息体）；未知字段保留到 unknown_fields()。
  bool Decode(qzrpc::serde::Reader* reader) override;
  // 字段元数据表；*count 写入元素个数。
  const qzrpc::serde::FieldMeta* Fields(std::size_t* count) const override;

  // field 1: string text
  const std::string& text() const noexcept { return text_; }
  void set_text(std::string value) { text_ = std::move(value); }
  std::string* mutable_text() noexcept { return &text_; }

  // field 2: int32 weight
  std::int32_t weight() const noexcept { return weight_; }
  void set_weight(std::int32_t value) noexcept { weight_ = value; }

  // field 3: double score
  double score() const noexcept { return score_; }
  void set_score(double value) noexcept { score_ = value; }

  // field 4: bool valid
  bool valid() const noexcept { return valid_; }
  void set_valid(bool value) noexcept { valid_ = value; }

 private:
  std::string text_;
  std::int32_t weight_{0};
  double score_{0.0};
  bool valid_{false};
};

// ---------------------------------------------------------------------------
// 消息 demo.EchoRequest
// ---------------------------------------------------------------------------
class EchoRequest final : public qzrpc::serde::Message {
 public:
  EchoRequest();
  ~EchoRequest() override;

  EchoRequest(const EchoRequest&) = default;
  EchoRequest& operator=(const EchoRequest&) = default;
  EchoRequest(EchoRequest&&) = default;
  EchoRequest& operator=(EchoRequest&&) = default;

  const char* TypeName() const override;
  void Clear() override;
  bool Encode(qzrpc::serde::Writer* writer) const override;
  bool Decode(qzrpc::serde::Reader* reader) override;
  const qzrpc::serde::FieldMeta* Fields(std::size_t* count) const override;

  // field 1: string message
  const std::string& message() const noexcept { return message_; }
  void set_message(std::string value) { message_ = std::move(value); }
  std::string* mutable_message() noexcept { return &message_; }

  // field 2: int32 repeat （默认值 1，等于默认值时不编码）
  std::int32_t repeat() const noexcept { return repeat_; }
  void set_repeat(std::int32_t value) noexcept { repeat_ = value; }

  // field 3: repeated int32 ids （packed 编码）
  std::size_t ids_size() const noexcept { return ids_.size(); }
  std::int32_t ids(std::size_t index) const { return ids_[index]; }
  void set_ids(std::size_t index, std::int32_t value) { ids_[index] = value; }
  void add_ids(std::int32_t value) { ids_.push_back(value); }
  const std::vector<std::int32_t>& ids() const noexcept { return ids_; }
  std::vector<std::int32_t>* mutable_ids() noexcept { return &ids_; }
  void clear_ids() noexcept { ids_.clear(); }

  // field 4: repeated string tags （逐元素带 tag，不 packed）
  std::size_t tags_size() const noexcept { return tags_.size(); }
  const std::string& tags(std::size_t index) const { return tags_[index]; }
  void set_tags(std::size_t index, std::string value) { tags_[index] = std::move(value); }
  void add_tags(std::string value) { tags_.push_back(std::move(value)); }
  const std::vector<std::string>& tags() const noexcept { return tags_; }
  std::vector<std::string>* mutable_tags() noexcept { return &tags_; }
  void clear_tags() noexcept { tags_.clear(); }

  // field 5: optional string lang （有 presence 语义）
  const std::string& lang() const noexcept { return lang_; }
  void set_lang(std::string value) { lang_ = std::move(value); has_lang_ = true; }
  std::string* mutable_lang() noexcept { has_lang_ = true; return &lang_; }
  bool has_lang() const noexcept { return has_lang_; }
  void clear_lang() noexcept { lang_.clear(); has_lang_ = false; }

  // field 6: demo.EchoBody body （message 字段永远有 presence）
  const EchoBody& body() const noexcept { return body_; }
  EchoBody* mutable_body() noexcept { has_body_ = true; return &body_; }
  void set_body(const EchoBody& value) { body_ = value; has_body_ = true; }
  bool has_body() const noexcept { return has_body_; }
  void clear_body() noexcept { body_.Clear(); has_body_ = false; }

  // field 7: bytes raw
  const std::string& raw() const noexcept { return raw_; }
  void set_raw(std::string value) { raw_ = std::move(value); }
  std::string* mutable_raw() noexcept { return &raw_; }

  // field 8: demo.SiteType site
  SiteType site() const noexcept { return site_; }
  void set_site(SiteType value) noexcept { site_ = value; }

 private:
  std::string message_;
  std::int32_t repeat_{1};
  std::vector<std::int32_t> ids_;
  std::vector<std::string> tags_;
  std::string lang_;
  bool has_lang_{false};
  EchoBody body_;
  bool has_body_{false};
  std::string raw_;
  SiteType site_{SiteType::UNKNOWN};
};

// ---------------------------------------------------------------------------
// 消息 demo.EchoResponse
// ---------------------------------------------------------------------------
class EchoResponse final : public qzrpc::serde::Message {
 public:
  EchoResponse();
  ~EchoResponse() override;

  EchoResponse(const EchoResponse&) = default;
  EchoResponse& operator=(const EchoResponse&) = default;
  EchoResponse(EchoResponse&&) = default;
  EchoResponse& operator=(EchoResponse&&) = default;

  const char* TypeName() const override;
  void Clear() override;
  bool Encode(qzrpc::serde::Writer* writer) const override;
  bool Decode(qzrpc::serde::Reader* reader) override;
  const qzrpc::serde::FieldMeta* Fields(std::size_t* count) const override;

  // field 1: string message
  const std::string& message() const noexcept { return message_; }
  void set_message(std::string value) { message_ = std::move(value); }
  std::string* mutable_message() noexcept { return &message_; }

  // field 2: int32 served_by
  std::int32_t served_by() const noexcept { return served_by_; }
  void set_served_by(std::int32_t value) noexcept { served_by_ = value; }

 private:
  std::string message_;
  std::int32_t served_by_{0};
};

// ---------------------------------------------------------------------------
// 客户端 Stub：持有 Channel 的引用，不拥有它
// ---------------------------------------------------------------------------
class EchoServiceStub {
 public:
  // 服务全名 "demo.EchoService"，用于日志、metrics label 与注册中心查询。
  static constexpr const char* kServiceFullName = "demo.EchoService";

  // 方法 id（= MethodIdOfName(full_name)，见 doc 03 §7）。
  // 具体数值由 codegen 计算并写死为常量，保证与 Python 侧完全一致。
  static std::uint32_t MethodIdEcho() noexcept;    // 0x21678893
  static std::uint32_t MethodIdNotify() noexcept;  // 0x9AF18CAF

  // 构造。channel 必须在 Stub 生命周期内有效。
  explicit EchoServiceStub(qzrpc::rpc::Channel& channel);
  ~EchoServiceStub();

  EchoServiceStub(const EchoServiceStub&) = delete;
  EchoServiceStub& operator=(const EchoServiceStub&) = delete;

  qzrpc::rpc::Channel& channel() const noexcept { return channel_; }

  // rpc Echo(EchoRequest) returns (EchoResponse)  [idempotent = true]
  // 同步调用。成功时 response 被填充；失败时返回非 ok 的 Status。
  // 线程安全：是（依赖 Channel 的线程安全）。
  qzrpc::Status Echo(const EchoRequest& request, EchoResponse* response,
                     const qzrpc::rpc::CallOptions& options = {});

  // rpc Notify(EchoRequest) returns (EchoResponse)  [非幂等]
  qzrpc::Status Notify(const EchoRequest& request, EchoResponse* response,
                       const qzrpc::rpc::CallOptions& options = {});

 private:
  qzrpc::rpc::Channel& channel_;
};

// ---------------------------------------------------------------------------
// 服务端基类：业务实现者继承它，只实现纯虚方法
// ---------------------------------------------------------------------------
class EchoServiceBase {
 public:
  virtual ~EchoServiceBase();

  // rpc Echo(EchoRequest) returns (EchoResponse)
  // 返回 ok 表示成功；非 ok 时框架把状态码写进响应帧，且**不使用** response 的内容。
  virtual qzrpc::Status Echo(const EchoRequest& request, EchoResponse* response) = 0;

  // rpc Notify(EchoRequest) returns (EchoResponse)
  virtual qzrpc::Status Notify(const EchoRequest& request, EchoResponse* response) = 0;
};

// ---------------------------------------------------------------------------
// 服务端分发器：构造即完成注册（写入 Dispatcher 路由表 + 全局 ServiceRegistry）
// ---------------------------------------------------------------------------
class EchoServiceDispatcher {
 public:
  // 参数：service - 业务实现（必须比本对象活得久）；dispatcher - 服务端路由表。
  // 构造时逐个注册方法；任一失败则 ok() 为 false，error_message() 给出原因。
  EchoServiceDispatcher(EchoServiceBase* service, qzrpc::rpc::Dispatcher* dispatcher);
  ~EchoServiceDispatcher();

  EchoServiceDispatcher(const EchoServiceDispatcher&) = delete;
  EchoServiceDispatcher& operator=(const EchoServiceDispatcher&) = delete;

  // 全部方法是否注册成功。
  bool ok() const noexcept { return ok_; }

  // 注册失败原因（ok() 为 true 时为空串）。
  const std::string& error_message() const noexcept { return error_; }

  qzrpc::rpc::Dispatcher* dispatcher() const noexcept { return dispatcher_; }

 private:
  EchoServiceBase* service_{nullptr};
  qzrpc::rpc::Dispatcher* dispatcher_{nullptr};
  bool ok_{false};
  std::string error_;
};

// ---------------------------------------------------------------------------
// 工厂函数：一步完成"创建 dispatcher 并检查注册结果"
// ---------------------------------------------------------------------------
// 返回非空指针；注册失败时返回 nullptr 并把原因写入 *error。
std::unique_ptr<EchoServiceDispatcher> RegisterEchoService(
    EchoServiceBase* service, qzrpc::rpc::Dispatcher* dispatcher, std::string* error = nullptr);

}  // namespace demo

#endif  // QZRPC_GENERATED_DEMO_ECHO_QZ_H_
```

#### 6.3.1 硬性规定：每个消息类必须显式 default 出**四件套**

```cpp
  XxxMessage(const XxxMessage&) = default;
  XxxMessage& operator=(const XxxMessage&) = default;
  XxxMessage(XxxMessage&&) = default;
  XxxMessage& operator=(XxxMessage&&) = default;
```

**没有例外，也不允许"某个消息字段少就省两个"**。三个理由：

1. **一致性可被机械检查**。`test_golden.py` 用纯文本断言"每个 `class X final : public
   qzrpc::serde::Message` 之后 6 行内出现 4 个 `= default;`"，一省就红。
   如果允许按字段数决定写几个，这条断言就写不出来了。
2. **避免"隐式生成 vs 显式 default"的语义漂移**。一旦某个消息加了
   `std::vector` 成员，隐式 move 仍然是 `noexcept` 的；但如果将来加了自定义析构，
   隐式 move 会**静默消失**，性能退化且没有任何编译期提示。显式 default 会在那一刻报错。
3. **零成本**。`= default` 不产生代码。

> 反例教育：phxrpc 生成的类是"继承 protobuf 的 Message"，四件套由 protobuf 提供，
> 自己不需要管；我们自研序列化层后，这件事就落到 codegen 头上了。
> **把"以前免费的东西"显式写出来，是自研路线的必然成本。**

### 6.4 `generated/echo.qz.cpp` 完整骨架

> **注意 `.cpp` 的 include 列表比 `.h` 长**：`.h` 里只用到 `Writer*` / `Reader*`
> 这种指针（前向声明就够），但 `.cpp` 里要真正构造 `qzrpc::serde::Writer packed;`
> 和 `qzrpc::serde::Reader nested(data, len);`，**必须是完整类型**。
> 早期版本的文档骨架只写了 `echo.qz.h` + `<utility>`，照那份文档生成会因为
> "使用了未定义的类型"而编译失败——这是实测确认过的错误，已修正。
>
> 三行 `serialize/*.h` 不能省：`writer.h`（`Encode` 里 `Writer packed;`）、
> `reader.h`（`Decode` 里 `Reader nested(...)`、`packed` 子视图）、
> `serializer.h`（匿名 namespace 里的方法处理器调 `Serializer::ParseFromArray`）。
>
> **每个 `Encode` 末尾都有一段"未知字段回写"循环**，不要漏：
> 解析时未知字段被收进 `Message::unknown_fields()`（`raw` 不含 tag），
> 编码时必须用 `WriteTag(number, wire_type)` + `WriteRaw(raw)` 原样补回去。
> 这是 proto3 的 unknown field 语义，也是"轻舟 RPC 能当 protobuf 代理/网关"的前提：
> 一个只做转发的中间层面对新版本的上游消息时，**它看不懂的字段必须原样带过去**，
> 否则会静默把上游的新特性吃掉。少了这 5 行，"解码再编码后字节应完全不变"
> 这类 round-trip 校验（其落地形态见 §8.2）会立刻失败。

```cpp
// =============================================================================
// 由 qzcodegen 1.0.0 生成，请勿手工修改。
// 源文件   : idl/echo.qz
// 生成时间 : <reproducible>（默认不写时间；可用 --timestamp 或 SOURCE_DATE_EPOCH）
// 协议版本 : 1
// 运行时   : qzrpc >= 0.1.0（见 docs/03-wire-protocol.md）
// =============================================================================

#include "echo.qz.h"  // 由构建系统保证 generated/ 在 include 路径中

#include <utility>

#include "serialize/reader.h"      // qzrpc::serde::Reader（Decode 需要完整类型）
#include "serialize/serializer.h"  // qzrpc::serde::Serializer（处理器使用）
#include "serialize/writer.h"      // qzrpc::serde::Writer（Encode 需要完整类型）

namespace demo {

// ===========================================================================
// 枚举 SiteType
// ===========================================================================
const char* SiteType_Name(SiteType value) {
  switch (value) {
    case SiteType::UNKNOWN: return "UNKNOWN";
    case SiteType::BLOG:    return "BLOG";
    case SiteType::NEWS:    return "NEWS";
    case SiteType::VIDEO:   return "VIDEO";
  }
  return "UNKNOWN";
}

bool SiteType_Parse(const std::string& name, SiteType* out) {
  if (out == nullptr) return false;
  if (name == "UNKNOWN") { *out = SiteType::UNKNOWN; return true; }
  if (name == "BLOG")    { *out = SiteType::BLOG;    return true; }
  if (name == "NEWS")    { *out = SiteType::NEWS;    return true; }
  if (name == "VIDEO")   { *out = SiteType::VIDEO;   return true; }
  return false;
}

// ===========================================================================
// 消息 EchoBody
// ===========================================================================
EchoBody::EchoBody() = default;
EchoBody::~EchoBody() = default;

const char* EchoBody::TypeName() const { return "demo.EchoBody"; }

void EchoBody::Clear() {
  text_.clear();
  weight_ = 0;
  score_ = 0.0;
  valid_ = false;
  ClearUnknownFields();
}

bool EchoBody::Encode(qzrpc::serde::Writer* writer) const {
  if (writer == nullptr) return false;
  namespace wt = qzrpc::serde;  // 仅为可读性

  // field 1: string text
  if (!text_.empty()) {
    writer->WriteTag(1, static_cast<std::uint8_t>(wt::WireType::kLengthDelimited));
    writer->WriteString(text_);
  }
  // field 2: int32 weight
  if (weight_ != 0) {
    writer->WriteTag(2, static_cast<std::uint8_t>(wt::WireType::kVarint));
    writer->WriteInt32(weight_);
  }
  // field 3: double score
  if (score_ != 0.0) {
    writer->WriteTag(3, static_cast<std::uint8_t>(wt::WireType::kFixed64));
    writer->WriteDouble(score_);
  }
  // field 4: bool valid
  if (valid_) {
    writer->WriteTag(4, static_cast<std::uint8_t>(wt::WireType::kVarint));
    writer->WriteBool(valid_);
  }
  // 未知字段原样追加（proto3 语义：读进来再写出去不丢字段）。
  // raw 不含 tag，因此 tag 由 number + wire_type 重建。
  for (const qzrpc::serde::UnknownField& unknown : unknown_fields()) {
    writer->WriteTag(unknown.number, unknown.wire_type);
    writer->WriteRaw(unknown.raw.data(), unknown.raw.size());
  }
  return true;
}

bool EchoBody::Decode(qzrpc::serde::Reader* reader) {
  if (reader == nullptr) return false;
  Clear();
  while (!reader->AtEnd()) {
    std::uint32_t field = 0;
    std::uint8_t wire_type = 0;
    if (!reader->ReadTag(&field, &wire_type)) break;

    switch (field) {
      case 1:
        if (!reader->ReadStringField(wire_type, &text_)) return false;
        break;
      case 2:
        if (!reader->ReadInt32Field(wire_type, &weight_)) return false;
        break;
      case 3:
        if (!reader->ReadDoubleField(wire_type, &score_)) return false;
        break;
      case 4:
        if (!reader->ReadBoolField(wire_type, &valid_)) return false;
        break;
      default: {
        std::string raw;
        if (!reader->SkipFieldCapture(wire_type, &raw)) return false;
        qzrpc::serde::UnknownField unknown;
        unknown.number = field;
        unknown.wire_type = wire_type;
        unknown.raw = std::move(raw);
        AddUnknownField(std::move(unknown));
        break;
      }
    }
  }
  return !reader->failed();
}

const qzrpc::serde::FieldMeta* EchoBody::Fields(std::size_t* count) const {
  static const qzrpc::serde::FieldMeta kFields[] = {
      // number, name,     type,                          type_name, repeated, optional, has_default, default_value
      {1, "text",   qzrpc::serde::FieldType::kString,  nullptr, false, false, false, nullptr},
      {2, "weight", qzrpc::serde::FieldType::kInt32,   nullptr, false, false, false, nullptr},
      {3, "score",  qzrpc::serde::FieldType::kDouble,  nullptr, false, false, false, nullptr},
      {4, "valid",  qzrpc::serde::FieldType::kBool,    nullptr, false, false, false, nullptr},
  };
  if (count != nullptr) *count = sizeof(kFields) / sizeof(kFields[0]);
  return kFields;
}

// ===========================================================================
// 消息 EchoRequest
// ===========================================================================
EchoRequest::EchoRequest() = default;
EchoRequest::~EchoRequest() = default;

const char* EchoRequest::TypeName() const { return "demo.EchoRequest"; }

void EchoRequest::Clear() {
  message_.clear();
  repeat_ = 1;
  ids_.clear();
  tags_.clear();
  lang_.clear();
  has_lang_ = false;
  body_.Clear();
  has_body_ = false;
  raw_.clear();
  site_ = SiteType::UNKNOWN;
  ClearUnknownFields();
}

bool EchoRequest::Encode(qzrpc::serde::Writer* writer) const {
  if (writer == nullptr) return false;
  namespace wt = qzrpc::serde;

  // field 1: string message
  if (!message_.empty()) {
    writer->WriteTag(1, static_cast<std::uint8_t>(wt::WireType::kLengthDelimited));
    writer->WriteString(message_);
  }
  // field 2: int32 repeat（默认值 1，等于默认值时不编码）
  if (repeat_ != 1) {
    writer->WriteTag(2, static_cast<std::uint8_t>(wt::WireType::kVarint));
    writer->WriteInt32(repeat_);
  }
  // field 3: repeated int32 ids（packed：一个 tag + 一段 varint 块）
  if (!ids_.empty()) {
    qzrpc::serde::Writer packed;
    for (std::int32_t value : ids_) packed.WriteInt32(value);
    writer->WriteTag(3, static_cast<std::uint8_t>(wt::WireType::kLengthDelimited));
    writer->WriteLengthDelimited(packed.data().data(), packed.size());
  }
  // field 4: repeated string tags（逐元素带 tag）
  for (const std::string& value : tags_) {
    writer->WriteTag(4, static_cast<std::uint8_t>(wt::WireType::kLengthDelimited));
    writer->WriteString(value);
  }
  // field 5: optional string lang
  if (has_lang_) {
    writer->WriteTag(5, static_cast<std::uint8_t>(wt::WireType::kLengthDelimited));
    writer->WriteString(lang_);
  }
  // field 6: demo.EchoBody body
  if (has_body_) {
    qzrpc::serde::Writer nested;
    if (!body_.Encode(&nested)) return false;
    writer->WriteTag(6, static_cast<std::uint8_t>(wt::WireType::kLengthDelimited));
    writer->WriteLengthDelimited(nested.data().data(), nested.size());
  }
  // field 7: bytes raw
  if (!raw_.empty()) {
    writer->WriteTag(7, static_cast<std::uint8_t>(wt::WireType::kLengthDelimited));
    writer->WriteBytes(raw_.data(), raw_.size());
  }
  // field 8: demo.SiteType site（枚举 0 不编码）
  if (site_ != SiteType::UNKNOWN) {
    writer->WriteTag(8, static_cast<std::uint8_t>(wt::WireType::kVarint));
    writer->WriteEnum(static_cast<std::int32_t>(site_));
  }
  // 未知字段原样追加（proto3 语义：读进来再写出去不丢字段）。
  // raw 不含 tag，因此 tag 由 number + wire_type 重建。
  for (const qzrpc::serde::UnknownField& unknown : unknown_fields()) {
    writer->WriteTag(unknown.number, unknown.wire_type);
    writer->WriteRaw(unknown.raw.data(), unknown.raw.size());
  }
  return true;
}

bool EchoRequest::Decode(qzrpc::serde::Reader* reader) {
  if (reader == nullptr) return false;
  Clear();
  while (!reader->AtEnd()) {
    std::uint32_t field = 0;
    std::uint8_t wire_type = 0;
    if (!reader->ReadTag(&field, &wire_type)) break;

    switch (field) {
      case 1:
        if (!reader->ReadStringField(wire_type, &message_)) return false;
        break;
      case 2:
        if (!reader->ReadInt32Field(wire_type, &repeat_)) return false;
        break;
      case 3: {
        // packed（wire_type==2 的块）与 unpacked（逐个 wire_type==0）都接受
        if (wire_type == static_cast<std::uint8_t>(qzrpc::serde::WireType::kLengthDelimited)) {
          const char* data = nullptr;
          std::size_t len = 0;
          if (!reader->ReadLengthDelimited(&data, &len)) return false;
          qzrpc::serde::Reader packed(data, len);
          while (!packed.AtEnd()) {
            std::int32_t value = 0;
            if (!packed.ReadInt32(&value)) return false;
            ids_.push_back(value);
          }
        } else {
          std::int32_t value = 0;
          if (!reader->ReadInt32Field(wire_type, &value)) return false;
          ids_.push_back(value);
        }
        break;
      }
      case 4: {
        std::string value;
        if (!reader->ReadStringField(wire_type, &value)) return false;
        tags_.push_back(std::move(value));
        break;
      }
      case 5:
        if (!reader->ReadStringField(wire_type, &lang_)) return false;
        has_lang_ = true;
        break;
      case 6: {
        const char* data = nullptr;
        std::size_t len = 0;
        if (!reader->ReadLengthDelimitedField(wire_type, &data, &len)) return false;
        qzrpc::serde::Reader nested(data, len);
        if (!body_.Decode(&nested)) return false;
        has_body_ = true;
        break;
      }
      case 7: {
        std::string value;
        if (!reader->ReadStringField(wire_type, &value)) return false;
        raw_ = std::move(value);
        break;
      }
      case 8: {
        std::int32_t enum_value = 0;
        if (!reader->ReadInt32Field(wire_type, &enum_value)) return false;
        site_ = static_cast<SiteType>(enum_value);
        break;
      }
      default: {
        std::string raw;
        if (!reader->SkipFieldCapture(wire_type, &raw)) return false;
        qzrpc::serde::UnknownField unknown;
        unknown.number = field;
        unknown.wire_type = wire_type;
        unknown.raw = std::move(raw);
        AddUnknownField(std::move(unknown));
        break;
      }
    }
  }
  return !reader->failed();
}

const qzrpc::serde::FieldMeta* EchoRequest::Fields(std::size_t* count) const {
  static const qzrpc::serde::FieldMeta kFields[] = {
      {1, "message", qzrpc::serde::FieldType::kString,  nullptr, false, false, false, nullptr},
      {2, "repeat",  qzrpc::serde::FieldType::kInt32,   nullptr, false, false, true,  "1"},
      {3, "ids",     qzrpc::serde::FieldType::kInt32,   nullptr, true,  false, false, nullptr},
      {4, "tags",    qzrpc::serde::FieldType::kString,  nullptr, true,  false, false, nullptr},
      {5, "lang",    qzrpc::serde::FieldType::kString,  nullptr, false, true,  false, nullptr},
      {6, "body",    qzrpc::serde::FieldType::kMessage, "demo.EchoBody", false, false, false, nullptr},
      {7, "raw",     qzrpc::serde::FieldType::kBytes,   nullptr, false, false, false, nullptr},
      {8, "site",    qzrpc::serde::FieldType::kEnum,    "demo.SiteType", false, false, false, nullptr},
  };
  if (count != nullptr) *count = sizeof(kFields) / sizeof(kFields[0]);
  return kFields;
}

// ===========================================================================
// 消息 EchoResponse
// 下面只列一个方法，其余 6 个成员函数定义（ctor/dtor/TypeName/Clear/Encode/Decode/Fields）
// 与 EchoBody 同构，为节省篇幅在**本文档**里省略；生成器必须逐个消息完整输出，
// 不允许"结构简单的消息就少写几个函数"——.h 侧的四件套同理，见 §6.3.1。
// ===========================================================================
EchoResponse::EchoResponse() = default;
EchoResponse::~EchoResponse() = default;

const char* EchoResponse::TypeName() const { return "demo.EchoResponse"; }

void EchoResponse::Clear() {
  message_.clear();
  served_by_ = 0;
  ClearUnknownFields();
}

bool EchoResponse::Encode(qzrpc::serde::Writer* writer) const {
  if (writer == nullptr) return false;
  namespace wt = qzrpc::serde;
  if (!message_.empty()) {
    writer->WriteTag(1, static_cast<std::uint8_t>(wt::WireType::kLengthDelimited));
    writer->WriteString(message_);
  }
  if (served_by_ != 0) {
    writer->WriteTag(2, static_cast<std::uint8_t>(wt::WireType::kVarint));
    writer->WriteInt32(served_by_);
  }
  // 未知字段原样追加（proto3 语义：读进来再写出去不丢字段）。
  // raw 不含 tag，因此 tag 由 number + wire_type 重建。
  for (const qzrpc::serde::UnknownField& unknown : unknown_fields()) {
    writer->WriteTag(unknown.number, unknown.wire_type);
    writer->WriteRaw(unknown.raw.data(), unknown.raw.size());
  }
  return true;
}

bool EchoResponse::Decode(qzrpc::serde::Reader* reader) {
  if (reader == nullptr) return false;
  Clear();
  while (!reader->AtEnd()) {
    std::uint32_t field = 0;
    std::uint8_t wire_type = 0;
    if (!reader->ReadTag(&field, &wire_type)) break;
    switch (field) {
      case 1:
        if (!reader->ReadStringField(wire_type, &message_)) return false;
        break;
      case 2:
        if (!reader->ReadInt32Field(wire_type, &served_by_)) return false;
        break;
      default: {
        std::string raw;
        if (!reader->SkipFieldCapture(wire_type, &raw)) return false;
        qzrpc::serde::UnknownField unknown;
        unknown.number = field;
        unknown.wire_type = wire_type;
        unknown.raw = std::move(raw);
        AddUnknownField(std::move(unknown));
        break;
      }
    }
  }
  return !reader->failed();
}

const qzrpc::serde::FieldMeta* EchoResponse::Fields(std::size_t* count) const {
  static const qzrpc::serde::FieldMeta kFields[] = {
      {1, "message",   qzrpc::serde::FieldType::kString, nullptr, false, false, false, nullptr},
      {2, "served_by", qzrpc::serde::FieldType::kInt32,  nullptr, false, false, false, nullptr},
  };
  if (count != nullptr) *count = sizeof(kFields) / sizeof(kFields[0]);
  return kFields;
}

// ===========================================================================
// 方法处理器（匿名 namespace：不被外部链接看到，永不与其他翻译单元冲突）
// ===========================================================================
namespace {

qzrpc::Status HandleEcho(demo::EchoServiceBase* service, const char* payload,
                         std::size_t payload_len, std::string* out,
                         qzrpc::rpc::ServerContext* ctx) {
  (void)ctx;
  demo::EchoRequest request;
  if (!qzrpc::serde::Serializer::ParseFromArray(payload, payload_len, &request)) {
    return qzrpc::Status::SerializeError("demo.EchoService.Echo: decode request failed");
  }
  demo::EchoResponse response;
  qzrpc::Status status = service->Echo(request, &response);
  if (!status.ok()) return status;
  if (!qzrpc::serde::Serializer::SerializeToString(response, out)) {
    return qzrpc::Status::SerializeError("demo.EchoService.Echo: encode response failed");
  }
  return qzrpc::Status::Ok();
}

qzrpc::Status HandleNotify(demo::EchoServiceBase* service, const char* payload,
                           std::size_t payload_len, std::string* out,
                           qzrpc::rpc::ServerContext* ctx) {
  (void)ctx;
  demo::EchoRequest request;
  if (!qzrpc::serde::Serializer::ParseFromArray(payload, payload_len, &request)) {
    return qzrpc::Status::SerializeError("demo.EchoService.Notify: decode request failed");
  }
  demo::EchoResponse response;
  qzrpc::Status status = service->Notify(request, &response);
  if (!status.ok()) return status;
  if (!qzrpc::serde::Serializer::SerializeToString(response, out)) {
    return qzrpc::Status::SerializeError("demo.EchoService.Notify: encode response failed");
  }
  return qzrpc::Status::Ok();
}

}  // namespace

// ===========================================================================
// 客户端 Stub
// ===========================================================================
std::uint32_t EchoServiceStub::MethodIdEcho() noexcept {
  return 0x21678893u;  // FNV-1a64("demo.EchoService.Echo") & 0xFFFFFFFF
}

std::uint32_t EchoServiceStub::MethodIdNotify() noexcept {
  return 0x9AF18CAFu;  // FNV-1a64("demo.EchoService.Notify") & 0xFFFFFFFF
}

EchoServiceStub::EchoServiceStub(qzrpc::rpc::Channel& channel) : channel_(channel) {}

EchoServiceStub::~EchoServiceStub() = default;

qzrpc::Status EchoServiceStub::Echo(const EchoRequest& request, EchoResponse* response,
                                    const qzrpc::rpc::CallOptions& options) {
  qzrpc::rpc::CallOptions opts = options;
  if (!opts.idempotent) opts.idempotent = true;  // IDL 声明了 [idempotent = true]
  return channel_.Call(MethodIdEcho(), request, response, opts);
}

qzrpc::Status EchoServiceStub::Notify(const EchoRequest& request, EchoResponse* response,
                                      const qzrpc::rpc::CallOptions& options) {
  // 非幂等方法：不擅自打开重试开关（尊重调用方在 options 里显式设置的值）
  return channel_.Call(MethodIdNotify(), request, response, options);
}

// ===========================================================================
// 服务端基类
// ===========================================================================
EchoServiceBase::~EchoServiceBase() = default;

// ===========================================================================
// 服务端分发器
// ===========================================================================
EchoServiceDispatcher::EchoServiceDispatcher(EchoServiceBase* service,
                                             qzrpc::rpc::Dispatcher* dispatcher)
    : service_(service), dispatcher_(dispatcher) {
  if (service_ == nullptr || dispatcher_ == nullptr) {
    error_ = "EchoServiceDispatcher: null service or dispatcher";
    return;
  }

  struct MethodDecl {
    std::uint32_t method_id;
    const char* full_name;
    const char* method_name;
    const char* request_type;
    const char* response_type;
    bool idempotent;
    bool method_id_overridden;  // IDL 里是否写了 [method_id = 0x...]
    qzrpc::rpc::MethodHandler handler;
  };

  const MethodDecl decls[] = {
      {EchoServiceStub::MethodIdEcho(), "demo.EchoService.Echo", "Echo",
       "demo.EchoRequest", "demo.EchoResponse", true,
       false,  // method_id_overridden：echo.qz 没写 [method_id]，走哈希派生
       [this](const char* p, std::size_t n, std::string* o, qzrpc::rpc::ServerContext* c) {
         return HandleEcho(service_, p, n, o, c);
       }},
      {EchoServiceStub::MethodIdNotify(), "demo.EchoService.Notify", "Notify",
       "demo.EchoRequest", "demo.EchoResponse", false,
       false,  // method_id_overridden
       [this](const char* p, std::size_t n, std::string* o, qzrpc::rpc::ServerContext* c) {
         return HandleNotify(service_, p, n, o, c);
       }},
  };

  for (const MethodDecl& decl : decls) {
    if (!dispatcher_->Register(decl.method_id, decl.full_name, decl.handler, &error_)) {
      return;  // 路由表注册失败（通常是 method_id 冲突）
    }
    qzrpc::rpc::MethodInfo info;
    info.method_id = decl.method_id;
    info.full_name = decl.full_name;
    info.package_name = "demo";
    info.service_name = "EchoService";
    info.method_name = decl.method_name;
    info.request_type = decl.request_type;
    info.response_type = decl.response_type;
    info.idempotent = decl.idempotent;
    info.method_id_overridden = decl.method_id_overridden;
    if (!qzrpc::rpc::ServiceRegistry::Instance().RegisterMethod(info, &error_)) {
      return;  // 元信息登记失败（通常是与已有方法 id 冲突，L2 检测）
    }
  }
  ok_ = true;
}

EchoServiceDispatcher::~EchoServiceDispatcher() = default;

std::unique_ptr<EchoServiceDispatcher> RegisterEchoService(EchoServiceBase* service,
                                                           qzrpc::rpc::Dispatcher* dispatcher,
                                                           std::string* error) {
  std::unique_ptr<EchoServiceDispatcher> guard(
      new EchoServiceDispatcher(service, dispatcher));
  if (!guard->ok()) {
    if (error != nullptr) *error = guard->error_message();
    return nullptr;
  }
  return guard;
}

}  // namespace demo
```

> `.h` 里的 `EchoServiceStub::Echo` 声明用了 `const qzrpc::rpc::CallOptions& options = {}`，
> `.cpp` 里**不得重复写默认值**（C++ 规定默认实参只能出现在一处）。

### 6.5 符号冲突避免（六条规则）

| # | 规则 | 为什么 |
| --- | --- | --- |
| S-01 | 所有符号包在 `namespace <package>` 里；`a.b` 包 → `namespace a { namespace b { ... } }`（或 C++17 的 `namespace a::b`） | 包名隔离是最省心的命名空间方案 |
| S-02 | 方法处理器一律放 `.cpp` 里的**匿名 namespace** | 内部链接，永不跨 TU 冲突；也不需要给它们编名字 |
| S-03 | include guard 宏名用"包名 + 文件名"而不是类型名 | 一个文件含多个类型时不会撞 |
| S-04 | 生成物**不定义任何全局变量**（特别是**不用** `static ClientConfig global_xxx_config_`） | phxrpc 的全局静态配置是反面教材：多实例不可能、线程安全靠约定 |
| S-05 | 枚举名、消息名原样使用 IDL 里的标识符，不做大小写变换或加前缀 | 变换会让"IDL 里叫什么、C++ 里叫什么"变成需要查表的第二套规则 |
| S-06 | 生成物只 `#include` 本框架公共头 + 生成头，绝不 include 其他生成头（除非 IDL 声明了 import） | 避免生成物之间形成包含环 |

### 6.6 生成代码 ↔ 运行时 API 对照表（**必须逐条自检**）

生成代码用到的每一个运行时符号，都必须在下表指定的公共头文件里有声明。
如果某一行找不到对应符号，说明**契约破了**，必须先补头文件再生成代码。

| 生成代码中的用法 | 运行时符号 | 声明位置 | 状态 |
| --- | --- | --- | --- |
| `class X : public qzrpc::serde::Message` | `serde::Message` | `serialize/message.h` | ✅ |
| `virtual const char* TypeName() const` | `Message::TypeName` | `serialize/message.h` | ✅ |
| `virtual void Clear()` | `Message::Clear` | `serialize/message.h` | ✅ |
| `virtual bool Encode(qzrpc::serde::Writer*) const` | `Message::Encode` | `serialize/message.h` | ✅ |
| `virtual bool Decode(qzrpc::serde::Reader*)` | `Message::Decode` | `serialize/message.h` | ✅ |
| `virtual const FieldMeta* Fields(std::size_t*) const` | `Message::Fields` | `serialize/message.h` | ✅ |
| `ClearUnknownFields()` | `Message::ClearUnknownFields` | `serialize/message.h` | ✅ |
| `AddUnknownField(std::move(u))` | `Message::AddUnknownField` | `serialize/message.h` | ✅ |
| `for (const UnknownField& u : unknown_fields())` | `Message::unknown_fields` | `serialize/message.h` | ✅ |
| `qzrpc::serde::UnknownField{number, wire_type, raw}` | `serde::UnknownField` | `serialize/message.h` | ✅ |
| `qzrpc::serde::FieldMeta{...}` 聚合初始化 | `serde::FieldMeta` | `serialize/field_meta.h` | ✅ |
| `FieldType::kString/kInt32/kDouble/kBool/kMessage/kEnum/kBytes` | `serde::FieldType` | `serialize/field_meta.h` | ✅ |
| `WireType::kLengthDelimited/kVarint/kFixed64/kFixed32` | `serde::WireType` | `serialize/field_meta.h` | ✅ |
| `writer->WriteTag(n, wt)` | `Writer::WriteTag` | `serialize/writer.h` | ✅ |
| `writer->WriteString(s)` / `WriteBytes` | `Writer::WriteString` / `WriteBytes` | `serialize/writer.h` | ✅ |
| `writer->WriteInt32` / `WriteEnum` / `WriteBool` / `WriteDouble` | 同名方法 | `serialize/writer.h` | ✅ |
| `writer->WriteLengthDelimited(p, n)` | `Writer::WriteLengthDelimited` | `serialize/writer.h` | ✅ |
| `writer->WriteRaw(p, n)`（未知字段回写） | `Writer::WriteRaw` | `serialize/writer.h` | ✅ |
| `packed.data()` / `packed.size()` | `Writer::data` / `Writer::size` | `serialize/writer.h` | ✅ |
| `reader->AtEnd()` / `ReadTag(&f,&wt)` | `Reader::AtEnd` / `ReadTag` | `serialize/reader.h` | ✅ |
| `reader->ReadStringField(wt, &out)`（string **与 bytes 共用**） | `Reader::ReadStringField` | `serialize/reader.h` | ✅ |
| `reader->ReadInt32Field` / `ReadBoolField` / `ReadDoubleField` | 同名方法 | `serialize/reader.h` | ✅ |
| `reader->ReadLengthDelimited` / `ReadLengthDelimitedField` | 同名方法 | `serialize/reader.h` | ✅ |
| `reader->ReadInt32(&v)`（packed 内部） | `Reader::ReadInt32` | `serialize/reader.h` | ✅ |
| `reader->SkipFieldCapture(wt, &raw)` / `SkipField(wt)` | 同名方法 | `serialize/reader.h` | ✅ |
| `reader->failed()` | `Reader::failed` | `serialize/reader.h` | ✅ |
| `qzrpc::serde::Reader nested(d, n)` | `Reader(const void*, std::size_t)` | `serialize/reader.h` | ✅ |
| `Serializer::ParseFromArray` / `SerializeToString` | 同名静态方法 | `serialize/serializer.h` | ✅ |
| `qzrpc::Status::SerializeError(msg)` / `Status::Ok()` | 同名工厂 | `common/status.h` | ✅ |
| `Status::ok()` | `Status::ok` | `common/status.h` | ✅ |
| `channel_.Call(id, req, resp, opts)` | `rpc::Channel::Call` | `client/channel.h` | ✅ |
| `qzrpc::rpc::CallOptions` 与其 `idempotent` 字段 | `rpc::CallOptions` | `client/channel.h` | ✅ |
| `dispatcher_->Register(id, name, handler, &err)` | `rpc::Dispatcher::Register` | `server/dispatcher.h` | ✅ |
| `qzrpc::rpc::MethodHandler` 签名 | `rpc::MethodHandler` | `server/dispatcher.h` | ✅ |
| `qzrpc::rpc::ServerContext*` | `rpc::ServerContext` | `server/dispatcher.h` | ✅ |
| `ServiceRegistry::Instance().RegisterMethod(info, &err)` | `ServiceRegistry::RegisterMethod` | `server/service_registry.h` | ✅ |
| `qzrpc::rpc::MethodInfo{...}` 逐字段赋值 | `rpc::MethodInfo` | `server/service_registry.h` | ✅ |
| `info.method_id_overridden = decl.method_id_overridden;` | `MethodInfo::method_id_overridden` | `server/service_registry.h` | ✅ |
| 方法 id 常量 `0x21678893u` | 由 codegen 计算，与 `protocol::MethodIdOfName` 一致 | `protocol/protocol_const.h` | ✅ |

**自检结果**：上表 39 项均在 `src/` 的公共头文件中存在声明，且 `src/` 下 37 个公共头文件与 1 个平台头
已在本机 MSVC 14.51（`/std:c++17 /W4 /WX /permissive-`）下编译通过（零警告）。

**为什么 `bytes` 没有专属的 `ReadBytesField`**：protobuf 的 `bytes` 与 `string` 在线格式上
**完全同构**（都是 wire_type=2 的 length-delimited 字节块），差别只在"语义上是不是 UTF-8 文本"。
所以 `Reader` 只提供了一个 `ReadStringField`，`bytes` 字段也用它读进 `std::string`。
另设一个 `ReadBytesField` 只会是同一份代码的第二个名字，是纯粹的 API 表面积浪费。
（生成代码里 `raw_` 字段就是这么读的，见 §6.4 的 `case 7`。）

**表里的 ✅ 是真跑过的**：本表不是"设计意图"，而是"生成物 `generated/echo.qz.{h,cpp}` 在
MSVC 14.51 `/W4` 下零警告编译通过"这件事的反向清单——每一条都对应生成物里的一处真实调用。
如果哪天有人改了 `src/` 的某个签名而没改这里，这张表就失去意义了，所以它必须随契约一起维护。

---

### 6.7 三条隐含契约（不写在签名里，但违反就是 bug）

#### 6.7.1 `UnknownField::raw` **包含**长度前缀

`Reader::SkipFieldCapture(wire_type, &raw)` 抓取的是 **tag 之后到字段结束的全部字节**。
对 `wire_type = 2`（string / bytes / 嵌套 message / packed），这段字节**包含**那个长度 varint。

实现如下，`start` 取在 tag 已被 `ReadTag` 消费之后：

```cpp
bool Reader::SkipFieldCapture(std::uint8_t wire_type, std::string* raw) {
  const std::size_t start = pos_;
  if (!SkipField(wire_type)) return false;      // SkipField 会吃掉长度前缀 + 内容
  raw->assign(begin_ + start, pos_ - start);
  return true;
}
```

**为什么必须含前缀**：回写只需要两个字

```cpp
writer->WriteTag(u.number, u.wire_type);
writer->WriteRaw(u.raw.data(), u.raw.size());   // 原封不动
```

不需要按 wire_type 分支，也不会因为"重算长度"而改变字节。后者才是关键：
protobuf 的长度前缀**允许非最小编码**（`0x80 0x01` 与 `0x01` 都能表示 1）。
如果 `raw` 只存内容、回写时重算长度，那么一份用非最小编码的历史数据经我们转一手之后
字节就变了——"解码再编码字节完全不变"的 round-trip 保证立刻破掉，
而轻舟 RPC 想当 protobuf 网关/转发层，靠的正是这个保证。

#### 6.7.2 `ByteSize()` 与 `Encode()` 的一致性：**这是实现方式，不是调用方契约**

`ByteSize()` 的实现就是"用计数 Writer 跑一遍 `Encode()`"：

```cpp
std::size_t Message::ByteSize() const {
  Writer writer = Writer::Counting();     // 只累加 size()，不存字节
  if (!Encode(&writer)) return 0;
  return writer.size();
}
```

所以**它与 `Encode()` 的实际字节数天然一致**——这一点由实现方式保证，
**不是调用方必须遵守的契约**。依赖关系上也印证了：`message.h` 只用到 `Writer`，
`protocol/codec.h` 里的 `Frame` 也不包含 `message.h`，两者没有互相引用。

**目前 `protocol/codec.cpp` 并不依赖 `ByteSize()`**：帧体长度是在 payload 写完之后按实际长度
回填的（而且 codec 明确"绝不按 `body_len` 预分配内存"）。因此 `ByteSize()` 属于
**可选优化手段**——例如上层想一次性预留输出缓冲时可以用它，但它**不是正确性前提**。
这条要写清，否则后来者容易把它当成"必须遵守的暗契约"，甚至为它加一堆无用的断言。

**真正需要写进生成物契约的是另一条：生成器的实现约束。**

> 生成代码里 `ByteSize` 与 `Encode` 必须由**同一份字段元数据驱动**——
> 哪些字段该写、哪些因为"等于默认值"而不写，必须是同一个判断依据。

理由：某天真的有人用 `ByteSize()` 预分配缓冲，如果 `Encode` 判 `!= 0` 而估算逻辑判 `> 0`
（负数分叉）、或 `optional` 字段的 presence 判断两处不一致，就会出现**预留不足**（写到越界）
或**大幅浪费**。而且现象是"偶发 buffer 不够"或"偶发多分配几百字节"，极难定位。

具体到生成器：**不要**再生成一个独立的 `static size_t FieldSize(const XxxRequest&)`。
字段"写不写"的规则只表达一次（在每字段的 `Encode` 分支里），`ByteSize` 复用它。
这正是上面 `Counting Writer` 做法的价值——它让"只有一处判断"成为默认。

#### 6.7.3 生成代码**不得**做超时判定；v1 不向业务暴露 `ServerContext`

**业务超时的唯一权威判定点在框架侧**：`server.cpp` 的 worker 出队处比较
`NowMs() - start_ms > request_timeout_ms`，超时直接回 `kTimeout` 且**不调用业务**。
`start_ms` 取的是"收到帧"的时刻，因此**排队时间被这一条覆盖**，不需要第二处判断。

生成代码的三条禁令：

1. **不得在调用业务前后各判一次超时。** 两头都判会出现"客户端 `kTimeout`、服务端 `kOk`"
   的日志不一致——排查时两边对不上，是极难查的一类问题。
2. **不得把 `ServerContext`（或其成员）传给业务实现。**
3. **不得调用 `ServerContext::deadline_exceeded()`。** 该方法在 v1 仅保留声明与定义
   （`server/dispatcher.h` / `dispatcher.cpp`），**框架内部也尚未调用它**——
   `server.cpp` 的排空路径（`CoordinatorLoop`）用的是自己的局部 `deadline` 与
   `global_pending` 计数，不是这个方法。保留它只为接口完整性，
   它**不进入生成物契约**。

**v1 不向业务实现暴露 `ServerContext`**，服务基类签名固定为
`Status Echo(const Req&, Resp*)`。理由是**所有权**：一旦把请求生命周期泄漏给业务，
业务把它（或它引用的对象）存到回调之外——异步日志、线程池再投递、本地缓存——就是悬垂引用，
而框架在编译期无法阻止这种写法。宁可不给，也不给一个"必须靠人记住"的约定。

**代价（诚实列出）**：「业务侧协作式取消」在 v1 **不可用**。长耗时任务请拆成多次 RPC，
或由业务在**自己的**状态里做进度检查。v2 若引入，只考虑两种形态之一：
显式新增带 `ServerContext*` 的重载（契约变更、需版本化），
或 thread_local 访问器（必须同时定义"业务执行期外调用即未定义"）。

生成物的现状是合规的：handler 收到 `ServerContext* ctx` 后只写 `(void)ctx;`，
既不读也不往下传。§6.4 的骨架保持这个形态。

---

## 7. 错误诊断格式

codegen 的所有诊断遵循统一格式，便于编辑器跳转与 CI 解析：

```
<file>:<line>:<column>: error: <message>
<file>:<line>:<column>: warning: <message>
```

示例：
```
idl/echo.qz:31:12: error: duplicate field tag 3 in message EchoRequest (first used by "ids")
idl/echo.qz:52:3: error: method_id collision: demo.EchoService.Echo and demo.Other.Echo both map to 0x21678893
idl/echo.qz:12:1: error: import is not supported by qz v1 (planned for v2)
```

退出码：`0` 全部通过；`1` 存在 error。

---

## 8. 测试策略（codegen 的"单元测试"该长什么样）

### 8.1 实际存在的 4 个测试文件

| 测试文件 | 用例数 | 验的是什么 |
| --- | ---: | --- |
| `codegen/tests/test_parser.py` | ~60 | lexer 与 EBNF 的每个产生式都有正例 + 反例；`import/extend/oneof/map/group` 必须报"不支持"而非静默忽略 |
| `codegen/tests/test_semantic.py` | ~63 | C-01 ~ C-12 每条约束至少一个用例（含边界：tag=536870911、tag=18999/19000、枚举首值非 0） |
| `codegen/tests/test_methodid.py` | ~17 | 与 C++ 侧 `MethodIdOfName` 交叉验证：文档里写死的两条 id、C++ 侧常量表、**150 条随机 `full_name`**（含 Unicode）三路一致。这是**最关键的一条**——两侧不一致 = 所有调用都 404 |
| `codegen/tests/test_golden.py` | ~40 | `idl/echo.qz` 的生成结果与仓库 `generated/` 的提交内容**逐字节一致**；两次生成必须完全相同；默认不写时间戳；文件头契约、include guard、`= default` 四件套、`Fields()` 表结构等形状断言 |
| **合计** | **~180** | 用例数会随开发增长，**以实际输出为准**，不要在别处引用具体数字 |

运行方式（`tests/` 下没有 `__init__.py`，所以 `-t` 必须指到 `tests` 本身，`-t .` 会报
`Start directory is not importable`）：

```bash
cd codegen && PYTHONPATH=. python -m unittest discover -s tests -p "test_*.py" -t tests
# 期望输出：Ran <N> tests ... OK   （N 会随用例增长；失败数必须为 0）
```

> 注意：`test_golden.py` 读的是仓库里的 `generated/`。**如果此刻有人正在重新生成**，
> 测试可能读到写了一半的文件而偶发失败。跑黄金测试前请确认没有并发的 `qzcodegen` 在跑。

### 8.2 原计划 6 个、实际 4 个：两处差异的交代

设计初稿列了 6 个测试文件，落地时少了两个。**不是被砍掉，而是换了更强的实现方式或受前置条件阻塞**，
这里逐条说清楚，避免后来的人以为漏做：

| 原计划 | 现状 | 说明 |
| --- | --- | --- |
| `test_roundtrip.py`（Python 构造 protobuf 字节喂给 C++ 的 `Decode`） | **已用更强的形式覆盖**：`.scratch/check_generated.cpp` | 纯 Python `unittest` 根本跑不了 C++ 代码，"在 Python 测试里调 C++"只能靠子进程编译 + 执行，那本质上是一个独立的集成测试，不属于 codegen 的单元测试。所以这件事挪到了 C++ 侧的 golden bytes 校验程序（`.scratch/check_generated.cpp`，**临时校验用，不入库**）：它按 protobuf 规范手工拼字节、调生成的 `Decode` 断言字段值，再 `Encode` 回去断言字节不变（顺带覆盖 §6.4 的未知字段回写）。**待办**：把它固化成 `tests/test_generated_roundtrip.cpp` 并接进 ctest（归关千锤的验收范围）。 |
| `test_selfbootstrap.py`（用 codegen 处理 `idl/registry.qz`，产物应与手写的 `registry_client_proto.h` 语义一致） | **被前置条件阻塞** | 依赖 `idl/registry.qz`，仓库目前没有这个文件——注册中心的协议消息是手写在 `src/registry/registry_client_proto.h` 里的（它本身就是"生成物该长什么样"的参照物）。**待办**：补 `idl/registry.qz`，然后要么让 codegen 产物取代手写实现，要么保留手写并把该测试降级为"结构一致性对照"。 |

### 8.3 ⚠️ CI 编译门（比单元测试更重要的一条）

**单元测试全绿 ≠ 生成代码能编译。** 这是本项目踩过的最贵的一个坑：

`idl/echo.qz` 只有 8 种字段形态。第一版生成器在它上面 `/W4` 零警告、当时全部测试全绿，
但一上压力输入 `codegen/tests/fixtures/kitchen_sink.qz`（11 个标量 × 4 种 label
+ `packed = false` + 嵌套 message + 多级包名 `stress.test` + 各种边界默认值）
就炸出 **4 个真 bug**：

1. **packed 块的 tag 误用了元素本身的 `kVarint`**（应该是 `kLengthDelimited`）。
   这是**线格式错**：编译通过、单元测试通过，只有真正跨端解码时才暴露——
   即"两个都按 doc 04 实现的东西互相读不懂"。这类 bug 用单元测试几乎不可能发现，
   因为测试和实现出自同一个错误理解。
2. 负数 `int32` 默认值丢符号（符号扩展没做）。
3. `repeated message` 的访问器名被格式化串参数顺序写坏。
4. 多级包名没有转成嵌套 `namespace a { namespace b {`。

所以 **CI 必须加一道编译门**，作为那批 Python 用例的补充。

**门禁脚本（POSIX shell 形态，Windows 下见下方注意事项）：**

```bash
for qz in idl/*.qz codegen/tests/fixtures/*.qz; do
  out=$(mktemp -d)
  PYTHONPATH=codegen python -m qzcodegen --input "$qz" --out "$out" --cpp -q || exit 1
  for cc in "$out"/*.qz.cpp; do
    cl /nologo /c /std:c++17 /EHsc /W4 /WX /utf-8 /permissive- \
       /I src /I "$out" "$cc" /Fo"$out/tu.obj" || exit 1      # /WX：警告即失败
  done
done
```

四条要点：

- **必须 `/WX`**。K-01 约束下的自研序列化层很容易产生"有符号/无符号比较""窄化转换"这类警告，
  放过一个就会攒成一堆。
- **不链接**，只做 `/c`。实测 MSVC 14.51 每个翻译单元约 **1.6 s**（`echo.qz.cpp` 1568 ms、
  `kitchen_sink.qz.cpp` 1580 ms，冷启动），当前两个输入合计 < 5 s。链接要等运行库齐备，
  会把 CI 拖慢十几倍。
- **`codegen/tests/fixtures/*.qz` 与 `idl/*.qz` 同等对待**。`idl/` 是"业务需要的"，
  `fixtures/` 是"打满分支的"；只测前者等于只测了 8 种字段形态。
- **Windows 上必须先把路径转成 Windows 形式**（Git Bash 下 `cygpath -w`）。直接把
  `/tmp/xxx.cpp` 交给 `cl` 会得到 `D8003: 缺少源文件`——`cl` 会把 `/` 开头的东西当选项。
  这个坑实测踩过，写 CI 脚本时注意。

**当前状态（已实测）**：`idl/echo.qz` 与 `codegen/tests/fixtures/kitchen_sink.qz` 的生成物
都在这道门下**零警告通过**（`/W4 /WX /permissive-`）。也就是说这 4 个 bug 已经全部修掉了，
编译门是用来防回归的，不是用来记录未修问题的。

**注意 `qzcodegen` 的 warning 与 `cl` 的 warning 语义不同**：codegen 的 warning
（例如 §3.1 提到的 `[method_id]` 覆盖提示）**退出码仍是 0**，不阻断流水线；
而 `cl /WX` 的 warning 直接失败。构建脚本不要用同一条规则处理两者。

**为什么这条比单测更值钱**：单元测试验证的是"我以为的逻辑"，编译门验证的是
"这些代码真的能被编译器接受"。前者可以自我欺骗，后者不能。而且它是唯一能挡住
"生成器在压力输入下崩掉"的手段。

---

## 9. 完整语法示例（覆盖全部特性）

```qz
syntax = "qz1";
package demo;

enum SiteType {
  UNKNOWN = 0;
  BLOG    = 1;
  NEWS    = 2;
  VIDEO   = 3;
}

message EchoBody {
  string text   = 1;
  int32  weight = 2;
  double score  = 3;
  bool   valid  = 4;
}

message EchoRequest {
  string          message = 1;
  int32           repeat  = 2 [default = 1];
  repeated int32  ids     = 3;
  repeated string tags    = 4;
  optional string lang    = 5;
  EchoBody        body    = 6;
  bytes           raw     = 7;
  SiteType        site    = 8;
}

message EchoResponse {
  string message   = 1;
  int32  served_by = 2;
}

service EchoService {
  rpc Echo(EchoRequest) returns (EchoResponse) [idempotent = true];
  rpc Notify(EchoRequest) returns (EchoResponse);
}
```

与仓库里的 `idl/echo.qz` 完全一致（该文件是本文档的落地副本，两者必须保持同步）。
