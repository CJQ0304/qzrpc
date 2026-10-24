"""test_semantic.py —— doc 04 §3.1 的 C-01 … C-12 逐条一个用例。

每条约束至少一个"违反"用例；能自然构造的都加了"满足"用例。
message_id 冲突（C-10/C-11）两条必须在**同一批输入内跨方法/跨文件**检测，
所以这里专门测了多文件场景。
"""

from __future__ import annotations

import unittest
from unittest import mock

from _support import HEADER, analyze, errors, expect_error, expect_no_error, warnings

from qzcodegen import methodid


def _msgs(*fields: str) -> str:
    return HEADER + "message M {\n" + "".join("  {}\n".format(f) for f in fields) + "}\n"


class TestC01Syntax(unittest.TestCase):
    def test_missing_syntax(self):
        _, bag = analyze("package demo;\nmessage A {}\n")
        expect_error(self, bag, "unsupported syntax version")

    def test_wrong_version(self):
        _, bag = analyze('syntax = "qz2";\npackage demo;\n')
        expect_error(self, bag, 'unsupported syntax version "qz2"')

    def test_syntax_not_first(self):
        _, bag = analyze('package demo;\nsyntax = "qz1";\n')
        expect_error(self, bag, "must be the first statement")

    def test_valid_syntax(self):
        _, bag = analyze(HEADER + "message A { int32 x = 1; }\n")
        expect_no_error(self, bag)

    def test_missing_package_is_only_a_warning(self):
        # 单文件 IDL 完全可能没有 package（full_name 退化成 Svc.Method）
        _, bag = analyze('syntax = "qz1";\nmessage A {}\n')
        expect_no_error(self, bag)
        self.assertTrue(any("no `package` declaration" in w for w in warnings(bag)))


class TestC02EnumStartsAtZero(unittest.TestCase):
    def test_first_value_not_zero(self):
        _, bag = analyze(HEADER + "enum E { A = 1; }\n")
        expect_error(self, bag, "enum E must start at 0")

    def test_empty_enum(self):
        _, bag = analyze(HEADER + "enum E {}\n")
        expect_error(self, bag, "enum E must start at 0")

    def test_duplicate_value_number(self):
        _, bag = analyze(HEADER + "enum E { A = 0; B = 0; }\n")
        expect_error(self, bag, "duplicate enum value 0 in enum E")

    def test_duplicate_value_name(self):
        _, bag = analyze(HEADER + "enum E { A = 0; A = 1; }\n")
        expect_error(self, bag, 'duplicate enum value name "A"')

    def test_zero_first_is_fine(self):
        _, bag = analyze(HEADER + "enum E { Z = 0; A = 5; B = 9; }\n")
        expect_no_error(self, bag)


class TestC03FieldTag(unittest.TestCase):
    def test_tag_zero(self):
        _, bag = analyze(_msgs("int32 x = 0;"))
        expect_error(self, bag, "out of range 1..536870911")

    def test_tag_too_large(self):
        _, bag = analyze(_msgs("int32 x = 536870912;"))
        expect_error(self, bag, "out of range 1..536870911")

    def test_max_allowed_tag(self):
        _, bag = analyze(_msgs("int32 x = 536870911;"))
        expect_no_error(self, bag)

    def test_duplicate_tag(self):
        _, bag = analyze(_msgs("int32 x = 3;", "int32 y = 3;"))
        expect_error(self, bag, 'duplicate field tag 3 in message M (first used by "x")')

    def test_duplicate_field_name(self):
        _, bag = analyze(_msgs("int32 x = 1;", "int32 x = 2;"))
        expect_error(self, bag, 'duplicate field name "x" in message M')


class TestC04ReservedTags(unittest.TestCase):
    def test_lower_bound(self):
        _, bag = analyze(_msgs("int32 x = 19000;"))
        expect_error(self, bag, "tag 19000-19999 are reserved")

    def test_upper_bound(self):
        _, bag = analyze(_msgs("int32 x = 19999;"))
        expect_error(self, bag, "tag 19000-19999 are reserved")

    def test_just_outside_the_range(self):
        _, bag = analyze(_msgs("int32 a = 18999;", "int32 b = 20000;"))
        expect_no_error(self, bag)


