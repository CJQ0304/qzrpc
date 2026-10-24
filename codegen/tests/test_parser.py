"""test_parser.py —— doc 04 §3 的每个 EBNF 产生式：一条正例 + 一条反例。

正例证明"合法的能过"，反例证明"非法的会报错**且报得准**"。
没有反例的语法测试是没有价值的：parser 最大的失败模式不是拒绝合法输入，
而是静默接受非法输入。
"""

from __future__ import annotations

import unittest

from _support import HEADER, analyze, errors, expect_no_error, expect_parse_error, parse

from qzcodegen import ast_nodes as ast
from qzcodegen.diagnostics import UnsupportedFeature


class TestLexer(unittest.TestCase):
    """doc 04 §2 的词法规则。"""

    def test_line_comment(self):
        node = parse(HEADER + "// 这是注释\nmessage A { int32 x = 1; }")
        self.assertEqual(node.messages[0].name, "A")

    def test_block_comment(self):
        node = parse(HEADER + "/* 块注释\n   跨行 */\nmessage A { int32 x = 1; }")
        self.assertEqual(node.messages[0].name, "A")

    def test_nested_block_comment_is_rejected(self):
        # 嵌套注释必须报错：C 系语言里它是"静默改变注释范围"的经典坑
        expect_parse_error(
            self, HEADER + "/* a /* b */ */\nmessage A {}", "nested block comments"
        )

    def test_unterminated_block_comment(self):
        expect_parse_error(self, HEADER + "/* 没有结束", "unterminated block comment")

    def test_string_escapes(self):
        node = parse(HEADER + 'message A { string s = 1 [default = "a\\n\\t\\\\\\"b"]; }')
        literal = node.messages[0].fields[0].default
        self.assertEqual(literal.value, 'a\n\t\\"b')

    def test_unsupported_escape(self):
        expect_parse_error(
            self, HEADER + 'message A { string s = 1 [default = "\\x41"]; }',
            "unsupported escape sequence",
        )

    def test_string_cannot_span_lines(self):
        expect_parse_error(
            self, HEADER + 'message A { string s = 1 [default = "a\nb"]; }',
            "line break in string literal",
        )

    def test_unterminated_string(self):
        expect_parse_error(
            self, HEADER + 'message A { string s = 1 [default = "abc]; }',
            "unterminated string literal",
        )

    def test_hex_and_decimal_integers(self):
        node = parse(
            HEADER
            + "enum E { ZERO = 0; A = 10; B = 0x10; }"
            + "message M { int32 x = 1; }"
        )
        values = {v.name: v.number for v in node.enums[0].values}
        self.assertEqual(values["A"], 10)
        self.assertEqual(values["B"], 16)

    def test_float_forms(self):
        node = parse(
            HEADER
            + "message M {\n"
            "  double a = 1 [default = 1.5];\n"
            "  double b = 2 [default = -2.5];\n"
            "  double c = 3 [default = 2e3];\n"
            "  double d = 4 [default = 1.5e-3];\n"
            "}"
        )
        defaults = [f.default.value for f in node.messages[0].fields]
        self.assertEqual(defaults, [1.5, -2.5, 2000.0, 0.0015])

    def test_invalid_numeric_literal(self):
        expect_parse_error(self, HEADER + "message A { int32 x = 3abc; }", "invalid numeric")

    def test_hex_without_digits(self):
        expect_parse_error(self, HEADER + "message A { int32 x = 0x; }", "hex literal")

    def test_unexpected_character(self):
        expect_parse_error(self, HEADER + "message A { int32 x = 1; } @", "unexpected character")

    def test_dotted_qualified_ident(self):
        node = parse('syntax = "qz1";\npackage a.b.c;\n')
        self.assertEqual(node.package, "a.b.c")


class TestTopLevel(unittest.TestCase):
    def test_syntax_declaration(self):
        node = parse('syntax = "qz1";\npackage demo;\n')
        self.assertEqual(node.syntax, "qz1")
        self.assertTrue(node.syntax_first)

    def test_syntax_must_be_string(self):
        expect_parse_error(self, "syntax = qz1;", "expected string syntax version")

    def test_duplicate_syntax(self):
        expect_parse_error(
            self, 'syntax = "qz1";\nsyntax = "qz1";', "duplicate syntax declaration"
        )

    def test_duplicate_package(self):
        expect_parse_error(
            self, HEADER + "package other;", "duplicate package declaration"
        )

    def test_empty_file_reports_missing_syntax(self):
        # 空文件本身是"合法的 token 流"，缺 syntax 属于语义层（C-01）
        node, bag = analyze("")
        self.assertIsNotNone(node)
        self.assertIn("unsupported syntax version", errors(bag)[0])

    def test_unknown_top_level_token(self):
        expect_parse_error(self, HEADER + "hello;", "expected a top-level declaration")

    def test_option_qzrpc_is_accepted(self):
        node = parse(HEADER + 'option qzrpc.default_timeout_ms = 3000;\nmessage A {}')
        self.assertEqual(node.options[0].name, "qzrpc.default_timeout_ms")

    def test_custom_option_is_rejected(self):
        expect_parse_error(
            self,
            HEADER + "option java_package = 1;",
            "custom option 'java_package' is not supported by qz v1",
        )


