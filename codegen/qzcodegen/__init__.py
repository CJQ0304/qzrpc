"""qzcodegen —— 轻舟 RPC 的 `.qz` IDL 代码生成器。

对外只暴露四个东西：
    compile_files()  读文件 → 解析 → 语义检查（不落盘，CI 的 --check 用它）
    write_outputs()  生成 .qz.h / .qz.cpp 并写入磁盘
    DiagnosticBag    诊断集合（`file:line:col: error: msg`）
    methodid         FNV-1a64 实现（与 C++ 侧 protocol::MethodIdOfName 必须一致）

模块分工（见 docs/00-file-map.md §8）：
    diagnostics  —— 统一诊断
    lexer        —— 手写词法扫描
    ast_nodes    —— AST dataclass
    parser       —— 递归下降，v1 不支持的语法立刻报错
    semantic     —— C-01 … C-12
    methodid     —— FNV-1a64 + method_id
    naming       —— 名字与类型映射
    cpp_generator—— 生成 doc 04 §6.3/§6.4 的骨架
"""

from __future__ import annotations

import os
import tempfile

from . import ast_nodes, diagnostics, lexer, methodid, naming, parser, semantic
from .cpp_generator import (
    GENERATOR_VERSION,
    PROTOCOL_VERSION,
    CppGenerator,
    GenerateOptions,
    generate,
)
from .diagnostics import Diagnostic, DiagnosticBag, QzError, UnsupportedFeature

__version__ = GENERATOR_VERSION

__all__ = [
    "ast_nodes",
    "diagnostics",
    "lexer",
    "methodid",
    "naming",
    "parser",
    "semantic",
    "CppGenerator",
    "GenerateOptions",
    "Diagnostic",
    "DiagnosticBag",
    "QzError",
    "UnsupportedFeature",
    "GENERATOR_VERSION",
    "PROTOCOL_VERSION",
    "compile_files",
    "write_outputs",
    "generate",
    "display_path",
]


def read_source(path: str) -> str:
    """读 IDL 源码。允许输入带 BOM（Windows 编辑器的默认行为）。"""
    with open(path, "r", encoding="utf-8-sig", newline="") as handle:
        return handle.read()


def display_path(abs_path: str, source_root: str) -> str:
    """生成物文件头里记的"源文件"路径。

    必须是**相对路径**且不含机器相关信息，否则每次生成的 git diff 都会抖。
    统一用 `/` 分隔符，保证 Windows 与 Linux 的产物逐字节相同。
    """
    try:
        relative = os.path.relpath(os.path.abspath(abs_path), os.path.abspath(source_root))
    except ValueError:  # 跨盘符时 relpath 会抛错
        relative = os.path.basename(abs_path)
    return relative.replace(os.sep, "/")


def compile_files(
    paths: list[str], source_root: str = ".", bag: DiagnosticBag | None = None
) -> tuple[list[ast_nodes.FileNode], DiagnosticBag]:
    """解析 + 语义检查全部输入。**不落盘**。

    返回（已通过解析的 AST 列表, 诊断）。诊断里有 error 时调用方应退出码 1。
    """
    bag = bag or DiagnosticBag()
    nodes: list[ast_nodes.FileNode] = []
    for path in paths:
        shown = display_path(path, source_root)
        try:
            source = read_source(path)
        except OSError as exc:
            bag.error(shown, 1, 1, "cannot read input file: {}".format(exc.strerror or exc))
            continue
        try:
            node = parser.parse_source(source, shown, abs_path=os.path.abspath(path))
        except QzError as exc:
            bag.extend([exc.to_diagnostic()])
            continue
        nodes.append(node)

    if nodes:
        semantic.analyze(nodes, bag)
    return nodes, bag


def file_stem(path: str) -> str:
    """`idl/echo.qz` → `echo`。"""
    base = os.path.basename(path)
    if base.endswith(".qz"):
        return base[: -len(".qz")]
    return os.path.splitext(base)[0]


def write_outputs(
    nodes: list[ast_nodes.FileNode],
    out_dir: str,
    source_root: str = ".",
    timestamp: str = "",
    dry_run: bool = False,
) -> list[tuple[str, bool]]:
    """生成并写入 `<out>/<base>.qz.h` / `.qz.cpp`。

    返回 [(路径, 是否真的写了)]。内容与磁盘一致时不写 —— 这样重复运行不会
    触碰 mtime，构建系统也就不会被无意义地触发。
    """
    results: list[tuple[str, bool]] = []
    for node in nodes:
        stem = file_stem(node.abs_path)
        options = GenerateOptions(
            source_rel_path=node.display_path,
            file_stem=stem,
            timestamp=timestamp,
        )
        header, source = generate(node, options)
        pairs = [
            (naming.generated_header_name(stem), header),
            (naming.generated_source_name(stem), source),
        ]
        for name, text in pairs:
            target = os.path.join(out_dir, name)
            written = False
            if not dry_run:
                written = _write_if_changed(target, text)
            results.append((target, written))
    return results


def _write_if_changed(path: str, text: str) -> bool:
    """原子写入：内容有变化才写，且任何时刻磁盘上都不存在"半写"的目标文件。

    为什么必须原子（不是"顺手优化"）：
      生成器与测试/其他人可能同时跑（`test_golden` 读的就是仓库里的
      `generated/`）。若直接 `open(path,'wb')` 后分块写，读方有机会读到
      "长度对但内容截断"的文件——这种失败是偶发的、难复现的，最耗人。
      换成"写临时文件 → fsync → os.replace"之后，读方要么看到旧内容、
      要么看到新内容，**不存在中间态**。

    三个平台细节：
      * 临时文件必须与目标**同目录**：跨卷的 rename 在不同文件系统之间不是原子操作；
      * `os.replace` 而不是 `os.rename`：Windows 上 `os.rename` 目标已存在会抛
        FileExistsError，`os.replace` 才是"原子覆盖"；
      * `fsync` 在 replace 之前：否则崩溃时可能 rename 了但内容还在页缓存里
        （对生成物这种可重跑的文件不致命，但既然是原子写就做完整）。
    """
    payload = text.encode("utf-8")  # 显式无 BOM

    # 内容相同就不碰磁盘：保持 mtime 稳定，构建系统不会被无意义地触发。
    try:
        with open(path, "rb") as handle:
            if handle.read() == payload:
                return False
    except FileNotFoundError:
        pass

    directory = os.path.dirname(path) or "."
    if not os.path.isdir(directory):
        os.makedirs(directory, exist_ok=True)

    # `dir=` 保证临时文件与目标同目录；前缀用 '.' 让它更像隐藏文件，
    # 万一进程被强杀留下残骸也不会污染目录列表的可读性。
    handle_fd, tmp_path = tempfile.mkstemp(
        prefix="." + os.path.basename(path) + ".tmp.", dir=directory
    )
    try:
        with os.fdopen(handle_fd, "wb") as handle:
            handle.write(payload)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(tmp_path, path)   # 原子生效点
        return True
    except BaseException:
        # 任何失败（磁盘满、权限、被中断）都要清掉临时文件，
        # 不能在 generated/ 里留垃圾——下次有人 `ls` 会以为是产物。
        try:
            os.unlink(tmp_path)
        except OSError:
            pass
        raise
