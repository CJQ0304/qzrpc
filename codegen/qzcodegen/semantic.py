"""语义检查（G-05）：doc 04 §3.1 的 C-01 … C-12。

分工原则：parser 只管"形状对不对"，这里只管"意思对不对"。
所有错误收集到 DiagnosticBag 里一次报完，不 fail-fast —— 用户改一处跑一次
的循环太贵了。

C-11（method_id 冲突）**必须是跨文件的**：L1 层冲突检测的意义就在于
"一次调用传入的全部 .qz 一起看"，所以 analyze() 接收文件列表。
"""

from __future__ import annotations

from . import ast_nodes as ast
from . import methodid
from .diagnostics import DiagnosticBag

# protobuf 保留 tag 区间（C-04）。
RESERVED_TAG_MIN = 19000
RESERVED_TAG_MAX = 19999
MAX_FIELD_TAG = 536870911  # 2^29 - 1（C-03）
MAX_METHOD_ID = 0xFFFFFFFF


class Analyzer:
    def __init__(self, bag: DiagnosticBag) -> None:
        self.bag = bag
        # 进程级（= 本次调用全部输入文件）的 method_id → full_name 表（C-10/C-11）。
        self._method_ids: dict[int, tuple[str, bool]] = {}

    # ---------------- 入口 ----------------

    def analyze(self, files: list[ast.FileNode]) -> None:
        for node in files:
            self._check_syntax(node)
            self._check_enums(node)
            self._collect_types(node)
            self._resolve_fields(node)
            self._check_fields(node)
            self._check_services(node)

        # 跨文件的两条：循环依赖与 method_id 唯一性。
        for node in files:
            self._check_circular_dependency(node)
        for node in files:
            self._assign_method_ids(node)

    # ---------------- 工具 ----------------

    def _error(self, node: ast.FileNode, line: int, col: int, message: str) -> None:
        self.bag.error(node.display_path, line, col, message)

    def _warning(self, node: ast.FileNode, line: int, col: int, message: str) -> None:
        self.bag.warning(node.display_path, line, col, message)

    # ---------------- C-01 ----------------

    def _check_syntax(self, node: ast.FileNode) -> None:
        if node.syntax is None:
            self._error(
                node,
                1,
                1,
                'unsupported syntax version: missing `syntax = "qz1";` '
                "declaration (it must be the first statement)",
            )
            return
        if node.syntax != "qz1":
            self._error(
                node,
                node.syntax_line,
                node.syntax_col,
                'unsupported syntax version "{}" (expected "qz1")'.format(node.syntax),
            )
        elif not node.syntax_first:
            self._error(
                node,
                node.syntax_line,
                node.syntax_col,
                "unsupported syntax version: `syntax` must be the first statement",
            )
        if not node.package:
            self._warning(
                node,
                node.syntax_line,
                node.syntax_col,
                "no `package` declaration: full_name will be `<Service>.<Method>` "
                "without a package prefix",
            )

    # ---------------- C-02 ----------------

    def _check_enums(self, node: ast.FileNode) -> None:
        for decl in node.iter_enums():
            if not decl.values:
                self._error(
                    node,
                    decl.line,
                    decl.col,
                    "enum {} must start at 0 (enum has no values)".format(decl.name),
                )
                continue
            if decl.values[0].number != 0:
                self._error(
                    node,
                    decl.values[0].line,
                    decl.values[0].col,
                    "enum {} must start at 0 (first value '{}' is {})".format(
                        decl.name, decl.values[0].name, decl.values[0].number
                    ),
                )
            seen_names: dict[str, ast.EnumValue] = {}
            seen_numbers: dict[int, ast.EnumValue] = {}
            for value in decl.values:
                if value.name in seen_names:
                    self._error(
                        node,
                        value.line,
                        value.col,
                        'duplicate enum value name "{}" in enum {}'.format(
                            value.name, decl.name
                        ),
                    )
                else:
                    seen_names[value.name] = value
                if value.number in seen_numbers:
                    self._error(
                        node,
                        value.line,
                        value.col,
                        "duplicate enum value {} in enum {} (first used by "
                        '"{}")'.format(value.number, decl.name, seen_numbers[value.number].name),
                    )
                else:
                    seen_numbers[value.number] = value

    # ---------------- 类型表（C-05 的前置） ----------------

    def _collect_types(self, node: ast.FileNode) -> None:
        table: dict[str, object] = {}
        for decl in list(node.iter_enums()) + list(node.iter_messages()):
            if decl.name in table:
                self._error(
                    node,
                    decl.line,
                    decl.col,
                    'duplicate type name "{}" (nested types are flattened into the '
                    "same namespace, so names must be unique in one file)".format(decl.name),
                )
                continue
            table[decl.name] = decl
        # 类型全名也可以被引用（如字段写成 demo.EchoBody）。
        node._type_table = table  # type: ignore[attr-defined]
        full_table: dict[str, object] = {}
        for name, decl in table.items():
            full_table[name] = decl
            full_table[decl.full_name(node.package)] = decl
        node._full_type_table = full_table  # type: ignore[attr-defined]

    def _lookup(self, node: ast.FileNode, name: str):
        table = getattr(node, "_full_type_table", {})
        return table.get(name)

    # ---------------- C-05 ----------------

    def _resolve_fields(self, node: ast.FileNode) -> None:
        for message in node.iter_messages():
            for field in message.fields:
                ref = field.type_ref
                if ref.name in ast.ALL_SCALARS:
                    ref.resolved_kind = ast.KIND_SCALAR
                    ref.resolved_name = ref.name
                    continue
                decl = self._lookup(node, ref.name)
                if decl is None:
                    self._error(
                        node,
                        ref.line,
                        ref.col,
                        'unknown type "{}" (only same-file messages/enums can be '
                        "referenced: qz v1 has no import)".format(ref.name),
                    )
                    continue
                if isinstance(decl, ast.MessageDecl):
                    ref.resolved_kind = ast.KIND_MESSAGE
                else:
                    ref.resolved_kind = ast.KIND_ENUM
                ref.resolved_node = decl
                ref.resolved_name = decl.full_name(node.package)

        for service in node.services:
            for rpc in service.methods:
                for ref in (rpc.request, rpc.response):
                    if ref.name in ast.ALL_SCALARS:
                        ref.resolved_kind = ast.KIND_SCALAR
                        ref.resolved_name = ref.name
                        continue
                    decl = self._lookup(node, ref.name)
                    if decl is None:
                        self._error(
                            node,
                            ref.line,
                            ref.col,
                            'unknown type "{}"'.format(ref.name),
                        )
                        continue
                    if isinstance(decl, ast.MessageDecl):
                        ref.resolved_kind = ast.KIND_MESSAGE
                    else:
                        ref.resolved_kind = ast.KIND_ENUM
                    ref.resolved_node = decl
                    ref.resolved_name = decl.full_name(node.package)

    # ---------------- C-03 / C-04 / C-07 / C-08 / C-09 ----------------

    def _check_fields(self, node: ast.FileNode) -> None:
        for message in node.iter_messages():
            seen_tags: dict[int, ast.Field] = {}
            seen_names: dict[str, ast.Field] = {}
            for field in message.fields:
                if field.name in seen_names:
                    self._error(
                        node,
                        field.line,
                        field.col,
                        'duplicate field name "{}" in message {}'.format(
                            field.name, message.name
                        ),
                    )
                else:
                    seen_names[field.name] = field

                if field.tag < 1 or field.tag > MAX_FIELD_TAG:
                    self._error(
                        node,
                        field.line,
                        field.col,
                        "field tag {} in message {} is out of range 1..{}".format(
                            field.tag, message.name, MAX_FIELD_TAG
                        ),
                    )
                elif RESERVED_TAG_MIN <= field.tag <= RESERVED_TAG_MAX:
                    self._error(
                        node,
                        field.line,
                        field.col,
                        "tag {}-{} are reserved (field '{}' in message {} uses {})".format(
                            RESERVED_TAG_MIN,
                            RESERVED_TAG_MAX,
                            field.name,
                            message.name,
                            field.tag,
                        ),
                    )
                if field.tag in seen_tags:
                    self._error(
                        node,
                        field.line,
                        field.col,
                        'duplicate field tag {} in message {} (first used by "{}")'.format(
                            field.tag, message.name, seen_tags[field.tag].name
                        ),
                    )
                else:
                    seen_tags[field.tag] = field

                self._check_packed(node, message, field)
                self._check_default(node, message, field)

    def _check_packed(self, node: ast.FileNode, message: ast.MessageDecl, field: ast.Field) -> None:
        if field.packed is None:
            return
        if not field.repeated or not field.type_ref.packable:
            self._error(
                node,
                field.line,
                field.col,
                "packed is only allowed on repeated numeric fields "
                "(field '{}' in message {} is {})".format(
                    field.name, message.name, _describe_field_shape(field)
                ),
            )

    def _check_default(self, node: ast.FileNode, message: ast.MessageDecl, field: ast.Field) -> None:
        if field.default is None:
            return
        if field.repeated:
            self._error(
                node,
                field.line,
                field.col,
                "repeated field cannot have default (field '{}' in message {})".format(
                    field.name, message.name
                ),
            )
            return
        ref = field.type_ref
        literal = field.default
        if ref.resolved_kind == ast.KIND_MESSAGE:
            self._error(
                node,
                literal.line,
                literal.col,
                "default value type mismatch: field '{}' in message {} is a message "
                "type and cannot have a default".format(field.name, message.name),
            )
            return
        if ref.resolved_kind == ast.KIND_ENUM:
            if literal.kind != ast.LIT_INT:
                self._error(
                    node,
                    literal.line,
                    literal.col,
                    "default value type mismatch: field '{}' in message {} is enum {} "
                    "and needs an integer default, got {}".format(
                        field.name, message.name, ref.name, literal.kind
                    ),
                )
                return
            allowed = {v.number for v in ref.resolved_node.values}
            if literal.value not in allowed:
                self._error(
                    node,
                    literal.line,
                    literal.col,
                    "default value type mismatch: {} is not a value of enum {} used by "
                    "field '{}'".format(literal.value, ref.name, field.name),
                )
            return
        ok = _default_matches_scalar(ref.name, literal.kind)
        if not ok:
            self._error(
                node,
                literal.line,
                literal.col,
                "default value type mismatch: field '{}' in message {} is {} but the "
                "default is a {} literal ('{}')".format(
                    field.name, message.name, ref.name, literal.kind, literal.text
                ),
            )
            return
        if ref.name in ("int32", "int64", "sint32", "sint64", "uint32", "uint64"):
            low, high = _int_range(ref.name)
            if not (low <= literal.value <= high):
                self._error(
                    node,
                    literal.line,
                    literal.col,
                    "default value type mismatch: {} does not fit in {} (field '{}')".format(
                        literal.value, ref.name, field.name
                    ),
                )

    # ---------------- C-06 ----------------

    def _check_circular_dependency(self, node: ast.FileNode) -> None:
        graph: dict[str, ast.MessageDecl] = {m.name: m for m in node.iter_messages()}
        edges: dict[str, list[tuple[str, ast.Field]]] = {}
        for message in node.iter_messages():
            out: list[tuple[str, ast.Field]] = []
            for field in message.fields:
                ref = field.type_ref
                if ref.resolved_kind == ast.KIND_MESSAGE and ref.resolved_node is not None:
                    target = ref.resolved_node.name
                    if target in graph:
                        out.append((target, field))
            edges[message.name] = out

        WHITE, GRAY, BLACK = 0, 1, 2
        color = {name: WHITE for name in graph}

        def dfs(name: str, stack: list[str]) -> bool:
            color[name] = GRAY
            stack.append(name)
            for target, _field in edges.get(name, []):
                if color[target] == GRAY:
                    cycle_start = stack.index(target)
                    path = stack[cycle_start:] + [target]
                    message = graph[name]
                    self._error(
                        node,
                        message.line,
                        message.col,
                        "circular message dependency: " + " -> ".join(path),
                    )
                    return True
                if color[target] == WHITE and dfs(target, stack):
                    return True
            stack.pop()
            color[name] = BLACK
            return False

        for name in graph:
            if color[name] == WHITE:
                if dfs(name, []):
                    return  # 一个文件报一处环就够了，避免同一环被多条路径重复打印

    # ---------------- C-12 ----------------

    def _check_services(self, node: ast.FileNode) -> None:
        seen_services: dict[str, ast.ServiceDecl] = {}
        for service in node.services:
            if service.name in seen_services:
                self._error(
                    node,
                    service.line,
                    service.col,
                    'duplicate service name "{}"'.format(service.name),
                )
            else:
                seen_services[service.name] = service

            seen_methods: dict[str, ast.RpcDecl] = {}
            for rpc in service.methods:
                if rpc.name in seen_methods:
                    self._error(
                        node,
                        rpc.line,
                        rpc.col,
                        'duplicate rpc method "{}" in service {}'.format(
                            rpc.name, service.name
                        ),
                    )
                else:
                    seen_methods[rpc.name] = rpc

                for ref, role in ((rpc.request, "parameter"), (rpc.response, "return value")):
                    if ref.resolved_kind and ref.resolved_kind != ast.KIND_MESSAGE:
                        self._error(
                            node,
                            ref.line,
                            ref.col,
                            "rpc {} must be a message type (method {}::{} uses '{}')".format(
                                role, service.name, rpc.name, ref.name
                            ),
                        )

    # ---------------- C-10 / C-11 ----------------

    def _assign_method_ids(self, node: ast.FileNode) -> None:
        for service in node.services:
            service_full = service.full_name(node.package)
            for rpc in service.methods:
                rpc.full_name = methodid.full_name(node.package, service.name, rpc.name)
                if rpc.explicit_method_id is not None:
                    value = rpc.explicit_method_id
                    if value < 1 or value > MAX_METHOD_ID:
                        self._error(
                            node,
                            rpc.line,
                            rpc.col,
                            "method_id 0x{:X} is out of range 1..0x{:X} ({})".format(
                                value, MAX_METHOD_ID, rpc.full_name
                            ),
                        )
                        continue
                    rpc.method_id = value
                    # 覆盖值是**钉死的**：改方法名不会移动 id，将来也可能与别人撞上。
                    # 这不是错误（MethodInfo.method_id_overridden = true 让 L2 跳过
                    # 重算比对），但值得在生成日志里留一行，因为它是长期维护负担。
                    self._warning(
                        node,
                        rpc.line,
                        rpc.col,
                        "explicit method_id 0x{:08X} pins this method (derived id would be "
                        "0x{:08X}); MethodInfo.method_id_overridden = true makes the runtime "
                        "L2 check skip the recompute comparison, but id uniqueness is still "
                        "enforced across all input files".format(
                            value, methodid.method_id(rpc.full_name)
                        ),
                    )
                    self._register_method_id(
                        node, rpc, value, explicit=True, service_full=service_full
                    )
                else:
                    computed = methodid.method_id(rpc.full_name)
                    if computed == 0:
                        # 0 在 Dispatcher::Register 里是"未指定"的哨兵值，会被直接拒绝
                        # （见 server/dispatcher.h 与霍承钧的实现）。32 位空间里撞到 0 的
                        # 概率约 2^-32，但真撞上的表现是"启动期 Register 返回 false"，
                        # 排查成本远高于在这里报一行。
                        self._error(
                            node,
                            rpc.line,
                            rpc.col,
                            "method_id 0x00000000 for {} collides with the reserved "
                            '"unspecified" sentinel (Dispatcher::Register rejects id 0); '
                            "rename the method or set an explicit "
                            "[method_id = 0x..........] override".format(rpc.full_name),
                        )
                        rpc.method_id = 0
                        continue
                    rpc.method_id = computed
                    self._register_method_id(
                        node, rpc, rpc.method_id, explicit=False, service_full=service_full
                    )

    def _register_method_id(
        self,
        node: ast.FileNode,
        rpc: ast.RpcDecl,
        value: int,
        explicit: bool,
        service_full: str,
    ) -> None:
        previous = self._method_ids.get(value)
        if previous is None:
            self._method_ids[value] = (rpc.full_name, explicit)
            return
        other_name, other_explicit = previous
        if other_name == rpc.full_name:
            self._error(
                node,
                rpc.line,
                rpc.col,
                "method_id 0x{:08X} already used by {} (duplicate full_name)".format(
                    value, other_name
                ),
            )
            return
        if explicit or other_explicit:
            self._error(
                node,
                rpc.line,
                rpc.col,
                "method_id 0x{:08X} already used by {} (conflict with {})".format(
                    value, other_name, rpc.full_name
                ),
            )
        else:
            self._error(
                node,
                rpc.line,
                rpc.col,
                "method_id collision: {} and {} both map to 0x{:08X} "
                "(set [method_id = 0x...] on one of them to override)".format(
                    other_name, rpc.full_name, value
                ),
            )


