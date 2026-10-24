"""命名与类型映射规则（G-07）。

集中放在一处，是因为"IDL 里的名字 → C++ 里的名字"如果散落在生成器各分支里，
就没人能回答"这个字段到底叫什么"这种问题。这里的每个函数都是纯函数，
可以被单元测试直接钉住。
"""

from __future__ import annotations

from . import ast_nodes as ast

# IDL 标量类型 → C++ 类型（doc 04 §4.1）。
SCALAR_CPP = {
    "int32": "std::int32_t",
    "int64": "std::int64_t",
    "uint32": "std::uint32_t",
    "uint64": "std::uint64_t",
    "sint32": "std::int32_t",
    "sint64": "std::int64_t",
    "bool": "bool",
    "float": "float",
    "double": "double",
    "string": "std::string",
    "bytes": "std::string",  # bytes 在 C++ 侧就是 std::string（可含 \0）
}

# 标量类型 → serde::FieldType 枚举项（serialize/field_meta.h）。
SCALAR_FIELD_TYPE = {
    "int32": "kInt32",
    "int64": "kInt64",
    "uint32": "kUInt32",
    "uint64": "kUInt64",
    "sint32": "kSInt32",
    "sint64": "kSInt64",
    "bool": "kBool",
    "float": "kFloat",
    "double": "kDouble",
    "string": "kString",
    "bytes": "kBytes",
}

# 标量类型 → serde::WireType 枚举项。
SCALAR_WIRE_TYPE = {
    "int32": "kVarint",
    "int64": "kVarint",
    "uint32": "kVarint",
    "uint64": "kVarint",
    "sint32": "kVarint",
    "sint64": "kVarint",
    "bool": "kVarint",
    "float": "kFixed32",
    "double": "kFixed64",
    "string": "kLengthDelimited",
    "bytes": "kLengthDelimited",
}

# 标量类型 → Writer 的写入方法（单值，不带 tag）。
SCALAR_WRITE = {
    "int32": "WriteInt32",
    "int64": "WriteInt64",
    "uint32": "WriteVarint32",
    "uint64": "WriteVarint64",
    "sint32": "WriteSInt32",
    "sint64": "WriteSInt64",
    "bool": "WriteBool",
    "float": "WriteFloat",
    "double": "WriteDouble",
}

# 标量类型 → Reader 的"带 wire_type 校验"的字段读取方法。
SCALAR_READ_FIELD = {
    "int32": "ReadInt32Field",
    "int64": "ReadInt64Field",
    "uint32": "ReadVarint32Field",
    "uint64": "ReadVarint64Field",
    "sint32": "ReadSInt32Field",
    "sint64": "ReadSInt64Field",
    "bool": "ReadBoolField",
    "float": "ReadFloatField",
    "double": "ReadDoubleField",
    "string": "ReadStringField",
    # bytes 只能借 ReadStringField 完成拷贝：src/serialize/reader.h 里
    # **没有** ReadBytesField（doc 04 §6.6 列了这个符号，属于文档偏差）。
    "bytes": "ReadStringField",
}

# 标量类型 → Reader 的"无 tag"读取方法（packed 元素用）。
SCALAR_READ_BARE = {
    "int32": "ReadInt32",
    "int64": "ReadInt64",
    "uint32": "ReadVarint32",
    "uint64": "ReadVarint64",
    "sint32": "ReadSInt32",
    "sint64": "ReadSInt64",
    "bool": "ReadBool",
    "float": "ReadFloat",
    "double": "ReadDouble",
}

# 标量类型的 C++ 零值表达式。
SCALAR_ZERO = {
    "int32": "0",
    "int64": "0",
    "uint32": "0",
    "uint64": "0",
    "sint32": "0",
    "sint64": "0",
    "bool": "false",
    "float": "0.0F",
    "double": "0.0",
}

INT64_SAFE_MAX = 2**31 - 1
UINT32_SAFE_MAX = 2**32 - 1


# ---------------------------------------------------------------- 文件名 / 宏


def header_guard(package: str, file_stem: str) -> str:
    """include guard 宏名（doc 04 §6.2）。

    `QZRPC_GENERATED_` + <package 全大写 '.'→'_'> + `_` + <文件名全大写 非字母数字→'_'> + `_H_`
    """
    pkg = _macro_safe(package)
    name = _macro_safe(file_stem)
    parts = [p for p in ("QZRPC_GENERATED", pkg, name) if p]
    return "_".join(parts) + "_H_"