class TestUnsupportedFeatures(unittest.TestCase):
    """doc 04 §1：这些语法必须"认出即报错"，绝不静默忽略。"""

    def _expect_unsupported(self, text: str, feature: str):
        with self.assertRaises(UnsupportedFeature) as ctx:
            parse(HEADER + text)
        self.assertIn("not supported by qz v1", ctx.exception.message)
        self.assertIn(feature, ctx.exception.message)

    def test_import(self):
        self._expect_unsupported('import "other.qz";', "import")

    def test_extend(self):
        self._expect_unsupported("extend Foo { }", "extend")

    def test_oneof(self):
        self._expect_unsupported("message A { oneof x { int32 a = 1; } }", "oneof")

    def test_map(self):
        self._expect_unsupported("message A { map<string, int32> x = 1; }", "map")

    def test_group(self):
        self._expect_unsupported("message A { group G = 1 { } }", "group")

    def test_required(self):
        self._expect_unsupported("message A { required int32 x = 1; }", "required")

    def test_reserved(self):
        self._expect_unsupported("message A { reserved 2; }", "reserved")

    def test_import_reports_feature_and_position(self):
        with self.assertRaises(UnsupportedFeature) as ctx:
            parse('syntax = "qz1";\nimport "x.qz";')
        self.assertEqual(ctx.exception.line, 2)
        self.assertEqual(ctx.exception.col, 1)
        self.assertIn("import is not supported by qz v1", ctx.exception.message)


class TestEnum(unittest.TestCase):
    def test_basic_enum(self):
        node = parse(HEADER + "enum E { A = 0; B = 1; }")
        self.assertEqual([v.name for v in node.enums[0].values], ["A", "B"])

    def test_comma_separator_and_last_member_without_separator(self):
        node = parse(HEADER + "enum E { A = 0, B = 1, C = 2 }")
        self.assertEqual(len(node.enums[0].values), 3)

    def test_missing_separator(self):
        expect_parse_error(
            self, HEADER + "enum E { A = 0 B = 1; }", "expected ';' or ','"
        )

    def test_missing_equals(self):
        expect_parse_error(self, HEADER + "enum E { A 0; }", "expected '='")

    def test_unterminated_enum(self):
        expect_parse_error(self, HEADER + "enum E { A = 0;", "unterminated enum")

    def test_keyword_as_enum_name_is_rejected(self):
        expect_parse_error(self, HEADER + "enum message { A = 0; }", "reserved word")

    def test_enum_value_may_shadow_keyword(self):
        # enum 值名允许用关键字：那里没有语法歧义（idl 里常见 UNKNOWN/optional 之类的命名）
        node = parse(HEADER + "enum E { default = 0; }")
        self.assertEqual(node.enums[0].values[0].name, "default")


class TestMessage(unittest.TestCase):
    def test_basic_message(self):
        node = parse(HEADER + "message A { int32 x = 1; string y = 2; }")
        self.assertEqual([f.name for f in node.messages[0].fields], ["x", "y"])

    def test_empty_message(self):
        node = parse(HEADER + "message Empty {}")
        self.assertEqual(node.messages[0].fields, [])

    def test_nested_message_and_enum(self):
        node = parse(
            HEADER
            + "message Outer {\n"
            "  message Inner { int32 v = 1; }\n"
            "  enum Kind { A = 0; }\n"
            "  Inner child = 1;\n"
            "}"
        )
        self.assertEqual(node.messages[0].messages[0].name, "Inner")
        self.assertEqual(node.messages[0].enums[0].name, "Kind")
        self.assertEqual(node.messages[0].messages[0].parent_path, "Outer")

    def test_all_labels(self):
        node = parse(
            HEADER
            + "message A {\n"
            "  int32 a = 1;\n"
            "  optional int32 b = 2;\n"
            "  repeated int32 c = 3;\n"
            "}"
        )
        labels = [f.label for f in node.messages[0].fields]
        self.assertEqual(labels, [ast.LABEL_NONE, ast.LABEL_OPTIONAL, ast.LABEL_REPEATED])

    def test_field_attributes(self):
        node = parse(
            HEADER
            + "message A {\n"
            "  int32 a = 1 [default = 7, deprecated];\n"
            "  repeated int32 b = 2 [packed = false];\n"
            "  double c = 3 [default = 1.5];\n"
            "}"
        )
        a, b, c = node.messages[0].fields
        self.assertEqual(a.default.value, 7)
        self.assertTrue(a.deprecated)
        self.assertFalse(b.packed)
        self.assertEqual(c.default.value, 1.5)

    def test_qualified_type_reference(self):
        node = parse(HEADER + "message A { demo.Other other = 1; }")
        self.assertEqual(node.messages[0].fields[0].type_ref.name, "demo.Other")

    def test_field_missing_tag(self):
        expect_parse_error(self, HEADER + "message A { int32 x; }", "expected '='")

    def test_field_missing_semicolon(self):
        expect_parse_error(self, HEADER + "message A { int32 x = 1 }", "expected ';'")

    def test_field_missing_name(self):
        expect_parse_error(self, HEADER + "message A { int32 = 1; }", "expected field name")

    def test_unknown_attribute(self):
        expect_parse_error(
            self, HEADER + "message A { int32 x = 1 [weird = 2]; }", "expected field attribute"
        )

    def test_unterminated_message(self):
        expect_parse_error(self, HEADER + "message A { int32 x = 1;", "unterminated message")

    def test_service_inside_message_is_rejected(self):
        expect_parse_error(
            self, HEADER + "message A { service S {} }", "not allowed inside a message"
        )

    def test_duplicate_attribute(self):
        expect_parse_error(
            self,
            HEADER + "message A { int32 x = 1 [default = 1, default = 2]; }",
            "duplicate 'default' attribute",
        )


