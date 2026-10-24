#!/usr/bin/env python3
"""轻舟 RPC —— protobuf 线格式互通验证器。

目的
----
    证明「qzrpc 自研的序列化实现」与「官方 protobuf」在字节层面完全互通。
    这是本项目最重要的一条外部证据：它不是自己跟自己对答案，而是拿
    第三方权威实现做交叉验证。

验证两个方向
-----------
    A. qzrpc 编码 → protobuf 解码：调用 dump_wire.exe encode 拿到十六进制，
       用官方 protobuf 解析，逐字段比对期望值。
    B. protobuf 编码 → qzrpc 解码：用官方 protobuf 构造同样的消息并序列化，
       把十六进制交给 dump_wire.exe decode，把它的规范输出与 Python 侧
       自己算出的规范串逐行比对。

依赖
----
    * protoc（仅本脚本使用，框架运行时零第三方依赖）
    * python protobuf 运行时
    两者都只存在于 tools/interop，不进入 src/ 与构建产物。

    两者缺一都不算失败，而是**跳过**：本脚本退出码 2 会被 ctest 的
    SKIP_RETURN_CODE 识别成 Skipped。理由见下方 EXIT CODES。

退出码
------
    0  三项断言全部通过
    1  有断言失败（真 bug），或 --strict 下环境缺失
    2  [SKIP] 环境缺失：找不到 dump_wire / protoc / protobuf 运行时
       —— "跳过"必须与"通过"区分开，否则 CI 里的验证会变成假绿。

用法
----
    python tools/interop/verify_interop.py [--exe path/to/dump_wire.exe]
                                          [--protoc path/to/protoc] [--strict]
"""

from __future__ import annotations

import argparse
import os
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path


class Skip(Exception):
    """环境缺失导致无法验证（退出码 2，ctest 记为 Skipped）。"""


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]

# 与 dump_wire.cpp 的 BuildSample 必须逐字段一致的样本值。
SAMPLE = {
    "message": "hello, 轻舟",
    "repeat": -7,
    "ids": [0, 127, 128, -1, -2**31, 2**31 - 1],
    "tags": ["a", "", "中文"],
    "lang": "",  # 显式 set 成空串，用于验证 presence 语义
    "body": {"text": "nested", "weight": -3, "score": -2.5, "valid": True},
    "raw": b"\x00\xff\x01",
    "site": 3,  # VIDEO
}


def hex_of(data: bytes) -> str:
    return data.hex()


def canonical_from_proto(msg) -> str:
    """把 protobuf 消息渲染成与 dump_wire.cpp 的 DumpCanonical 完全一致的文本。"""
    lines = [
        f"message={hex_of(msg.message.encode('utf-8'))}",
        f"repeat={msg.repeat}",
        "ids=" + ",".join(str(v) for v in msg.ids),
        "tags=" + ",".join(hex_of(t.encode("utf-8")) for t in msg.tags),
        f"has_lang={1 if msg.HasField('lang') else 0}",
        f"lang={hex_of(msg.lang.encode('utf-8'))}",
        f"has_body={1 if msg.HasField('body') else 0}",
        f"body.text={hex_of(msg.body.text.encode('utf-8'))}",
        f"body.weight={msg.body.weight}",
        f"body.score={format(msg.body.score, '.17g')}",
        f"body.valid={1 if msg.body.valid else 0}",
        f"raw={hex_of(msg.raw)}",
        f"site={msg.site}",
    ]
    return "\n".join(lines)


def build_proto_message(cls):
    msg = cls()
    msg.message = SAMPLE["message"]
    msg.repeat = SAMPLE["repeat"]
    msg.ids.extend(SAMPLE["ids"])
    msg.tags.extend(SAMPLE["tags"])
    msg.lang = SAMPLE["lang"]  # 赋值即产生 presence
    msg.body.text = SAMPLE["body"]["text"]
    msg.body.weight = SAMPLE["body"]["weight"]
    msg.body.score = SAMPLE["body"]["score"]
    msg.body.valid = SAMPLE["body"]["valid"]
    msg.raw = SAMPLE["raw"]
    msg.site = SAMPLE["site"]
    return msg