class TestC05UnknownType(unittest.TestCase):
    def test_unknown_field_type(self):
        _, bag = analyze(_msgs("Foo x = 1;"))
        expect_error(self, bag, 'unknown type "Foo"')

    def test_unknown_type_in_nested_message(self):
        _, bag = analyze(HEADER + "message A { message B { Nope n = 1; } }\n")
        expect_error(self, bag, 'unknown type "Nope"')

    def test_unknown_rpc_type(self):
        _, bag = analyze(HEADER + "service S { rpc Do(Nope) returns (Nope); }\n")
        expect_error(self, bag, 'unknown type "Nope"')

    def test_scalar_and_declared_types_resolve(self):
        _, bag = analyze(
            HEADER
            + "enum E { Z = 0; }\n"
            + "message Inner { int32 v = 1; }\n"
            + "message Outer { Inner i = 1; E e = 2; string s = 3; }\n"
        )
        expect_no_error(self, bag)

    def test_qualified_self_reference_resolves(self):
        _, bag = analyze(HEADER + "message A { demo.A child = 1; }\n")
        # 自引用不是"未知类型"，而是循环依赖（C-06），所以这里必须是 C-06 的错
        expect_error(self, bag, "circular message dependency")


class TestC06CircularDependency(unittest.TestCase):
    def test_two_message_cycle(self):
        _, bag = analyze(
            HEADER
            + "message A { B b = 1; }\n"
            + "message B { A a = 1; }\n"
        )
        expect_error(self, bag, "circular message dependency")

    def test_self_reference(self):
        _, bag = analyze(HEADER + "message A { A self = 1; }\n")
        expect_error(self, bag, "circular message dependency")

    def test_three_message_cycle(self):
        _, bag = analyze(
            HEADER
            + "message A { B b = 1; }\n"
            + "message B { C c = 1; }\n"
            + "message C { A a = 1; }\n"
        )
        expect_error(self, bag, "circular message dependency")

    def test_diamond_is_not_a_cycle(self):
        # A -> B, A -> C, B -> D, C -> D：不是环，必须放行
        _, bag = analyze(
            HEADER
            + "message D { int32 v = 1; }\n"
            + "message B { D d = 1; }\n"
            + "message C { D d = 1; }\n"
            + "message A { B b = 1; C c = 2; }\n"
        )
        expect_no_error(self, bag)


class TestC07Packed(unittest.TestCase):
    def test_packed_on_singular_field(self):
        _, bag = analyze(_msgs("int32 x = 1 [packed = true];"))
        expect_error(self, bag, "packed is only allowed on repeated numeric fields")

    def test_packed_on_repeated_string(self):
        _, bag = analyze(_msgs("repeated string s = 1 [packed = true];"))
        expect_error(self, bag, "packed is only allowed on repeated numeric fields")

    def test_packed_on_repeated_message(self):
        _, bag = analyze(
            HEADER + "message Inner { int32 v = 1; }\n"
            + "message M { repeated Inner xs = 1 [packed = true]; }\n"
        )
        expect_error(self, bag, "packed is only allowed on repeated numeric fields")

    def test_packed_on_repeated_numeric_is_fine(self):
        _, bag = analyze(_msgs("repeated int32 xs = 1 [packed = true];"))
        expect_no_error(self, bag)

    def test_packed_false_on_repeated_numeric_is_fine(self):
        _, bag = analyze(_msgs("repeated double xs = 1 [packed = false];"))
        expect_no_error(self, bag)

    def test_packed_allowed_on_repeated_enum(self):
        _, bag = analyze(
            HEADER
            + "enum E { Z = 0; A = 1; }\n"
            + "message M { repeated E xs = 1 [packed = true]; }\n"
        )
        expect_no_error(self, bag)


class TestC08DefaultOnRepeated(unittest.TestCase):
    def test_repeated_with_default(self):
        _, bag = analyze(_msgs("repeated int32 xs = 1 [default = 1];"))
        expect_error(self, bag, "repeated field cannot have default")

    def test_singular_with_default(self):
        _, bag = analyze(_msgs("int32 x = 1 [default = 1];"))
        expect_no_error(self, bag)