class TestService(unittest.TestCase):
    def test_basic_service(self):
        node = parse(
            HEADER
            + "message Q { int32 x = 1; }\n"
            + "message R { int32 y = 1; }\n"
            + "service S {\n"
            + "  rpc Do(Q) returns (R);\n"
            + "}"
        )
        self.assertEqual(node.services[0].name, "S")
        self.assertEqual(node.services[0].methods[0].name, "Do")
        self.assertEqual(node.services[0].methods[0].request.name, "Q")
        self.assertEqual(node.services[0].methods[0].response.name, "R")

    def test_rpc_attributes(self):
        node = parse(
            HEADER
            + "message Q { int32 x = 1; }\n"
            + "service S {\n"
            + "  rpc Do(Q) returns (Q) [idempotent = true, method_id = 0x1234, timeout_ms = 500];\n"
            + "}"
        )
        method = node.services[0].methods[0]
        self.assertTrue(method.idempotent)
        self.assertEqual(method.explicit_method_id, 0x1234)
        self.assertEqual(method.timeout_ms, 500)

    def test_missing_returns(self):
        expect_parse_error(
            self, HEADER + "service S { rpc Do(Q) (Q); }", "expected 'returns'"
        )

    def test_missing_parentheses(self):
        expect_parse_error(
            self, HEADER + "service S { rpc Do Q returns (Q); }", "expected '('"
        )

    def test_empty_service(self):
        node = parse(HEADER + "service S {}")
        self.assertEqual(node.services[0].methods, [])

    def test_unterminated_service(self):
        expect_parse_error(self, HEADER + "service S { rpc Do(Q) returns (Q);", "unterminated")

    def test_non_rpc_member_is_rejected(self):
        expect_parse_error(self, HEADER + "service S { int32 x = 1; }", "expected 'rpc'")

    def test_unknown_rpc_attribute(self):
        expect_parse_error(
            self,
            HEADER + "service S { rpc Do(Q) returns (Q) [packed = true]; }",
            "expected rpc attribute",
        )


class TestPositions(unittest.TestCase):
    """报错要指到准确的 file:line:col（编辑器跳转依赖它）。"""

    def test_error_position_is_one_based_and_exact(self):
        text = 'syntax = "qz1";\npackage demo;\nmessage A {\n  int32 x = 1\n}\n'
        try:
            parse(text, "pos.qz")
        except UnsupportedFeature:  # pragma: no cover - 不该走到这里
            self.fail("unexpected UnsupportedFeature")
        except Exception as exc:  # QzError
            self.assertEqual(exc.file, "pos.qz")
            # 缺分号是在读到下一行的 '}' 时才发现的，所以位置指向 '}'（第 5 行第 1 列）
            self.assertEqual(exc.line, 5)
            self.assertEqual(exc.col, 1)
            self.assertIn("expected ';'", exc.message)
        else:
            self.fail("期望解析失败")


class TestParseThenSemanticSmoke(unittest.TestCase):
    def test_full_example_parses(self):
        source = (
            'syntax = "qz1";\n'
            "package demo;\n"
            "\n"
            "enum SiteType {\n"
            "  UNKNOWN = 0;\n"
            "  BLOG    = 1;\n"
            "}\n"
            "\n"
            "message EchoBody {\n"
            "  string text   = 1;\n"
            "  int32  weight = 2;\n"
            "}\n"
            "\n"
            "message EchoRequest {\n"
            "  string          message = 1;\n"
            "  int32           repeat  = 2 [default = 1];\n"
            "  repeated int32  ids     = 3;\n"
            "  optional string lang    = 5;\n"
            "  EchoBody        body    = 6;\n"
            "  bytes           raw     = 7;\n"
            "  SiteType        site    = 8;\n"
            "}\n"
            "\n"
            "service EchoService {\n"
            "  rpc Echo(EchoRequest) returns (EchoRequest) [idempotent = true];\n"
            "  rpc Notify(EchoRequest) returns (EchoRequest);\n"
            "}\n"
        )
        node, bag = analyze(source)
        self.assertIsNotNone(node)
        expect_no_error(self, bag)
        self.assertEqual(errors(bag), [])
        self.assertEqual(len(node.services[0].methods), 2)


if __name__ == "__main__":
    unittest.main()