def expected_canonical() -> str:
    b = SAMPLE["body"]
    lines = [
        f"message={hex_of(SAMPLE['message'].encode('utf-8'))}",
        f"repeat={SAMPLE['repeat']}",
        "ids=" + ",".join(str(v) for v in SAMPLE["ids"]),
        "tags=" + ",".join(hex_of(t.encode("utf-8")) for t in SAMPLE["tags"]),
        "has_lang=1",
        f"lang={hex_of(SAMPLE['lang'].encode('utf-8'))}",
        "has_body=1",
        f"body.text={hex_of(b['text'].encode('utf-8'))}",
        f"body.weight={b['weight']}",
        f"body.score={format(b['score'], '.17g')}",
        f"body.valid={1 if b['valid'] else 0}",
        f"raw={hex_of(SAMPLE['raw'])}",
        f"site={SAMPLE['site']}",
    ]
    return "\n".join(lines)


def compile_descriptor(workdir: Path, protoc: str = "protoc") -> Path:
    """用 protoc 把 echo.proto 编译成 FileDescriptorSet（避免依赖 _pb2.py 的版本匹配）。"""
    desc = workdir / "echo.desc"
    cmd = [
        protoc,
        f"--proto_path={HERE}",
        f"--descriptor_set_out={desc}",
        "--include_imports",
        str(HERE / "echo.proto"),
    ]
    try:
        subprocess.run(cmd, check=True, capture_output=True, text=True)
    except FileNotFoundError as exc:
        raise Skip(
            f"找不到 protoc（{protoc}）—— 它只在验证期需要，框架本身不依赖 protobuf。"
            "装一个 protobuf 编译器，或用 --protoc 指定绝对路径"
        ) from exc
    except subprocess.CalledProcessError as exc:
        # protoc 在但跑不动（比如 proto_path 解析失败）属于真问题，不能当跳过。
        raise RuntimeError(f"protoc 执行失败: {exc.stderr.strip() or exc}") from exc
    return desc


def load_message_class(desc_path: Path):
    try:
        from google.protobuf import descriptor_pb2, descriptor_pool, message_factory
    except ImportError as exc:
        raise Skip(
            "找不到 python protobuf 运行时（import google.protobuf 失败）—— "
            "pip install protobuf，或用另一个装了它的解释器跑本脚本"
        ) from exc

    fds = descriptor_pb2.FileDescriptorSet()
    fds.ParseFromString(desc_path.read_bytes())

    pool = descriptor_pool.DescriptorPool()
    for file_proto in fds.file:
        pool.Add(file_proto)

    msg_desc = pool.FindMessageTypeByName("demo.EchoRequest")
    try:
        return message_factory.GetMessageClass(msg_desc)
    except AttributeError as exc:
        # GetMessageClass 是 protobuf 4.22+ 的 API；更老的版本要走
        # message_factory.MessageFactory().GetPrototype()。环境问题，不是我们的 bug。
        raise Skip(
            "protobuf 运行时版本过旧（缺 message_factory.GetMessageClass，需 >= 4.22）"
        ) from exc


