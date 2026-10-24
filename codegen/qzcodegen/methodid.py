"""method_id 计算（doc 03 §7.3 的逐字实现）。

这里的三个常量与两个函数**必须**与 docs/03-wire-protocol.md §7.3 逐字一致：
C++ 侧 protocol::MethodIdOfName（src/protocol/protocol_const.h，内部调
StringUtil::Fnv1a64）与这里算出的值一旦不同，所有调用都会 NotFound——
而且症状是"运行期 404"，编译期毫无提示。所以这个文件不做任何"优化"。
"""

from __future__ import annotations

FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
MASK64 = 0xFFFFFFFFFFFFFFFF
MASK32 = 0xFFFFFFFF


def fnv1a64(s: str) -> int:
    """FNV-1a 64 位哈希（输入按 UTF-8 取字节）。"""
    h = FNV_OFFSET
    for b in s.encode("utf-8"):
        h ^= b
        h = (h * FNV_PRIME) & MASK64
    return h


def method_id(full_name: str) -> int:
    """method_id = FNV-1a64(full_name) & 0xFFFFFFFF。"""
    return fnv1a64(full_name) & MASK32


def full_name(package: str, service: str, method: str) -> str:
    """拼出 full_name："<pkg>.<Svc>.<Method>"，无 package 时 "Svc.Method"（无前导点）。"""
    if package:
        return "{}.{}.{}".format(package, service, method)
    return "{}.{}".format(service, method)


def format_id(value: int) -> str:
    """渲染成生成代码里用的 C++ 字面量（大写十六进制 + u 后缀）。"""
    return "0x{:08X}U".format(value & MASK32)


def format_id_comment(value: int) -> str:
    """渲染成注释里的十六进制（无后缀）。"""
    return "0x{:08X}".format(value & MASK32)
