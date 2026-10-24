"""测试公用设施（纯标准库，不依赖 pytest）。

把 sys.path 的处理集中在这里：`python -m unittest discover -s codegen/tests -v`
会把本目录放进 sys.path[0]，于是 `import _support` 可用，而它负责把
`codegen/` 也放进 sys.path，`import qzcodegen` 才能成功。
"""

from __future__ import annotations

import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_CODEGEN_DIR = os.path.dirname(_HERE)
_REPO_ROOT = os.path.dirname(_CODEGEN_DIR)

if _CODEGEN_DIR not in sys.path:
    sys.path.insert(0, _CODEGEN_DIR)

from qzcodegen import parser, semantic  # noqa: E402  (必须晚于 sys.path 设置)
from qzcodegen.diagnostics import DiagnosticBag, QzError  # noqa: E402

# 每个正例都从这三行起步；省得每个用例重复写。
HEADER = 'syntax = "qz1";\npackage demo;\n'


def repo_root() -> str:
    return _REPO_ROOT


def parse(text: str, path: str = "test.qz"):
    """只做词法 + 语法分析；失败抛 QzError。"""
    return parser.parse_source(text, path)


def analyze(text: str, path: str = "test.qz"):
    """解析 + 语义检查。返回 (node|None, bag)，语法错误也折进 bag。"""
    bag = DiagnosticBag()
    try:
        node = parse(text, path)
    except QzError as exc:
        bag.extend([exc.to_diagnostic()])
        return None, bag
    semantic.analyze([node], bag)
    return node, bag


def errors(bag: DiagnosticBag) -> list[str]:
    return [item.message for item in bag.items if item.severity == "error"]


def warnings(bag: DiagnosticBag) -> list[str]:
    return [item.message for item in bag.items if item.severity == "warning"]


def expect_error(case, bag: DiagnosticBag, needle: str) -> None:
    """断言 bag 里有一条错误信息包含 needle。"""
    messages = errors(bag)
    for message in messages:
        if needle in message:
            return
    case.fail("没有找到包含 {!r} 的错误；实际为 {!r}".format(needle, messages))


def expect_no_error(case, bag: DiagnosticBag) -> None:
    messages = errors(bag)
    if messages:
        case.fail("期望无错误，实际为 {!r}".format(messages))


def expect_parse_error(case, text: str, needle: str) -> None:
    """期望 parse 阶段就失败（词法/语法/不支持特性）。"""
    try:
        parse(text)
    except QzError as exc:
        case.assertIn(needle, exc.message)
        return
    case.fail("期望解析失败（{}），但解析通过了".format(needle))
