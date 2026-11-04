#!/usr/bin/env python3
"""从 Ninja Multi-Config 的 compile_commands.json 里抽出一套配置。

clang-tidy 会把同一文件的 Debug、Release、RelWithDebInfo 各跑一遍。CI 只测 RelWithDebInfo。
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


def select_commands(entries: list[dict], config: str) -> list[dict]:
    picked = [entry for entry in entries if config in json.dumps(entry)]
    return picked or list(entries)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="抽出一套配置的编译命令给 clang-tidy")
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--config", default="RelWithDebInfo")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    entries = json.loads(args.input.read_text(encoding="utf-8-sig"))
    picked = select_commands(entries, args.config)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    (args.output_dir / "compile_commands.json").write_text(json.dumps(picked), encoding="utf-8")
    print(f"clang-tidy commands: {len(picked)} of {len(entries)} ({args.config})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
