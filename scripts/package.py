#!/usr/bin/env python3
"""把 cmake --install 的结果打成 zip。

T01 只调用 windeployqt 收集 Qt 运行时。OCCT、FreeType 的 DLL 和 VC++ 运行库在 T17 收集。
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import zipfile
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="打包 StepLens 绿色版目录（T01 最小实现）")
    parser.add_argument("--prefix", type=Path, required=True, help="cmake --install 的前缀，下面应有 StepLens/")
    parser.add_argument("--qt-dir", type=Path, required=True, help="QTDIR，例如 C:/Qt/6.12.0/msvc2022_64")
    parser.add_argument("--output", type=Path, required=True, help="输出 zip 路径")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    app_dir = args.prefix / "StepLens"
    exe = app_dir / "StepLens.exe"
    if not exe.is_file():
        print(f"未找到 {exe}。先执行 cmake --install。", file=sys.stderr)
        return 1

    windeployqt = args.qt_dir / "bin" / "windeployqt.exe"
    if not windeployqt.is_file():
        print(f"未找到 {windeployqt}", file=sys.stderr)
        return 1

    command = [
        str(windeployqt),
        "--release",
        "--no-translations",
        "--no-system-d3d-compiler",
        "--no-opengl-sw",
        "--no-compiler-runtime",
        str(exe),
    ]
    print(" ".join(command))
    completed = subprocess.run(command, cwd=app_dir, check=False)
    if completed.returncode != 0:
        print(f"windeployqt 退出码 {completed.returncode}", file=sys.stderr)
        return completed.returncode

    # windeployqt 会改写 qt.conf。用仓库里的相对路径版本盖回去。
    repo_qt_conf = Path(__file__).resolve().parents[1] / "packaging" / "qt.conf"
    (app_dir / "qt.conf").write_bytes(repo_qt_conf.read_bytes())

    args.output.parent.mkdir(parents=True, exist_ok=True)
    if args.output.exists():
        args.output.unlink()
    with zipfile.ZipFile(args.output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(app_dir.rglob("*")):
            if path.is_file():
                archive.write(path, path.relative_to(args.prefix).as_posix())
    print(f"已写入 {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