def _macro_safe(text: str) -> str:
    out = []
    for ch in text.upper():
        out.append(ch if ch.isalnum() else "_")
    result = "".join(out)
    while "__" in result:
        result = result.replace("__", "_")
    return result.strip("_")


def generated_header_name(file_stem: str) -> str:
    """`echo` → `echo.qz.h`。"""
    return "{}.qz.h".format(file_stem)


def generated_source_name(file_stem: str) -> str:
    return "{}.qz.cpp".format(file_stem)


def namespace_open(package: str, indent: str = "") -> list[str]:
    if not package:
        return []
    return ["{}namespace {} {{".format(indent, package.replace(".", "::"))]


def namespace_close(package: str, indent: str = "", comment: bool = True) -> list[str]:
    if not package:
        return []
    suffix = "  // namespace {}".format(package.replace(".", "::")) if comment else ""
    return ["{}}}{}".format(indent, suffix)]


# ---------------------------------------------------------------- 类型名


def cpp_type(ref: ast.TypeRef) -> str:
    """单值（非 repeated）的 C++ 类型。"""
    if ref.resolved_kind == ast.KIND_SCALAR:
        return SCALAR_CPP[ref.name]
    if ref.resolved_node is not None:
        return ref.resolved_node.cpp_name
    return ref.name  # 解析失败时原样输出，让 C++ 编译器替我们报错


def cpp_container_type(ref: ast.TypeRef) -> str:
    """repeated 字段的容器类型。"""
    return "std::vector<{}>".format(cpp_type(ref))


def field_type_enum(ref: ast.TypeRef) -> str:
    if ref.resolved_kind == ast.KIND_SCALAR:
        return SCALAR_FIELD_TYPE[ref.name]
    if ref.resolved_kind == ast.KIND_ENUM:
        return "kEnum"
    return "kMessage"


def wire_type_enum(ref: ast.TypeRef) -> str:
    if ref.resolved_kind == ast.KIND_SCALAR:
        return SCALAR_WIRE_TYPE[ref.name]
    if ref.resolved_kind == ast.KIND_ENUM:
        return "kVarint"
    return "kLengthDelimited"


def idl_type_name(ref: ast.TypeRef) -> str:
    """注释里显示的 IDL 类型名（message/enum 用全名）。"""
    if ref.resolved_kind in (ast.KIND_MESSAGE, ast.KIND_ENUM):
        return ref.resolved_name or ref.name
    return ref.name


# ---------------------------------------------------------------- 默认值


def zero_expr(field: ast.Field) -> str:
    """字段"默认值表达式"：Clear() 里的赋值与成员初始化都用它。"""
    if field.default is not None:
        return literal_expr(field.default, field.type_ref)
    ref = field.type_ref
    if ref.resolved_kind == ast.KIND_ENUM:
        enum = ref.resolved_node
        zero_name = enum.zero_value_name() if enum is not None else None
        if zero_name is None:
            return "static_cast<{}>(0)".format(cpp_type(ref))
        return "{}::{}".format(cpp_type(ref), zero_name)
    if ref.resolved_kind == ast.KIND_MESSAGE:
        return ""  # message 字段用 .Clear()，不靠赋值
    if ref.name in SCALAR_ZERO:
        return SCALAR_ZERO[ref.name]
    return "0"


def literal_expr(literal: ast.Literal, ref: ast.TypeRef) -> str:
    """把 IDL 字面量渲染成 C++ 表达式。"""
    if literal.kind == ast.LIT_BOOL:
        return "true" if literal.value else "false"
    if literal.kind == ast.LIT_STRING:
        return cpp_string_literal(literal.value)
    if literal.kind == ast.LIT_FLOAT:
        return _float_expr(float(literal.value), ref)
    if literal.kind == ast.LIT_INT:
        return _int_expr(int(literal.value), ref)
    return "0"


