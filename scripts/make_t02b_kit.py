#!/usr/bin/env python3
"""从当前 git 提交打包 T02b 摸底 kit。

在仓库根目录执行（合入 main 之后给老板的那一份，要在 main HEAD 上再跑一遍）：

    python scripts/make_t02b_kit.py

zip 根目录：

    KIT_COMMIT              完整 commit SHA，一行
    bench/**                该提交的 bench/（与仓库里的 bench/ 相同）
    src/occtsupport/**      bench 在仓库外编译需要的兼容头

解压到干净目录后，`cmake -S bench` 能配置：`bench/../src/occtsupport` 与仓库布局一致。
`bench/` 或 `windows/run-t02b.ps1` 再改过，就在当时的 main HEAD 上把这条命令再跑一次。
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
import zipfile
from pathlib import Path, PurePosixPath


PACK_PREFIXES = ("bench", "src/occtsupport")
REQUIRED = (
    "bench/CMakeLists.txt",
    "bench/windows/run-t02b.ps1",
    "src/occtsupport/occt_compat.h",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="从当前 git 提交打包 T02b kit（bench/ + src/occtsupport + KIT_COMMIT）")
    parser.add_argument(
        "--output",
        type=Path,
        help="zip 路径。默认 dist/t02b-kit-<12位SHA>.zip",
    )
    parser.add_argument(
        "--repo",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help="仓库根目录，默认是本脚本的上两级",
    )
    return parser.parse_args()


def _configure_stdio() -> None:
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is None:
            continue
        reconfigure(encoding="utf-8", errors="backslashreplace")


def _git(repo: Path, *args: str, text: bool = True) -> subprocess.CompletedProcess[str] | subprocess.CompletedProcess[bytes]:
    return subprocess.run(
        ["git", *args],
        cwd=repo,
        check=True,
        capture_output=True,
        text=text,
    )


def _commit_sha(repo: Path) -> str:
    sha = _git(repo, "rev-parse", "HEAD").stdout.strip()
    if len(sha) != 40 or any(ch not in "0123456789abcdef" for ch in sha):
        raise SystemExit(f"git rev-parse HEAD 不是 40 位提交号：{sha}")
    return sha


def _warn_if_not_main(repo: Path) -> None:
    branch = _git(repo, "rev-parse", "--abbrev-ref", "HEAD").stdout.strip()
    if branch != "main":
        print(
            f"警告：当前分支是 {branch}，不是 main。发给老板的 kit 要在 main HEAD 上重新打包。",
            file=sys.stderr,
        )


def _warn_if_dirty(repo: Path) -> None:
    status = _git(repo, "status", "--porcelain", "--", *PACK_PREFIXES).stdout
    if status.strip():
        print(
            "警告：工作区里 bench/ 或 src/occtsupport 有未提交改动。kit 只打包当前提交，不含这些改动。",
            file=sys.stderr,
        )


def _tracked_files(repo: Path, sha: str) -> list[str]:
    listed = _git(repo, "ls-tree", "-r", "--name-only", sha, "--", *PACK_PREFIXES).stdout
    files = [line for line in listed.splitlines() if line]
    missing = [path for path in REQUIRED if path not in files]
    if missing:
        raise SystemExit("当前提交缺少 kit 必需文件：" + ", ".join(missing))
    return files


def _blob(repo: Path, sha: str, path: str) -> bytes:
    # git show 走二进制，避免文本模式改掉换行。
    completed = _git(repo, "show", f"{sha}:{path}", text=False)
    assert isinstance(completed.stdout, bytes)
    return completed.stdout


def _commit_date(repo: Path, sha: str) -> tuple[int, int, int, int, int, int]:
    timestamp = int(_git(repo, "log", "-1", "--format=%ct", sha).stdout.strip())
    return time.gmtime(timestamp)[:6]


def _directory_names(files: list[str]) -> list[str]:
    names: set[str] = set()
    for path in files:
        parts = PurePosixPath(path).parts
        for index in range(1, len(parts)):
            names.add("/".join(parts[:index]) + "/")
    return sorted(names)


def _add_bytes(archive: zipfile.ZipFile, name: str, data: bytes, date_time: tuple[int, int, int, int, int, int], mode: int) -> None:
    info = zipfile.ZipInfo(name, date_time)
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = mode << 16
    # Expand-Archive 认 DOS 属性：目录 0x10，文件带归档位 0x20。
    info.external_attr |= 0x10 if name.endswith("/") else 0x20
    archive.writestr(info, data)


def write_kit(repo: Path, sha: str, output: Path) -> int:
    files = _tracked_files(repo, sha)
    date_time = _commit_date(repo, sha)
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists():
        output.unlink()
    with zipfile.ZipFile(output, "w") as archive:
        for directory in _directory_names(files):
            _add_bytes(archive, directory, b"", date_time, 0o40755)
        for path in files:
            _add_bytes(archive, path, _blob(repo, sha, path), date_time, 0o100644)
        _add_bytes(archive, "KIT_COMMIT", (sha + "\n").encode("ascii"), date_time, 0o100644)
    return len(files)


def main() -> int:
    _configure_stdio()
    args = parse_args()
    repo = args.repo.resolve()
    if not (repo / ".git").exists():
        print(f"{repo} 不是 git 仓库", file=sys.stderr)
        return 1
    sha = _commit_sha(repo)
    _warn_if_not_main(repo)
    _warn_if_dirty(repo)
    output = args.output
    if output is None:
        output = repo / "dist" / f"t02b-kit-{sha[:12]}.zip"
    count = write_kit(repo, sha, output)
    print(f"KIT_COMMIT={sha}")
    print(f"已写入 {output}（{count} 个仓库文件 + KIT_COMMIT）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