class TestC09DefaultTypeMismatch(unittest.TestCase):
    def test_string_field_with_int_default(self):
        _, bag = analyze(_msgs("string s = 1 [default = 3];"))
        expect_error(self, bag, "default value type mismatch")

    def test_int_field_with_string_default(self):
        _, bag = analyze(_msgs('int32 x = 1 [default = "hi"];'))
        expect_error(self, bag, "default value type mismatch")

    def test_bool_field_with_int_default(self):
        _, bag = analyze(_msgs("bool b = 1 [default = 1];"))
        expect_error(self, bag, "default value type mismatch")

    def test_int_out_of_range(self):
        _, bag = analyze(_msgs("int32 x = 1 [default = 3000000000];"))
        expect_error(self, bag, "does not fit in int32")

    def test_enum_default_not_a_member(self):
        _, bag = analyze(
            HEADER
            + "enum E { Z = 0; A = 1; }\n"
            + "message M { E e = 1 [default = 7]; }\n"
        )
        expect_error(self, bag, "is not a value of enum E")

    def test_message_field_cannot_have_default(self):
        _, bag = analyze(
            HEADER
            + "message Inner { int32 v = 1; }\n"
            + "message M { Inner i = 1 [default = 1]; }\n"
        )
        expect_error(self, bag, "cannot have a default")

    def test_float_field_accepts_int_default(self):
        _, bag = analyze(_msgs("double d = 1 [default = 2];"))
        expect_no_error(self, bag)

    def test_uint64_accepts_large_default(self):
        _, bag = analyze(_msgs("uint64 u = 1 [default = 18446744073709551615];"))
        expect_no_error(self, bag)


def _service_with_method_id(value: int) -> str:
    """构造一个单方法服务，并把它的 method_id 显式设成 value。"""
    return (
        HEADER
        + "message Q { int32 x = 1; }\n"
        + "service S {\n  rpc A(Q) returns (Q) [method_id = __ID__];\n}\n"
    ).replace("__ID__", str(value))