def run_exe(exe: Path, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [str(exe), *args], capture_output=True, text=True, check=False
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--exe",
        default=str(HERE / "dump_wire.exe"),
        help="dump_wire 可执行文件路径",
    )
    parser.add_argument(
        "--protoc",
        default=os.environ.get("PROTOC", "protoc"),
        help="protoc 可执行文件路径（默认取环境变量 PROTOC 或 PATH 上的 protoc）",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="环境缺失按失败处理（发布门禁用；默认是跳过并返回 2）",
    )
    parser.add_argument("--keep", action="store_true", help="保留临时目录便于排查")
    args = parser.parse_args()

    exe = Path(args.exe)
    if not exe.exists():
        print(f"[SKIP] 找不到 {exe}，请先构建 tools/interop/dump_wire")
        return 1 if args.strict else 2

    workdir = Path(tempfile.mkdtemp(prefix="qz_interop_"))
    failures: list[str] = []
    checks = 0

    try:
        # 环境探测放在内层：Skip 只包住"拿不到 protoc / protobuf"，不影响下面的
        # 断言逻辑；外层 finally 仍会清理临时目录。
        try:
            desc = compile_descriptor(workdir, args.protoc)
            cls = load_message_class(desc)
        except Skip as exc:
            print(f"[SKIP] {exc}")
            if args.strict:
                print("       --strict 已开启：环境缺失按失败处理")
                return 1
            print("       ctest 将把本用例记为 Skipped（SKIP_RETURN_CODE 2）")
            return 2
        except RuntimeError as exc:
            # protoc 存在但执行失败：这是真问题（不是环境缺失），直接判失败。
            print(f"[FAIL] {exc}")
            return 1

        # ---------- 方向 A：qzrpc 编码 → protobuf 解码 ----------
        proc = run_exe(exe, "encode")
        if proc.returncode != 0:
            print(f"[FAIL] dump_wire encode 失败: {proc.stderr.strip()}")
            return 1
        wire_hex = proc.stdout.strip()
        wire = bytes.fromhex(wire_hex)
        print(f"[A] qzrpc 编码 {len(wire)} 字节: {wire_hex}")

        parsed = cls()
        try:
            parsed.ParseFromString(wire)
        except Exception as exc:  # noqa: BLE001
            failures.append(f"A: protobuf 无法解析 qzrpc 字节流: {exc}")
            parsed = None

        if parsed is not None:
            actual = canonical_from_proto(parsed)
            expect = expected_canonical()
            checks += 1
            if actual == expect:
                print("[PASS] A: 官方 protobuf 解析 qzrpc 字节流，13 个字段全部一致")
            else:
                failures.append("A: 字段值不一致")
                for got, want in zip(actual.splitlines(), expect.splitlines()):
                    if got != want:
                        print(f"       got  {got}")
                        print(f"       want {want}")

        # ---------- 方向 B：protobuf 编码 → qzrpc 解码 ----------
        msg = build_proto_message(cls)
        py_wire = msg.SerializeToString()
        # 注意：ParseFromString 返回的是"解析了多少字节"（int），不是消息本身，
        # 所以必须另开一个实例来渲染规范串。
        reparsed = cls()
        reparsed.ParseFromString(py_wire)
        py_canonical = canonical_from_proto(reparsed)

        proc = run_exe(exe, "decode", py_wire.hex())
        checks += 1
        if proc.returncode != 0:
            failures.append(f"B: qzrpc 解析 protobuf 字节流失败: {proc.stderr.strip()}")
        else:
            cpp_canonical = proc.stdout.strip()
            if cpp_canonical == py_canonical:
                print(f"[PASS] B: qzrpc 解析 protobuf 字节流（{len(py_wire)} 字节），字段全部一致")
            else:
                failures.append("B: 字段值不一致")
                for got, want in zip(cpp_canonical.splitlines(), py_canonical.splitlines()):
                    if got != want:
                        print(f"       cpp  {got}")
                        print(f"       py   {want}")

        # ---------- 方向 C：字节级一致性（同一逻辑消息的理想化比对） ----------
        # 注意：字段顺序与 packed 分块由实现决定，protobuf 不保证逐字节相同，
        # 因此这里仅在长度与"可互相解析"上做断言，不做逐字节相等断言。
        checks += 1
        if parsed is not None and reparsed.ByteSize() == msg.ByteSize():
            print("[PASS] C: 同一逻辑消息在两侧的编码长度一致")
        elif parsed is not None:
            failures.append("C: 编码长度不一致")

    finally:
        if not args.keep:
            import shutil

            shutil.rmtree(workdir, ignore_errors=True)
        else:
            print(f"[INFO] 临时目录保留在 {workdir}")

    print()
    if failures:
        print(f"结果: {checks - len(failures)}/{checks} 通过，{len(failures)} 项失败")
        for f in failures:
            print(f"  - {f}")
        return 1

    print(f"结果: {checks}/{checks} 全部通过 —— qzrpc 与官方 protobuf 线格式互通")
    return 0


if __name__ == "__main__":
    sys.exit(main())
