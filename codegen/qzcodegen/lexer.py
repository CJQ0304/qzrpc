"""手写词法扫描（G-02）。

为什么不用正则一把梭：报错要精确到列，而 `re` 只能告诉你"没匹配上"，
不能告诉你"你在这一行的第 12 列写了个非法转义"。手写扫描器多 120 行，
换来的是**每个错误都能定位到字符**。

规范见 doc 04 §2。
"""

from __future__ import annotations

from dataclasses import dataclass

from .diagnostics import QzError

# ---- token 种类 ----
IDENT = "ident"
INT = "int"
FLOAT = "float"
STRING = "string"
PUNCT = "punct"
EOF = "eof"

# 标量类型名（doc 04 §3 的 scalar_type）。
SCALAR_TYPES = (
    "int32",
    "int64",
    "uint32",
    "uint64",
    "sint32",
    "sint64",
    "bool",
    "float",
    "double",
    "string",
    "bytes",
)

# 全部保留字：关键字不能当消息名/字段名/服务名（doc 04 §2 末段）。
KEYWORDS = frozenset(
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
    )
    + SCALAR_TYPES
)

# v1 明确不支持：认出即报错，绝不静默忽略（doc 04 §1）。
UNSUPPORTED_KEYWORDS = frozenset(
    ("import", "extend", "oneof", "map", "group", "required", "reserved")
)

# 单字符标点（doc 04 §2：{ } ( ) [ ] = ; , .）
# `<` `>` 不在 doc 的标点表里，这里收进来只为了一件事：让 `map<string, int32>`
# 能通过词法层，从而在语法层得到真正有用的报错 "map is not supported by qz v1"，
# 而不是含糊的 "unexpected character '<'"。其它位置出现它们仍然会被 parser 拒绝。
PUNCT_CHARS = frozenset("{}()[]=;.,<>")

# 字符串里允许的转义（doc 04 §2：不支持 \xNN、不支持跨行）。
ESCAPES = {
    "\\": "\\",
    '"': '"',
    "n": "\n",
    "t": "\t",
    "r": "\r",
    "0": "\0",
}

_DIGITS = "0123456789"
_HEX_DIGITS = "0123456789abcdefABCDEF"


@dataclass(frozen=True)
class Token:
    kind: str
    text: str  # 源码原文（报错时回显用）
    value: object  # 解码后的值：int/float/str，punct/ident 与 text 相同
    line: int
    col: int

    def __str__(self) -> str:  # 报错消息里拼 "遇到什么"
        if self.kind == EOF:
            return "end of file"
        return repr(self.text)

    def is_keyword(self, word: str) -> bool:
        return self.kind == IDENT and self.text == word

    def is_punct(self, char: str) -> bool:
        return self.kind == PUNCT and self.text == char


