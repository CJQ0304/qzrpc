"""AST 节点（G-03）。

全部 dataclass，全部带 line/col —— 报错时要把用户精确地指到"哪一行的哪一列"，
所以位置信息不是可选装饰，是节点的一部分。
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Optional

# ---- 字段 label ----
LABEL_NONE = ""  # 无 label = 单数
LABEL_OPTIONAL = "optional"
LABEL_REPEATED = "repeated"

# ---- 类型引用解析结果 ----
KIND_SCALAR = "scalar"
KIND_MESSAGE = "message"
KIND_ENUM = "enum"

# ---- 字面量种类 ----
LIT_INT = "int"
LIT_FLOAT = "float"
LIT_STRING = "string"
LIT_BOOL = "bool"

# 可 packed 的标量类型（doc 04 §4.1：repeated 数值类型默认 packed）。
PACKABLE_SCALARS = frozenset(
    ("int32", "int64", "uint32", "uint64", "sint32", "sint64", "bool", "float", "double")
)

NUMERIC_SCALARS = PACKABLE_SCALARS
STRING_SCALARS = frozenset(("string", "bytes"))
ALL_SCALARS = frozenset(PACKABLE_SCALARS | STRING_SCALARS)


@dataclass
class Literal:
    """IDL 里的一个字面量。`text` 是源码原文，元数据表里的 default_value 用它。"""

    kind: str
    value: object
    text: str
    line: int
    col: int


@dataclass
class TypeRef:
    """一次类型引用。

    `resolved_*` 由 semantic 填：C++ 侧必须知道它是标量、message 还是 enum，
    才能选对编码方式（varint / length-delimited）与访问器形态。
    """

    name: str
    line: int
    col: int
    resolved_kind: str = ""
    resolved_name: str = ""
    resolved_node: object = None

    @property
    def is_scalar(self) -> bool:
        return self.resolved_kind == KIND_SCALAR

    @property
    def is_message(self) -> bool:
        return self.resolved_kind == KIND_MESSAGE

    @property
    def is_enum(self) -> bool:
        return self.resolved_kind == KIND_ENUM

    @property
    def packable(self) -> bool:
        """该类型能否 packed 编码：数值型标量（含 bool/float/double）或 enum。"""
        if self.resolved_kind == KIND_ENUM:
            return True
        if self.resolved_kind != KIND_SCALAR:
            return False
        return self.name in PACKABLE_SCALARS


@dataclass
class Field:
    name: str
    type_ref: TypeRef
    tag: int
    line: int
    col: int
    label: str = LABEL_NONE
    default: Optional[Literal] = None
    # None = 未写 [packed = ...]；True/False = 显式指定。
    packed: Optional[bool] = None
    deprecated: bool = False

    @property
    def repeated(self) -> bool:
        return self.label == LABEL_REPEATED

    @property
    def optional(self) -> bool:
        return self.label == LABEL_OPTIONAL

    @property
    def packed_effective(self) -> bool:
        """最终是否按 packed 编码：repeated + 可 packed + 未被 [packed = false] 关掉。"""
        if not self.repeated:
            return False
        if self.packed is False:
            return False
        if self.packed is True:
            return True
        return self.type_ref.packable


@dataclass
class EnumValue:
    name: str
    number: int
    line: int
    col: int


@dataclass
class EnumDecl:
    name: str
    values: list[EnumValue]
    line: int
    col: int
    # 嵌套声明时的父消息路径（"Outer" / "Outer.Inner"），空串表示文件顶层。
    parent_path: str = ""

    @property
    def cpp_name(self) -> str:
        """打平后的 C++ 类名：嵌套类型也只占一个标识符（不做重命名）。"""
        return self.name

    def zero_value_name(self) -> Optional[str]:
        """数值为 0 的枚举项名（C-02 保证一定存在）。"""
        for value in self.values:
            if value.number == 0:
                return value.name
        return None

    def full_name(self, package: str) -> str:
        return _join_name(package, self.parent_path, self.name)


@dataclass
class MessageDecl:
    name: str
    line: int
    col: int
    fields: list[Field] = field(default_factory=list)
    messages: list["MessageDecl"] = field(default_factory=list)
    enums: list[EnumDecl] = field(default_factory=list)
    parent_path: str = ""

    @property
    def cpp_name(self) -> str:
        return self.name

    def sorted_fields(self) -> list[Field]:
        """按 tag 升序 —— Encode 必须按字段号升序输出（doc 04 §6.3）。"""
        return sorted(self.fields, key=lambda f: f.tag)

    def full_name(self, package: str) -> str:
        return _join_name(package, self.parent_path, self.name)


@dataclass
class RpcDecl:
    name: str
    request: TypeRef
    response: TypeRef
    line: int
    col: int
    idempotent: Optional[bool] = None
    explicit_method_id: Optional[int] = None
    timeout_ms: Optional[int] = None
    # 由 semantic 填入最终 id（显式覆盖或自动计算）。
    method_id: int = 0
    full_name: str = ""

    @property
    def is_idempotent(self) -> bool:
        return bool(self.idempotent)


@dataclass
class ServiceDecl:
    name: str
    methods: list[RpcDecl]
    line: int
    col: int

    def full_name(self, package: str) -> str:
        return _join_name(package, "", self.name)


@dataclass
class OptionDecl:
    name: str
    value: Literal
    line: int
    col: int


@dataclass
class FileNode:
    """一个 .qz 文件的完整 AST。"""

    # 展示用路径（写进生成物文件头：必须是相对路径、不含机器相关信息）。
    display_path: str
    # 绝对路径（读文件、报错前缀用）。
    abs_path: str
    syntax: Optional[str] = None
    syntax_line: int = 0
    syntax_col: int = 0
    # `syntax` 是否真的是文件第一条语句（C-01 的"顺序"要求）。
    syntax_first: bool = True
    package: str = ""
    package_line: int = 0
    package_col: int = 0
    options: list[OptionDecl] = field(default_factory=list)
    enums: list[EnumDecl] = field(default_factory=list)
    messages: list[MessageDecl] = field(default_factory=list)
    services: list[ServiceDecl] = field(default_factory=list)

    # ---- 遍历 ----

    def iter_messages(self) -> list[MessageDecl]:
        """全部消息（含嵌套），打平后按声明顺序返回。"""
        out: list[MessageDecl] = []

        def walk(node: MessageDecl) -> None:
            out.append(node)
            for child in node.messages:
                walk(child)

        for message in self.messages:
            walk(message)
        return out

    def iter_enums(self) -> list[EnumDecl]:
        out: list[EnumDecl] = list(self.enums)
        for message in self.iter_messages():
            out.extend(message.enums)
        return out


def _join_name(package: str, parent_path: str, name: str) -> str:
    parts = [p for p in (package, parent_path, name) if p]
    return ".".join(parts)
