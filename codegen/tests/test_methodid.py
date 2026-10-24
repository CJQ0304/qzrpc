"""test_methodid.py —— 本次交付里最关键的一个测试。

为什么说它"生死线"：Python 侧算出的 method_id 与 C++ 侧 `protocol::MethodIdOfName`
只要有一位不同，**所有调用都会在运行期变成 NotFound**，而编译、单测、启动日志
全都正常。这类 bug 极难在运行期被发现，所以必须用静态对照把它钉死。

三层验证：
  1. doc 03 §7.3 / §10 给的两个真实值（Echo / Notify）；
  2. 由 **真实的 C++ 实现**（src/common/string_util.cpp + protocol_const.h，
     用 MSVC 编译运行）打印出来的 14 条对照表；
  3. 150 条随机构造的 full_name 与一份**独立写法**的 FNV-1a64 交叉验证。
"""

from __future__ import annotations

import random
import unittest

import _support  # noqa: F401  (负责 sys.path)

from qzcodegen import methodid


# ---------------------------------------------------------------------------
# 由真实 C++ 实现生成的对照表（不是用 Python 算的）
#
#   编译命令：cl /std:c++17 /I src .scratch/midtable.cpp src/common/string_util.cpp
#   程序内容：对每个名字打印 qzrpc::protocol::MethodIdOfName(name)
#
# 前两条同时出现在 docs/03-wire-protocol.md §7.3 与 §10，属于"文档给定值"。
# ---------------------------------------------------------------------------
CPP_REFERENCE = [
    ("demo.EchoService.Echo", 0x21678893),
    ("demo.EchoService.Notify", 0x9AF18CAF),
    ("demo.EchoService.SlowEcho", 0x0F89F4AA),
    ("demo.RegistryService.Register", 0x8C73587D),
    ("demo.RegistryService.Heartbeat", 0x7959992C),
    ("demo.user.UserService.GetUserById", 0x97C8AC8E),
    ("demo.user.UserService.ListUsers", 0xC85EB421),
    ("svc.A.Method", 0xE45545CF),
    ("ping.PingService.Ping", 0x5949119A),
    ("demo.EchoService.Echo2", 0xC2ED2993),
    ("demo.EchoService.echo", 0x1FF714B3),
    ("a.b.C.D", 0x09E4AE25),
    # 非 ASCII：验证两侧都按 UTF-8 取字节，而不是按宽字符/本地代码页
    ("服务.EchoService.Echo", 0xDE2FA499),
    ("x", 0x86021707),
]

MASK64 = 0xFFFFFFFFFFFFFFFF


def independent_fnv1a64(text: str) -> int:
    """独立的第二实现：取字节 → 累积 XOR/乘法，用取模而不是掩码。

    刻意写成与 methodid.fnv1a64 不同的形状（取模 vs 位与），这样"两处都写错
    同一个地方"的概率才足够低。
    """
    value = 0xCBF29CE484222325
    for byte in text.encode("utf-8"):
        value = ((value ^ byte) * 0x100000001B3) % (2**64)
    return value


class TestDocumentedValues(unittest.TestCase):
    """doc 03 §7.3 的实现逐字对照 + §10 的两个真实值。"""

    def test_echo_matches_doc(self):
        self.assertEqual(methodid.method_id("demo.EchoService.Echo"), 0x21678893)

    def test_notify_matches_doc(self):
        self.assertEqual(methodid.method_id("demo.EchoService.Notify"), 0x9AF18CAF)

    def test_fnv_constants_match_doc(self):
        self.assertEqual(methodid.FNV_OFFSET, 0xCBF29CE484222325)
        self.assertEqual(methodid.FNV_PRIME, 0x100000001B3)
        self.assertEqual(methodid.MASK64, 0xFFFFFFFFFFFFFFFF)

    def test_offset_basis_is_hash_of_empty_string(self):
        # FNV 的定义性质：空串的哈希就是 offset basis。
        self.assertEqual(methodid.fnv1a64(""), 0xCBF29CE484222325)