# 标量类型 → 允许的默认值字面量种类。
_SCALAR_DEFAULT_KINDS = {
    "int32": (ast.LIT_INT,),
    "int64": (ast.LIT_INT,),
    "uint32": (ast.LIT_INT,),
    "uint64": (ast.LIT_INT,),
    "sint32": (ast.LIT_INT,),
    "sint64": (ast.LIT_INT,),
    "bool": (ast.LIT_BOOL,),
    "float": (ast.LIT_INT, ast.LIT_FLOAT),
    "double": (ast.LIT_INT, ast.LIT_FLOAT),
    "string": (ast.LIT_STRING,),
    "bytes": (ast.LIT_STRING,),
}

_INT_RANGES = {
    "int32": (-(2**31), 2**31 - 1),
    "sint32": (-(2**31), 2**31 - 1),
    "int64": (-(2**63), 2**63 - 1),
    "sint64": (-(2**63), 2**63 - 1),
    "uint32": (0, 2**32 - 1),
    "uint64": (0, 2**64 - 1),
}


def _int_range(scalar: str) -> tuple[int, int]:
    return _INT_RANGES[scalar]


def _default_matches_scalar(scalar: str, literal_kind: str) -> bool:
    return literal_kind in _SCALAR_DEFAULT_KINDS.get(scalar, ())


def _describe_field_shape(field: ast.Field) -> str:
    label = field.label or "singular"
    return "{} {}".format(label, field.type_ref.name)


def analyze(files: list[ast.FileNode], bag: DiagnosticBag) -> None:
    Analyzer(bag).analyze(files)