def _int_expr(value: int, ref: ast.TypeRef) -> str:
    if ref.resolved_kind == ast.KIND_ENUM:
        enum = ref.resolved_node
        if enum is not None:
            for item in enum.values:
                if item.number == value:
                    return "{}::{}".format(cpp_type(ref), item.name)
        return "static_cast<{}>({})".format(cpp_type(ref), value)
    scalar = ref.name
    if scalar in ("uint32",):
        return "{}U".format(value) if value > INT64_SAFE_MAX else str(value)
    if scalar in ("uint64",):
        return "{}ULL".format(value)
    if scalar in ("int64", "sint64"):
        if value == -(2**63):
            # -9223372036854775808LL 会被解析成 -(9223372036854775808LL)，
            # 而那个字面量是 unsigned，MSVC 会告警 C4146 并收缩失败。
            return "(-9223372036854775807LL - 1)"
        if value > INT64_SAFE_MAX or value < -INT64_SAFE_MAX:
            return "{}LL".format(value)
    if scalar in ("int32", "sint32") and value == -(2**31):
        # -2147483648 会被解析成 -(2147483648)，写成 (-2147483647 - 1) 避免溢出告警。
        return "(-2147483647 - 1)"
    return str(value)


def _float_expr(value: float, ref: ast.TypeRef) -> str:
    text = repr(value)
    if "e" in text or "E" in text or "." in text or "inf" in text:
        pass
    else:
        text += ".0"
    if ref.name == "float":
        return "{}F".format(text)
    return text


def cpp_string_literal(text: str) -> str:
    """生成合法的 C++ 字符串字面量（含引号），非 ASCII 用八进制转义收窄。"""
    out = ['"']
    for ch in text:
        code = ord(ch)
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\t":
            out.append("\\t")
        elif ch == "\r":
            out.append("\\r")
        elif ch == "\0":
            out.append("\\0")
        elif 0x20 <= code < 0x7F:
            out.append(ch)
        else:
            for byte in ch.encode("utf-8"):
                out.append("\\{:03o}".format(byte))
    out.append('"')
    return "".join(out)


# ---------------------------------------------------------------- 访问器名


def accessor_names(field_name: str) -> dict[str, str]:
    """字段名 → 全套访问器名（doc 04 §4.2）。"""
    return {
        "get": field_name,
        "set": "set_{}".format(field_name),
        "mutable": "mutable_{}".format(field_name),
        "has": "has_{}".format(field_name),
        "clear": "clear_{}".format(field_name),
        "size": "{}_size".format(field_name),
        "add": "add_{}".format(field_name),
    }


def member_name(field_name: str) -> str:
    """私有成员名：字段名 + 下划线（与访问器区分，避免同名冲突）。"""
    return "{}_".format(field_name)


def has_flag_name(field_name: str) -> str:
    return "has_{}_".format(field_name)


def method_id_constant_name(method_name: str) -> str:
    """`Echo` → `kMethodIdEcho`。"""
    return "kMethodId{}".format(method_name)


def method_id_function_name(method_name: str) -> str:
    """`Echo` → `MethodIdEcho`。"""
    return "MethodId{}".format(method_name)


def handler_name(method_name: str, service_name: str, need_service_prefix: bool) -> str:
    """匿名 namespace 里的方法处理器名。

    单服务（最常见）时用 `Handle<Method>`，可读性最好；一旦同一文件里出现
    重名方法，就统一改成 `Handle<Service><Method>`，避免匿名 namespace 内冲突。
    """
    if need_service_prefix:
        return "Handle{}{}".format(service_name, method_name)
    return "Handle{}".format(method_name)


def dispatcher_class_name(service_name: str) -> str:
    return "{}Dispatcher".format(service_name)


def stub_class_name(service_name: str) -> str:
    return "{}Stub".format(service_name)


def base_class_name(service_name: str) -> str:
    return "{}Base".format(service_name)


def register_function_name(service_name: str) -> str:
    return "Register{}".format(service_name)


def needs_service_prefix(services: list[ast.ServiceDecl]) -> bool:
    """同一文件内是否存在跨服务重名的方法（决定处理器名要不要带服务前缀）。"""
    seen: set[str] = set()
    for service in services:
        for method in service.methods:
            if method.name in seen:
                return True
            seen.add(method.name)
    return False


def enum_name_function(enum_name: str) -> str:
    """`SiteType` → `SiteType_Name`。"""
    return "{}_Name".format(enum_name)


def enum_parse_function(enum_name: str) -> str:
    return "{}_Parse".format(enum_name)
