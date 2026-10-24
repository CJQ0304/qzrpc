"""统一诊断（G-01）。

格式固定为 `<file>:<line>:<col>: error: <msg>`（doc 04 §7），
因为编辑器跳转与 CI 日志解析都按这个形状匹配。
line/col 从 1 开始。
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Iterable

ERROR = "error"
WARNING = "warning"


class QzError(Exception):
    """词法/语法层的硬错误（携带位置，供 CLI 转成 Diagnostic）。

    词法、语法错误采用"发现即抛"而不是收集：一个 token 出错后后续 token 流
    已经没有意义，继续解析只会产生一串和根因无关的次生报错。
    """

    def __init__(self, file: str, line: int, col: int, message: str) -> None:
        super().__init__("{}:{}:{}: error: {}".format(file, line, col, message))
        self.file = file
        self.line = line
        self.col = col
        self.message = message

    def to_diagnostic(self) -> "Diagnostic":
        return Diagnostic(self.file, self.line, self.col, ERROR, self.message)


class UnsupportedFeature(QzError):
    """v1 明确不支持的语法（import/extend/oneof/map/group/required/reserved）。"""


@dataclass(frozen=True)
class Diagnostic:
    file: str
    line: int
    col: int
    severity: str
    message: str

    def format(self) -> str:
        return "{}:{}:{}: {}: {}".format(
            self.file, self.line, self.col, self.severity, self.message
        )

    def __str__(self) -> str:  # 方便 print(diag)
        return self.format()


class DiagnosticBag:
    """收集诊断，最后统一输出。

    收集而不是"发现即抛异常"，是为了让一次运行尽量把所有问题都报出来——
    用户改一处、跑一次、又发现一处，是最消耗耐心的交互。
    """

    def __init__(self) -> None:
        self._items: list[Diagnostic] = []

    def error(self, file: str, line: int, col: int, message: str) -> None:
        self._items.append(Diagnostic(file, line, col, ERROR, message))

    def warning(self, file: str, line: int, col: int, message: str) -> None:
        self._items.append(Diagnostic(file, line, col, WARNING, message))

    def extend(self, others: Iterable[Diagnostic]) -> None:
        self._items.extend(others)

    @property
    def items(self) -> list[Diagnostic]:
        return list(self._items)

    def has_errors(self) -> bool:
        return any(d.severity == ERROR for d in self._items)

    def error_count(self) -> int:
        return sum(1 for d in self._items if d.severity == ERROR)

    def warning_count(self) -> int:
        return sum(1 for d in self._items if d.severity == WARNING)

    def messages(self) -> list[str]:
        return [d.format() for d in self._items]

    def __bool__(self) -> bool:
        # 有 error 才算"失败"，warning 不影响退出码。
        return self.has_errors()

    def __len__(self) -> int:
        return len(self._items)
