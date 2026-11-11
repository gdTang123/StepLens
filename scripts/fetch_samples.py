#!/usr/bin/env python3
"""按 docs/prd/samples.md 下载样例到 STEPLENS_SAMPLES，并用清单里的 sha256 校验。

大文件只写到目标目录，不进仓库。清单是 scripts/samples_manifest.json。
S-08～S-10 先校验 NIST 压缩包，再按成员路径解出单个文件并校验。
已经存在且 sha256 相符的文件不会重新下载。
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import time
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

_CHUNK = 1024 * 1024
_USER_AGENT = "StepLens-fetch/0.1"
_ATTEMPTS = 4


class UsageError(Exception):
    """命令行或环境不完整。退出码 2。"""


def manifest_path() -> Path:
    return Path(__file__).resolve().with_name("samples_manifest.json")


def load_manifest(path: Path) -> dict:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise UsageError(f"无法读取样例清单 {path}: {exc}") from exc
    if not isinstance(document, dict) or not isinstance(document.get("samples"), list):
        raise UsageError(f"样例清单格式不对: {path}")
    return document


def resolve_destination(explicit: str | None) -> Path:
    if explicit:
        return Path(explicit)
    env = os.environ.get("STEPLENS_SAMPLES")
    if env is None or env.strip() == "":
        raise UsageError(
            "STEPLENS_SAMPLES 未设置。\n"
            "样例不要提交到仓库。请指定下载目录，例如：\n"
            "  STEPLENS_SAMPLES=/path/to/samples python3 scripts/fetch_samples.py"
        )
    return Path(env)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while True:
            chunk = handle.read(_CHUNK)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def _expected_sha(entry: dict, label: str) -> str:
    value = entry.get("sha256")
    if not isinstance(value, str) or len(value) != 64:
        raise UsageError(f"{label} 的 sha256 不是 64 位十六进制")
    try:
        int(value, 16)
    except ValueError as exc:
        raise UsageError(f"{label} 的 sha256 不是十六进制") from exc
    return value.lower()


def _expected_bytes(entry: dict) -> int | None:
    value = entry.get("bytes")
    if value is None:
        return None
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise UsageError(f"{entry.get('id', entry.get('filename'))} 的 bytes 不是非负整数")
    return value


def verify_file(path: Path, expected_sha: str, expected_bytes: int | None) -> tuple[bool, str]:
    if not path.is_file():
        return False, "文件不存在"
    size = path.stat().st_size
    digest = sha256_file(path)
    if digest.lower() != expected_sha.lower():
        return False, f"sha256 {digest} 与清单 {expected_sha} 不一致（大小 {size}）"
    if expected_bytes is not None and size != expected_bytes:
        return False, f"sha256 相符但大小 {size} 与清单 {expected_bytes} 不一致"
    return True, digest.lower()


def download(url: str, dest: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    partial = dest.with_name(dest.name + ".partial")
    last_error: Exception | None = None
    for attempt in range(1, _ATTEMPTS + 1):
        try:
            request = urllib.request.Request(url, headers={"User-Agent": _USER_AGENT})
            with urllib.request.urlopen(request, timeout=120) as response, partial.open("wb") as handle:
                while True:
                    chunk = response.read(_CHUNK)
                    if not chunk:
                        break
                    handle.write(chunk)
            os.replace(partial, dest)
            return
        except (OSError, urllib.error.URLError, TimeoutError) as exc:
            last_error = exc
            partial.unlink(missing_ok=True)
            if attempt < _ATTEMPTS:
                time.sleep(2 * attempt)
    raise RuntimeError(f"下载失败 {url}: {last_error}")


def _extract_member(archive_path: Path, member: str, dest: Path) -> None:
    with zipfile.ZipFile(archive_path) as archive:
        try:
            payload = archive.read(member)
        except KeyError as exc:
            names = ", ".join(archive.namelist()[:8])
            raise RuntimeError(f"压缩包里没有 {member}（前几个成员: {names}）") from exc
    dest.parent.mkdir(parents=True, exist_ok=True)
    partial = dest.with_name(dest.name + ".partial")
    partial.write_bytes(payload)
    os.replace(partial, dest)


def _ensure_verified(url: str, dest: Path, expected_sha: str, expected_bytes: int | None, label: str) -> str:
    ok, detail = verify_file(dest, expected_sha, expected_bytes)
    if ok:
        print(f"{label} 已校验 {dest.stat().st_size} {detail} {dest.name}", flush=True)
        return detail
    print(f"{label} 需要下载 {url}", flush=True)
    download(url, dest)
    ok, detail = verify_file(dest, expected_sha, expected_bytes)
    if not ok:
        raise RuntimeError(f"{label} 下载后校验失败: {detail}")
    print(f"{label} 已下载 {dest.stat().st_size} {detail} {dest.name}", flush=True)
    return detail


def fetch_samples(destination: Path, manifest: dict, only: set[str] | None) -> int:
    archives = manifest.get("archives") or {}
    if not isinstance(archives, dict):
        raise UsageError("清单 archives 必须是对象")
    destination.mkdir(parents=True, exist_ok=True)
    archive_ready: set[str] = set()
    seen: set[str] = set()
    count = 0
    for entry in manifest["samples"]:
        if not isinstance(entry, dict):
            raise UsageError("清单 samples 里有非对象条目")
        sample_id = entry.get("id")
        filename = entry.get("filename")
        if not isinstance(sample_id, str) or not isinstance(filename, str) or not filename:
            raise UsageError("清单条目缺少 id 或 filename")
        if sample_id in seen:
            raise UsageError(f"清单里样例编号重复: {sample_id}")
        seen.add(sample_id)
        if only is not None and sample_id not in only:
            continue
        expected_sha = _expected_sha(entry, sample_id)
        expected_bytes = _expected_bytes(entry)
        dest = destination / filename
        archive_id = entry.get("archive")
        if archive_id is not None:
            if not isinstance(archive_id, str) or archive_id not in archives:
                raise UsageError(f"{sample_id} 的 archive 不在清单里")
            archive = archives[archive_id]
            member = entry.get("member")
            if not isinstance(member, str) or not member:
                raise UsageError(f"{sample_id} 缺少压缩包成员路径")
            if archive_id not in archive_ready:
                archive_name = archive.get("filename")
                archive_url = archive.get("url")
                if not isinstance(archive_name, str) or not isinstance(archive_url, str):
                    raise UsageError(f"压缩包 {archive_id} 缺少 filename 或 url")
                _ensure_verified(
                    archive_url,
                    destination / archive_name,
                    _expected_sha(archive, archive_id),
                    _expected_bytes(archive),
                    archive_id,
                )
                archive_ready.add(archive_id)
            ok, detail = verify_file(dest, expected_sha, expected_bytes)
            if not ok:
                print(f"{sample_id} 从 {archive_id} 解出 {member}", flush=True)
                _extract_member(destination / archives[archive_id]["filename"], member, dest)
                ok, detail = verify_file(dest, expected_sha, expected_bytes)
            if not ok:
                raise RuntimeError(f"{sample_id} 解压后校验失败: {detail}")
            print(f"{sample_id} 已校验 {dest.stat().st_size} {detail} {filename}", flush=True)
        else:
            url = entry.get("url")
            if not isinstance(url, str) or not url:
                raise UsageError(f"{sample_id} 缺少 url")
            _ensure_verified(url, dest, expected_sha, expected_bytes, sample_id)
        count += 1
    if only is not None:
        missing = sorted(only - seen)
        if missing:
            raise UsageError("清单里没有这些编号: " + ", ".join(missing))
    print(f"samples={count} failed=0 directory={destination}", flush=True)
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="下载 docs/prd/samples.md 里的样例并校验 sha256")
    parser.add_argument("--dest", help="下载目录。默认用环境变量 STEPLENS_SAMPLES")
    parser.add_argument("--manifest", type=Path, default=None, help="样例清单 JSON")
    parser.add_argument("--only", help="只处理这些编号，逗号分隔，例如 S-01,S-03")
    args = parser.parse_args(argv)
    try:
        destination = resolve_destination(args.dest)
        manifest = load_manifest(args.manifest or manifest_path())
        only = None
        if args.only:
            only = {item.strip() for item in args.only.split(",") if item.strip()}
            if not only:
                raise UsageError("--only 没有给出编号")
        return fetch_samples(destination, manifest, only)
    except UsageError as exc:
        sys.stderr.write(f"{exc}\n")
        return 2
    except Exception as exc:
        sys.stderr.write(f"fetch_samples.py 失败: {exc}\n")
        return 1


if __name__ == "__main__":
    sys.exit(main())