class TestC10ExplicitMethodId(unittest.TestCase):
    def test_zero_is_illegal(self):
        _, bag = analyze(_service_with_method_id(0))
        expect_error(self, bag, "out of range 1..0xFFFFFFFF")

    def test_negative_is_illegal(self):
        _, bag = analyze(_service_with_method_id(-5))
        expect_error(self, bag, "out of range 1..0xFFFFFFFF")

    def test_too_large_is_illegal(self):
        _, bag = analyze(_service_with_method_id(0x100000000))
        expect_error(self, bag, "out of range 1..0xFFFFFFFF")

    def test_max_is_legal(self):
        _, bag = analyze(_service_with_method_id(0xFFFFFFFF))
        expect_no_error(self, bag)

    def test_override_warns_about_pinned_id(self):
        # (a) 方案落地后：MethodInfo.method_id_overridden = true 让 L2 跳过重算比对，
        # 所以覆盖不再是"半个能用的逃生口"。但覆盖仍是长期维护负担（改方法名不会
        # 移动 id），生成期要留一行提示，且**不能**再说"会被判成 codegen bug"。
        _, bag = analyze(_service_with_method_id(0x1234))
        expect_no_error(self, bag)  # 覆盖本身不是错误
        messages = warnings(bag)
        self.assertTrue(
            any("method_id_overridden" in m for m in messages),
            "warning 应说明 method_id_overridden 的作用；实际：{}".format(messages),
        )
        self.assertTrue(
            any("pins this method" in m for m in messages),
            "warning 应说明 id 被钉死；实际：{}".format(messages),
        )
        self.assertFalse(
            any("codegen bug" in m for m in messages),
            "warning 不能再威胁说会被判成 codegen bug；实际：{}".format(messages),
        )

    def test_two_explicit_ids_with_different_names_still_collide(self):
        """覆盖**不是**绕过冲突检测的后门（team-lead 点名的边界）。

        两个方法都显式指定同一个 method_id，即使双方都"合法覆盖"，
        也必须在 L1 报错——否则 override 就成了删掉冲突检测的开关。
        """
        source = (
            HEADER
            + "message Q { int32 x = 1; }\n"
            + "service S {\n"
            + "  rpc A(Q) returns (Q) [method_id = 0x4242];\n"
            + "  rpc B(Q) returns (Q) [method_id = 0x4242];\n"
            + "}\n"
        )
        _, bag = analyze(source)
        expect_error(self, bag, "method_id 0x00004242 already used by demo.S.A")

    def test_explicit_override_does_not_hide_cross_file_collision(self):
        """两个**不同文件**、都用覆盖值、id 相同 → 仍报错（C-10 的跨文件形态）。"""
        from qzcodegen import parser as qzparser
        from qzcodegen import semantic
        from qzcodegen.diagnostics import DiagnosticBag

        file_one = (
            HEADER
            + "message Q { int32 x = 1; }\n"
            + "service S { rpc A(Q) returns (Q) [method_id = 0x9999]; }\n"
        )
        file_two = (
            HEADER
            + "message R { int32 x = 1; }\n"
            + "service T { rpc B(R) returns (R) [method_id = 0x9999]; }\n"
        )
        bag = DiagnosticBag()
        nodes = [
            qzparser.parse_source(file_one, "a.qz"),
            qzparser.parse_source(file_two, "b.qz"),
        ]
        semantic.analyze(nodes, bag)
        expect_error(self, bag, "method_id 0x00009999 already used by demo.S.A")

    def test_two_explicit_ids_collide(self):
        source = (
            HEADER
            + "message Q { int32 x = 1; }\n"
            + "service S {\n"
            + "  rpc A(Q) returns (Q) [method_id = 0x1234];\n"
            + "  rpc B(Q) returns (Q) [method_id = 0x1234];\n"
            + "}\n"
        )
        _, bag = analyze(source)
        expect_error(self, bag, "method_id 0x00001234 already used by demo.S.A")

    def test_explicit_collides_with_computed(self):
        # 显式值正好等于另一个方法自动计算出来的值：这是最阴的一类冲突
        computed = methodid.method_id("demo.S.B")
        source = (
            HEADER
            + "message Q { int32 x = 1; }\n"
            + "service S {\n"
            + "  rpc A(Q) returns (Q) [method_id = __ID__];\n"
            + "  rpc B(Q) returns (Q);\n"
            + "}\n"
        ).replace("__ID__", "0x{:X}".format(computed))
        _, bag = analyze(source)
        expect_error(self, bag, "already used by")

    def test_cross_file_collision(self):
        # C-10/C-11 的 L1 层是"一次输入的全部文件一起看"
        computed = methodid.method_id("demo.S.A")
        file_one = HEADER + "message Q { int32 x = 1; }\nservice S { rpc A(Q) returns (Q); }\n"
        file_two = (
            HEADER
            + "message R { int32 x = 1; }\n"
            + "service T { rpc B(R) returns (R) [method_id = __ID__]; }\n"
        ).replace("__ID__", "0x{:X}".format(computed))

        from qzcodegen import parser as qzparser
        from qzcodegen import semantic
        from qzcodegen.diagnostics import DiagnosticBag

        bag = DiagnosticBag()
        nodes = [
            qzparser.parse_source(file_one, "a.qz"),
            qzparser.parse_source(file_two, "b.qz"),
        ]
        semantic.analyze(nodes, bag)
        expect_error(self, bag, "already used by demo.S.A")


class TestC11ComputedCollision(unittest.TestCase):
    def test_two_names_mapping_to_same_id(self):
        source = (
            HEADER
            + "message Q { int32 x = 1; }\n"
            + "service S {\n"
            + "  rpc A(Q) returns (Q);\n"
            + "  rpc B(Q) returns (Q);\n"
            + "}\n"
        )
        with mock.patch.object(methodid, "method_id", return_value=0xDEADBEEF):
            _, bag = analyze(source)
        expect_error(self, bag, "method_id collision")
        expect_error(self, bag, "0xDEADBEEF")

    def test_computed_id_zero_is_rejected(self):
        # 0 是 Dispatcher::Register 的"未指定"哨兵，算出来是 0 必须编译期就报
        source = (
            HEADER
            + "message Q { int32 x = 1; }\n"
            + "service S { rpc A(Q) returns (Q); }\n"
        )
        with mock.patch.object(methodid, "method_id", return_value=0):
            _, bag = analyze(source)
        expect_error(self, bag, "reserved")
        expect_error(self, bag, "rename the method")
        # 逃生口必须写进错误信息里，否则用户只知道"改名"这一条路
        expect_error(self, bag, "[method_id = 0x..........] override")

    def test_no_collision_normally(self):
        source = (
            HEADER
            + "message Q { int32 x = 1; }\n"
            + "service S {\n"
            + "  rpc A(Q) returns (Q);\n"
            + "  rpc B(Q) returns (Q);\n"
            + "}\n"
        )
        _, bag = analyze(source)
        expect_no_error(self, bag)


