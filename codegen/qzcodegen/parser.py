"""递归下降 parser（G-04）。

严格按 doc 04 §3 的 EBNF。设计上的两条硬规则：

1. 遇到 v1 不支持的语法（import / extend / oneof / map / group / required /
   reserved / 自定义 option）**立刻抛 UnsupportedFeature**，错误消息里带
   "<feature> is not supported by qz v1"。静默忽略是最坏的选择——用户会以为
   配置生效了。
2. 任何语法错误都带 file:line:col。不能只报"语法错误"。
"""

from __future__ import annotations

from . import ast_nodes as ast
from .diagnostics import QzError, UnsupportedFeature
from .lexer import (
    EOF,
    FLOAT,
    IDENT,
    INT,
    PUNCT,
    SCALAR_TYPES,
    STRING,
    UNSUPPORTED_KEYWORDS,
    Token,
    tokenize,
)

# 顶层声明关键字。
_TOP_LEVEL = ("syntax", "package", "option", "enum", "message", "service")

# qzrpc 自己保留的 option 前缀；其它前缀一律视为"自定义 option"。
_QZRPC_OPTION_PREFIX = "qzrpc."


class Parser:
    def __init__(self, tokens: list[Token], display_path: str, abs_path: str) -> None:
        self.tokens = tokens
        self.pos = 0
        self.file = display_path
        self.node = ast.FileNode(display_path=display_path, abs_path=abs_path)
        self._top_count = 0

    # ---------------- token 辅助 ----------------

    def _peek(self, offset: int = 0) -> Token:
        index = self.pos + offset
        if index >= len(self.tokens):
            return self.tokens[-1]  # EOF
        return self.tokens[index]

    def _next(self) -> Token:
        tok = self._peek()
        self.pos += 1
        return tok

    def _at_eof(self) -> bool:
        return self._peek().kind == EOF

    def _error(self, message: str, tok: Token = None) -> QzError:
        tok = tok or self._peek()
        return QzError(self.file, tok.line, tok.col, message)

    def _expect_punct(self, char: str) -> Token:
        tok = self._peek()
        if tok.kind == PUNCT and tok.text == char:
            return self._next()
        raise self._error("expected '{}', got {}".format(char, tok))

    def _optional_punct(self, char: str) -> bool:
        tok = self._peek()
        if tok.kind == PUNCT and tok.text == char:
            self._next()
            return True
        return False

    def _expect_keyword(self, word: str) -> Token:
        tok = self._peek()
        if tok.kind == IDENT and tok.text == word:
            return self._next()
        raise self._error("expected '{}', got {}".format(word, tok))

    def _optional_keyword(self, word: str) -> bool:
        tok = self._peek()
        if tok.kind == IDENT and tok.text == word:
            self._next()
            return True
        return False

    def _expect_name(self, what: str) -> Token:
        """取一个"名字"：标识符，且不能是保留字（doc 04 §2 末段）。"""
        tok = self._peek()
        if tok.kind != IDENT:
            raise self._error("expected {}, got {}".format(what, tok))
        if tok.text in SCALAR_TYPES or tok.text in _RESERVED:
            raise self._error(
                "keyword '{}' cannot be used as {} (reserved word)".format(tok.text, what)
            )
        return self._next()

    def _expect_ident_any(self, what: str) -> Token:
        """取一个标识符，**允许**是保留字。

        为什么允许：`string message = 1;` 这种写法在 idl/echo.qz 与 doc 04 §9 的
        完整示例里都出现了。在"类型已解析、下一个 token 只能是名字"的位置上，
        关键字没有任何歧义，硬拦下来只会让黄金输入无法解析。真正需要保留字
        隔离的是**声明名**（message/enum/service），那些仍走 _expect_name。
        """
        tok = self._peek()
        if tok.kind != IDENT:
            raise self._error("expected {}, got {}".format(what, tok))
        return self._next()

    def _expect_int(self, what: str) -> Token:
        tok = self._peek()
        if tok.kind != INT:
            raise self._error("expected integer {}, got {}".format(what, tok))
        return self._next()

    def _expect_string(self, what: str) -> Token:
        tok = self._peek()
        if tok.kind != STRING:
            raise self._error("expected string {}, got {}".format(what, tok))
        return self._next()

    def _expect_bool(self, what: str) -> Token:
        tok = self._peek()
        if tok.kind == IDENT and tok.text in ("true", "false"):
            return self._next()
        raise self._error("expected boolean literal for {}, got {}".format(what, tok))

    def _qualified_ident(self, what: str) -> tuple[str, Token]:
        first = self._expect_name(what)
        parts = [first.text]
        while self._optional_punct("."):
            parts.append(self._expect_name(what).text)
        return ".".join(parts), first

    def _literal(self) -> ast.Literal:
        tok = self._peek()
        if tok.kind == INT:
            self._next()
            return ast.Literal(ast.LIT_INT, tok.value, tok.text, tok.line, tok.col)
        if tok.kind == FLOAT:
            self._next()
            return ast.Literal(ast.LIT_FLOAT, tok.value, tok.text, tok.line, tok.col)
        if tok.kind == STRING:
            self._next()
            return ast.Literal(ast.LIT_STRING, tok.value, tok.text, tok.line, tok.col)
        if tok.kind == IDENT and tok.text in ("true", "false"):
            self._next()
            return ast.Literal(
                ast.LIT_BOOL, tok.text == "true", tok.text, tok.line, tok.col
            )
        raise self._error("expected literal, got {}".format(tok))

    # ---------------- 入口 ----------------

    def parse(self) -> ast.FileNode:
        while not self._at_eof():
            self._top_level()
        return self.node

    def _top_level(self) -> None:
        tok = self._peek()
        text = tok.text if tok.kind == IDENT else ""
        first = self._top_count == 0

        if text in UNSUPPORTED_KEYWORDS:
            raise UnsupportedFeature(
                self.file,
                tok.line,
                tok.col,
                _unsupported_message(text, inside="top-level"),
            )

        if text == "syntax":
            self._syntax(first)
        elif text == "package":
            self._package()
        elif text == "option":
            self._option()
        elif text == "enum":
            self.node.enums.append(self._enum(parent_path=""))
        elif text == "message":
            self.node.messages.append(self._message(parent_path=""))
        elif text == "service":
            self.node.services.append(self._service())
        elif tok.kind == IDENT and text in _RESERVED:
            raise self._error(
                "unexpected keyword '{}' at top level; expected one of: {}".format(
                    text, ", ".join(_TOP_LEVEL)
                )
            )
        else:
            raise self._error(
                "expected a top-level declaration ({}), got {}".format(
                    ", ".join(_TOP_LEVEL), tok
                )
            )
        self._top_count += 1

    # ---------------- 顶层声明 ----------------

    def _syntax(self, first: bool) -> None:
        start = self._expect_keyword("syntax")
        self._expect_punct("=")
        value = self._expect_string("syntax version")
        self._expect_punct(";")
        if self.node.syntax is not None:
            raise self._error("duplicate syntax declaration", start)
        self.node.syntax = value.value
        self.node.syntax_line = start.line
        self.node.syntax_col = start.col
        self.node.syntax_first = first

    def _package(self) -> None:
        start = self._expect_keyword("package")
        name, _ = self._qualified_ident("package name")
        self._expect_punct(";")
        if self.node.package:
            raise self._error("duplicate package declaration", start)
        self.node.package = name
        self.node.package_line = start.line
        self.node.package_col = start.col

    def _option(self) -> None:
        start = self._expect_keyword("option")
        name, name_tok = self._qualified_ident("option name")
        self._expect_punct("=")
        value = self._literal()
        self._expect_punct(";")
        if not name.startswith(_QZRPC_OPTION_PREFIX):
            raise UnsupportedFeature(
                self.file,
                name_tok.line,
                name_tok.col,
                "custom option '{}' is not supported by qz v1 "
                "(only the qzrpc.* family is accepted)".format(name),
            )
        self.node.options.append(ast.OptionDecl(name, value, start.line, start.col))

    # ---------------- 枚举 ----------------

    def _enum(self, parent_path: str) -> ast.EnumDecl:
        self._expect_keyword("enum")
        name_tok = self._expect_name("enum name")
        decl = ast.EnumDecl(name_tok.text, [], name_tok.line, name_tok.col, parent_path)
        self._expect_punct("{")
        while not self._peek().is_punct("}"):
            if self._at_eof():
                raise self._error("unterminated enum '{}'".format(name_tok.text))
            value_tok = self._peek()
            if value_tok.kind == IDENT and value_tok.text in UNSUPPORTED_KEYWORDS:
                raise UnsupportedFeature(
                    self.file,
                    value_tok.line,
                    value_tok.col,
                    _unsupported_message(value_tok.text, inside="enum"),
                )
            value_name = self._expect_ident_any("enum value name")
            self._expect_punct("=")
            number_tok = self._expect_int("enum value")
            if number_tok.value < 0:
                raise self._error(
                    "negative enum values are not supported by qz v1 "
                    "(enum '{}' value '{}')".format(name_tok.text, value_name.text),
                    number_tok,
                )
            decl.values.append(
                ast.EnumValue(value_name.text, number_tok.value, value_name.line, value_name.col)
            )
            if not (self._optional_punct(";") or self._optional_punct(",")):
                if not self._peek().is_punct("}"):
                    raise self._error("expected ';' or ',' after enum value")
        self._expect_punct("}")
        return decl

    # ---------------- 消息 ----------------

    def _message(self, parent_path: str) -> ast.MessageDecl:
        self._expect_keyword("message")
        name_tok = self._expect_name("message name")
        decl = ast.MessageDecl(name_tok.text, name_tok.line, name_tok.col)
        decl.parent_path = parent_path
        child_prefix = "{}.".format(parent_path) if parent_path else ""
        self._expect_punct("{")
        while not self._peek().is_punct("}"):
            if self._at_eof():
                raise self._error("unterminated message '{}'".format(name_tok.text))
            tok = self._peek()
            text = tok.text if tok.kind == IDENT else ""
            if text in UNSUPPORTED_KEYWORDS:
                raise UnsupportedFeature(
                    self.file,
                    tok.line,
                    tok.col,
                    _unsupported_message(text, inside="message"),
                )
            if text == "message":
                decl.messages.append(
                    self._message(parent_path=child_prefix + name_tok.text)
                )
            elif text == "enum":
                decl.enums.append(
                    self._enum(parent_path=child_prefix + name_tok.text)
                )
            elif text == "service":
                raise self._error("service declaration is not allowed inside a message")
            else:
                decl.fields.append(self._field())
        self._expect_punct("}")
        return decl

    def _field(self) -> ast.Field:
        label = ast.LABEL_NONE
        tok = self._peek()
        if tok.kind == IDENT and tok.text in ("optional", "repeated"):
            label = tok.text
            self._next()
        type_ref = self._type_ref()
        name_tok = self._expect_ident_any("field name")
        self._expect_punct("=")
        tag_tok = self._expect_int("field tag")
        field = ast.Field(
            name_tok.text, type_ref, tag_tok.value, name_tok.line, name_tok.col, label=label
        )
        if self._optional_punct("["):
            self._attr_list(field)
        self._expect_punct(";")
        return field

    def _attr_list(self, field: ast.Field) -> None:
        while True:
            tok = self._peek()
            text = tok.text if tok.kind == IDENT else ""
            if text == "default":
                self._next()
                self._expect_punct("=")
                if field.default is not None:
                    raise self._error("duplicate 'default' attribute", tok)
                field.default = self._literal()
            elif text == "packed":
                self._next()
                self._expect_punct("=")
                value = self._expect_bool("packed")
                if field.packed is not None:
                    raise self._error("duplicate 'packed' attribute", tok)
                field.packed = value.text == "true"
            elif text == "deprecated":
                self._next()
                field.deprecated = True
            elif text in UNSUPPORTED_KEYWORDS:
                raise UnsupportedFeature(
                    self.file,
                    tok.line,
                    tok.col,
                    _unsupported_message(text, inside="field attribute"),
                )
            else:
                raise self._error(
                    "expected field attribute (default / packed / deprecated), got {}".format(
                        tok
                    )
                )
            if not self._optional_punct(","):
                break
        self._expect_punct("]")

    def _type_ref(self) -> ast.TypeRef:
        tok = self._peek()
        if tok.kind == IDENT and tok.text in SCALAR_TYPES:
            self._next()
            return ast.TypeRef(tok.text, tok.line, tok.col)
        name, first = self._qualified_ident("type name")
        return ast.TypeRef(name, first.line, first.col)

    # ---------------- 服务 ----------------

    def _service(self) -> ast.ServiceDecl:
        self._expect_keyword("service")
        name_tok = self._expect_name("service name")
        decl = ast.ServiceDecl(name_tok.text, [], name_tok.line, name_tok.col)
        self._expect_punct("{")
        while not self._peek().is_punct("}"):
            if self._at_eof():
                raise self._error("unterminated service '{}'".format(name_tok.text))
            tok = self._peek()
            if not tok.is_keyword("rpc"):
                if tok.kind == IDENT and tok.text in UNSUPPORTED_KEYWORDS:
                    raise UnsupportedFeature(
                        self.file,
                        tok.line,
                        tok.col,
                        _unsupported_message(tok.text, inside="service"),
                    )
                raise self._error("expected 'rpc' in service body, got {}".format(tok))
            decl.methods.append(self._rpc())
        self._expect_punct("}")
        return decl

    def _rpc(self) -> ast.RpcDecl:
        self._expect_keyword("rpc")
        name_tok = self._expect_ident_any("rpc method name")
        self._expect_punct("(")
        request = self._type_ref()
        self._expect_punct(")")
        self._expect_keyword("returns")
        self._expect_punct("(")
        response = self._type_ref()
        self._expect_punct(")")
        rpc = ast.RpcDecl(name_tok.text, request, response, name_tok.line, name_tok.col)
        if self._optional_punct("["):
            self._rpc_attr_list(rpc)
        self._expect_punct(";")
        return rpc

    def _rpc_attr_list(self, rpc: ast.RpcDecl) -> None:
        while True:
            tok = self._peek()
            text = tok.text if tok.kind == IDENT else ""
            if text == "idempotent":
                self._next()
                self._expect_punct("=")
                value = self._expect_bool("idempotent")
                rpc.idempotent = value.text == "true"
            elif text == "method_id":
                self._next()
                self._expect_punct("=")
                number = self._expect_int("method_id")
                if rpc.explicit_method_id is not None:
                    raise self._error("duplicate 'method_id' attribute", tok)
                rpc.explicit_method_id = number.value
            elif text == "timeout_ms":
                self._next()
                self._expect_punct("=")
                number = self._expect_int("timeout_ms")
                rpc.timeout_ms = number.value
            elif text in UNSUPPORTED_KEYWORDS:
                raise UnsupportedFeature(
                    self.file,
                    tok.line,
                    tok.col,
                    _unsupported_message(text, inside="rpc attribute"),
                )
            else:
                raise self._error(
                    "expected rpc attribute (idempotent / method_id / timeout_ms), got {}".format(
                        tok
                    )
                )
            if not self._optional_punct(","):
                break
        self._expect_punct("]")


# 全部保留字（关键字 + 标量类型名）。用于"名字不能是保留字"的检查。
_RESERVED = frozenset(
    (
        "syntax",
        "package",
        "import",
        "option",
        "enum",
        "message",
        "service",
        "rpc",
        "returns",
        "optional",
        "repeated",
        "default",
        "packed",
        "deprecated",
        "idempotent",
        "method_id",
        "timeout_ms",
        "true",
        "false",
        "extend",
        "oneof",
        "map",
        "group",
        "required",
        "reserved",
    )
) | frozenset(SCALAR_TYPES)


def _unsupported_message(feature: str, inside: str) -> str:
    if feature == "import":
        return "import is not supported by qz v1 (planned for v2)"
    return "{} is not supported by qz v1 (found in {})".format(feature, inside)


def parse_source(source: str, display_path: str, abs_path: str = "") -> ast.FileNode:
    """词法 + 语法分析。失败抛 QzError / UnsupportedFeature。"""
    tokens = tokenize(source, display_path)
    return Parser(tokens, display_path, abs_path or display_path).parse()