class Lexer:
    def __init__(self, source: str, file: str = "<input>") -> None:
        self.src = source
        self.file = file
        self.pos = 0
        self.line = 1
        self.col = 1

    # ---- 位置推进 ----
    def _advance(self, count: int = 1) -> None:
        for _ in range(count):
            if self.pos >= len(self.src):
                return
            if self.src[self.pos] == "\n":
                self.line += 1
                self.col = 1
            else:
                self.col += 1
            self.pos += 1

    def _peek(self, offset: int = 0) -> str:
        index = self.pos + offset
        return self.src[index] if index < len(self.src) else ""

    def _error(self, message: str, line: int = None, col: int = None) -> QzError:
        return QzError(
            self.file,
            self.line if line is None else line,
            self.col if col is None else col,
            message,
        )

    # ---- 主循环 ----
    def tokenize(self) -> list[Token]:
        tokens: list[Token] = []
        while True:
            self._skip_trivia()
            if self.pos >= len(self.src):
                tokens.append(Token(EOF, "", None, self.line, self.col))
                return tokens
            tokens.append(self._next_token())

    def _skip_trivia(self) -> None:
        while self.pos < len(self.src):
            ch = self.src[self.pos]
            if ch in " \t\r\n":
                self._advance()
                continue
            if ch == "/" and self._peek(1) == "/":
                while self.pos < len(self.src) and self.src[self.pos] != "\n":
                    self._advance()
                continue
            if ch == "/" and self._peek(1) == "*":
                self._skip_block_comment()
                continue
            return

    def _skip_block_comment(self) -> None:
        start_line, start_col = self.line, self.col
        self._advance(2)  # 吃掉 "/*"
        while True:
            if self.pos >= len(self.src):
                raise self._error(
                    "unterminated block comment", start_line, start_col
                )
            if self.src[self.pos] == "*" and self._peek(1) == "/":
                self._advance(2)
                return
            if self.src[self.pos] == "/" and self._peek(1) == "*":
                # 嵌套注释必须报错：C 系语言里它是"静默改变注释范围"的经典坑。
                raise self._error(
                    "nested block comments are not supported by qz v1",
                    self.line,
                    self.col,
                )
            self._advance()

    def _next_token(self) -> Token:
        line, col = self.line, self.col
        ch = self.src[self.pos]

        if ch == '"':
            return self._read_string(line, col)
        if ch in _DIGITS:
            return self._read_number(line, col)
        if ch in "+-" and self._peek(1) in _DIGITS:
            # 允许 `default = -1` 这种写法。语法里没有算术表达式，因此不会歧义。
            return self._read_number(line, col)
        if ch.isalpha() or ch == "_":
            return self._read_ident(line, col)
        if ch in PUNCT_CHARS:
            self._advance()
            return Token(PUNCT, ch, ch, line, col)

        raise self._error("unexpected character {!r}".format(ch))

    def _read_ident(self, line: int, col: int) -> Token:
        start = self.pos
        while True:
            ch = self._peek()
            if ch and (ch.isalnum() or ch == "_"):
                self._advance()
            else:
                break
        text = self.src[start : self.pos]
        return Token(IDENT, text, text, line, col)

    def _read_number(self, line: int, col: int) -> Token:
        start = self.pos
        sign = 1
        if self._peek() in "+-":
            if self._peek() == "-":
                sign = -1
            self._advance()
        # 符号已经吃掉，数值部分从这里开始。**不要再对含符号的整段文本做
        # int()/float()**——那会把符号算两次（-2.5 变成 +2.5，这是个真实 bug）。
        digits_start = self.pos

        if self._peek() == "0" and self._peek(1) in "xX":
            self._advance(2)
            hex_start = self.pos
            while self._peek() and self._peek() in _HEX_DIGITS:
                self._advance()
            if self.pos == hex_start:
                raise self._error("hex literal has no digits", line, col)
            if self._peek() in "+-":
                raise self._error("sign is not allowed in hex literal", line, col)
            text = self.src[start : self.pos]
            return Token(INT, text, sign * int(self.src[hex_start : self.pos], 16), line, col)

        is_float = False
        while self._peek() and self._peek() in _DIGITS:
            self._advance()
        if self._peek() == "." and self._peek(1) and self._peek(1) in _DIGITS:
            is_float = True
            self._advance()
            while self._peek() and self._peek() in _DIGITS:
                self._advance()
        if self._peek() in "eE":
            nxt = self._peek(1)
            nxt2 = self._peek(2)
            looks_like_exp = (nxt and nxt in _DIGITS) or (nxt in "+-" and nxt2 and nxt2 in _DIGITS)
            if looks_like_exp:
                is_float = True
                self._advance()
                if self._peek() in "+-":
                    self._advance()
                while self._peek() and self._peek() in _DIGITS:
                    self._advance()

        text = self.src[start : self.pos]
        digits = self.src[digits_start : self.pos]
        trailing = self._peek()
        if trailing and (trailing.isalnum() or trailing == "_"):
            raise self._error(
                "invalid numeric literal {!r}".format(
                    self.src[start : self.pos + 1]
                ),
                line,
                col,
            )
        if is_float:
            return Token(FLOAT, text, sign * float(digits), line, col)
        return Token(INT, text, sign * int(digits, 10), line, col)

    def _read_string(self, line: int, col: int) -> Token:
        start = self.pos
        self._advance()  # 开引号
        out: list[str] = []
        while True:
            ch = self._peek()
            if ch == "":
                raise self._error("unterminated string literal", line, col)
            if ch == "\n":
                raise self._error(
                    "line break in string literal is not allowed", line, col
                )
            if ch == '"':
                self._advance()
                break
            if ch == "\\":
                esc = self._peek(1)
                if esc not in ESCAPES:
                    raise self._error(
                        "unsupported escape sequence \\{!r} in string literal".format(esc),
                        self.line,
                        self.col,
                    )
                out.append(ESCAPES[esc])
                self._advance(2)
                continue
            out.append(ch)
            self._advance()
        text = self.src[start : self.pos]
        return Token(STRING, text, "".join(out), line, col)


def tokenize(source: str, file: str = "<input>") -> list[Token]:
    return Lexer(source, file).tokenize()