class TestC12RpcTypesMustBeMessages(unittest.TestCase):
    def test_scalar_parameter(self):
        _, bag = analyze(HEADER + "service S { rpc Do(int32) returns (int32); }\n")
        expect_error(self, bag, "rpc parameter must be a message type")

    def test_enum_parameter(self):
        _, bag = analyze(
            HEADER + "enum E { Z = 0; }\nservice S { rpc Do(E) returns (E); }\n"
        )
        expect_error(self, bag, "rpc parameter must be a message type")

    def test_message_types_are_fine(self):
        _, bag = analyze(
            HEADER
            + "message Q { int32 x = 1; }\n"
            + "service S { rpc Do(Q) returns (Q); }\n"
        )
        expect_no_error(self, bag)


class TestStructuralChecks(unittest.TestCase):
    def test_duplicate_service_name(self):
        _, bag = analyze(
            HEADER
            + "message Q { int32 x = 1; }\n"
            + "service S { rpc A(Q) returns (Q); }\n"
            + "service S { rpc B(Q) returns (Q); }\n"
        )
        expect_error(self, bag, 'duplicate service name "S"')

    def test_duplicate_rpc_name(self):
        _, bag = analyze(
            HEADER
            + "message Q { int32 x = 1; }\n"
            + "service S { rpc A(Q) returns (Q); rpc A(Q) returns (Q); }\n"
        )
        expect_error(self, bag, 'duplicate rpc method "A" in service S')

    def test_duplicate_type_name_after_flattening(self):
        _, bag = analyze(
            HEADER
            + "message A { message Inner { int32 v = 1; } }\n"
            + "message Inner { int32 v = 1; }\n"
        )
        expect_error(self, bag, 'duplicate type name "Inner"')

    def test_nested_message_full_name(self):
        node, bag = analyze(
            HEADER + "message Outer { message Inner { int32 v = 1; } Inner i = 1; }\n"
        )
        expect_no_error(self, bag)
        inner = node.iter_messages()[1]
        self.assertEqual(inner.full_name("demo"), "demo.Outer.Inner")
        self.assertEqual(node.messages[0].fields[0].type_ref.resolved_name, "demo.Outer.Inner")


class TestValidPrograms(unittest.TestCase):
    def test_all_labels_and_types(self):
        source = (
            HEADER
            + "enum E { Z = 0; A = 1; }\n"
            + "message Inner { int32 v = 1; }\n"
            + "message M {\n"
            + "  int32 i32 = 1;\n"
            + "  int64 i64 = 2;\n"
            + "  uint32 u32 = 3;\n"
            + "  uint64 u64 = 4;\n"
            + "  sint32 s32 = 5;\n"
            + "  sint64 s64 = 6;\n"
            + "  bool b = 7;\n"
            + "  float f = 8;\n"
            + "  double d = 9;\n"
            + "  string s = 10;\n"
            + "  bytes by = 11;\n"
            + "  E e = 12;\n"
            + "  Inner m = 13;\n"
            + "  repeated int32 ri = 14;\n"
            + "  repeated string rs = 15;\n"
            + "  repeated E re = 16;\n"
            + "  repeated Inner rm = 17;\n"
            + "  optional int32 oi = 18;\n"
            + "  optional string os = 19;\n"
            + "}\n"
        )
        _, bag = analyze(source)
        expect_no_error(self, bag)

    def test_no_package_service(self):
        _, bag = analyze(
            'syntax = "qz1";\nmessage Q { int32 x = 1; }\nservice S { rpc Do(Q) returns (Q); }\n'
        )
        expect_no_error(self, bag)


if __name__ == "__main__":
    unittest.main()