class TestCppCrossValidation(unittest.TestCase):
    """与真实 C++ 实现打印出来的值逐条对照。"""

    def test_against_cpp_table(self):
        for full_name, expected in CPP_REFERENCE:
            with self.subTest(full_name=full_name):
                self.assertEqual(
                    methodid.method_id(full_name),
                    expected,
                    "{} 的 method_id 与 C++ 侧不一致（Python=0x{:08X} C++=0x{:08X}）".format(
                        full_name, methodid.method_id(full_name), expected
                    ),
                )

    def test_table_size_is_meaningful(self):
        # 防止有人"修测试"时把表删空
        self.assertGreaterEqual(len(CPP_REFERENCE), 10)

    def test_all_ids_fit_in_32_bits(self):
        for full_name, expected in CPP_REFERENCE:
            self.assertLessEqual(expected, 0xFFFFFFFF)
            self.assertGreaterEqual(expected, 0)


class TestRandomCrossValidation(unittest.TestCase):
    """150 条随机 full_name：与独立实现交叉验证（这是"随机"覆盖，不是抽样）。"""

    ALPHABET = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_"

    def _random_full_name(self, rng: random.Random) -> str:
        parts = rng.randint(1, 4)
        out = []
        for _ in range(parts):
            length = rng.randint(1, 12)
            out.append("".join(rng.choice(self.ALPHABET) for _ in range(length)))
        return ".".join(out)

    def test_150_random_names(self):
        rng = random.Random(20240501)  # 固定种子：失败可复现
        for _ in range(150):
            name = self._random_full_name(rng)
            with self.subTest(name=name):
                self.assertEqual(
                    methodid.fnv1a64(name), independent_fnv1a64(name)
                )
                self.assertEqual(
                    methodid.method_id(name),
                    independent_fnv1a64(name) & 0xFFFFFFFF,
                )

    def test_random_names_include_unicode(self):
        rng = random.Random(7)
        for _ in range(20):
            name = "".join(
                rng.choice("服务轻舟接口ABCDE") for _ in range(rng.randint(1, 6))
            ) + ".Svc.M"
            with self.subTest(name=name):
                self.assertEqual(methodid.fnv1a64(name), independent_fnv1a64(name))


class TestProperties(unittest.TestCase):
    def test_method_id_is_low_32_bits_of_fnv(self):
        for full_name, _ in CPP_REFERENCE[:5]:
            self.assertEqual(
                methodid.method_id(full_name),
                methodid.fnv1a64(full_name) & 0xFFFFFFFF,
            )

    def test_full_name_with_package(self):
        self.assertEqual(
            methodid.full_name("demo", "EchoService", "Echo"), "demo.EchoService.Echo"
        )

    def test_full_name_without_package_has_no_leading_dot(self):
        self.assertEqual(methodid.full_name("", "EchoService", "Echo"), "EchoService.Echo")

    def test_package_changes_the_id(self):
        self.assertNotEqual(
            methodid.method_id("demo.EchoService.Echo"),
            methodid.method_id("EchoService.Echo"),
        )

    def test_case_matters(self):
        # 大小写不同必须是不同的 id（IDL 里的原始大小写参与哈希）
        self.assertNotEqual(
            methodid.method_id("demo.EchoService.Echo"),
            methodid.method_id("demo.EchoService.echo"),
        )

    def test_format_id_is_cxx_literal(self):
        self.assertEqual(methodid.format_id(0x21678893), "0x21678893U")
        self.assertEqual(methodid.format_id_comment(0x21678893), "0x21678893")

    def test_generated_literal_parses_back(self):
        # 生成物里写的是 0x...U，这里确认它可以被还原成同一个整数
        for _, expected in CPP_REFERENCE:
            literal = methodid.format_id(expected)
            self.assertTrue(literal.endswith("U"))
            self.assertEqual(int(literal[:-1], 16), expected)

    def test_pure_function_is_stable(self):
        first = methodid.method_id("demo.EchoService.Echo")
        for _ in range(5):
            self.assertEqual(methodid.method_id("demo.EchoService.Echo"), first)


if __name__ == "__main__":
    unittest.main()
