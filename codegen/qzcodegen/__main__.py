"""命令行入口（G-09）。

    # 生成
    python -m qzcodegen --input idl/echo.qz --out generated --cpp
    # 只做语法/语义检查（CI 用）
    python -m qzcodegen --input idl/echo.qz --check
    # 一次校验多个文件，做全局 method_id 冲突检测（L1 层）
    python -m qzcodegen --input "idl/*.qz" --check

为什么 `--check` 与 `--cpp` 分开：CI 只想知道"IDL 还合法吗"，
不应该在 check 阶段产生任何文件写入（否则 CI 工作区会脏）。

退出码：0 全部通过；1 存在 error（诊断写到 stderr）。
"""

from __future__ import annotations

import argparse
import datetime
import glob
import os
import sys

from . import (
    GENERATOR_VERSION,
    PROTOCOL_VERSION,
    DiagnosticBag,
    compile_files,
    write_outputs,
)


def _expand_inputs(patterns: list[str]) -> list[str]:
    """展开 glob。

    Windows 的 cmd.exe 不会替我们展开 `idl/*.qz`（那是 bash 的行为），
    所以这里两种来源都要支持：shell 展开过的字面路径 + 未展开的模式。
    """
    out: list[str] = []
    for pattern in patterns:
        if any(ch in pattern for ch in "*?["):
            matched = sorted(glob.glob(pattern))
            if matched:
                out.extend(matched)
                continue
        out.append(pattern)
    # 去重但保持顺序
    seen: set[str] = set()
    unique: list[str] = []
    for path in out:
        key = os.path.abspath(path)
        if key in seen:
            continue
        seen.add(key)
        unique.append(path)
    return unique


def _timestamp_from_env(spec: str | None) -> str:
    """`--timestamp` 优先；否则看 SOURCE_DATE_EPOCH（可复现构建的通用约定）。"""
    if spec:
        return spec
    epoch = os.environ.get("SOURCE_DATE_EPOCH")
    if epoch:
        try:
            moment = datetime.datetime.fromtimestamp(int(epoch), datetime.timezone.utc)
            return moment.strftime("%Y-%m-%dT%H:%M:%SZ")
        except (ValueError, OSError, OverflowError):
            return "<invalid SOURCE_DATE_EPOCH>"
    return ""


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="qzcodegen",
        description="轻舟 RPC (Qingzhou RPC) 的 .qz IDL 代码生成器",
        epilog="退出码：0 成功；1 有错误。",
    )
    parser.add_argument(
        "-i",
        "--input",
        nargs="+",
        required=True,
        metavar="FILE",
        help="输入 .qz 文件（可多个，支持 glob；多个文件会一起做 method_id 冲突检测）",
    )
    parser.add_argument(
        "-o", "--out", default="generated", metavar="DIR", help="生成物输出目录（默认 generated）"
    )
    parser.add_argument("--cpp", action="store_true", help="生成 C++ 头文件与实现")
    parser.add_argument(
        "--check", action="store_true", help="只做语法/语义检查，不写任何文件（CI 用）"
    )
    parser.add_argument(
        "--source-root",
        default=".",
        metavar="DIR",
        help="计算源文件相对路径的基准目录（默认当前目录）",
    )
    parser.add_argument(
        "--timestamp",
        default=None,
        metavar="ISO8601",
        help="写入文件头的生成时间；默认不写以保证生成物逐字节可复现",
    )
    parser.add_argument("-q", "--quiet", action="store_true", help="只输出错误")
    parser.add_argument("--version", action="version", version="qzcodegen {}".format(GENERATOR_VERSION))
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_argument_parser()
    args = parser.parse_args(argv)

    if not args.check and not args.cpp:
        parser.error("nothing to do: pass --cpp to generate code, or --check to validate only")

    inputs = _expand_inputs(args.input)
    bag = DiagnosticBag()
    nodes, bag = compile_files(inputs, source_root=args.source_root, bag=bag)

    for message in bag.messages():
        print(message, file=sys.stderr)

    if bag.has_errors():
        if not args.quiet:
            print(
                "qzcodegen: {} error(s), {} warning(s) — nothing written".format(
                    bag.error_count(), bag.warning_count()
                ),
                file=sys.stderr,
            )
        return 1

    if args.check:
        if not args.quiet:
            print(
                "qzcodegen: {} file(s) checked, protocol v{}, {} warning(s)".format(
                    len(nodes), PROTOCOL_VERSION, bag.warning_count()
                )
            )
        return 0

    changed = 0
    results = write_outputs(
        nodes,
        args.out,
        source_root=args.source_root,
        timestamp=_timestamp_from_env(args.timestamp),
    )
    for path, written in results:
        changed += 1 if written else 0
        if not args.quiet:
            print("{} {}".format("wrote    " if written else "unchanged", path))
    if not args.quiet:
        print(
            "qzcodegen: {} file(s) generated, {} updated, {} warning(s)".format(
                len(results), changed, bag.warning_count()
            )
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
