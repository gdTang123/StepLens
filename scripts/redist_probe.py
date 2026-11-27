#!/usr/bin/env python3
"""只读记录 VS 安装树里的 Redist 证据。不复制 DLL，不改打包门。"""

from __future__ import annotations

import argparse
import hashlib
import os
import subprocess
import sys
from pathlib import Path

ENV_WHITELIST = (
    "ImageOS",
    "ImageVersion",
    "VCToolsVersion",
    "VCToolsRedistDir",
    "VCToolsInstallDir",
    "VSINSTALLDIR",
    "VCINSTALLDIR",
)

CRT_NAMES = (
    "MSVCP140.dll",
    "MSVCP140_1.dll",
    "MSVCP140_2.dll",
    "MSVCP140_ATOMIC_WAIT.dll",
    "VCRUNTIME140.dll",
    "VCRUNTIME140_1.dll",
)

CRT_NAME_KEYS = {name.lower() for name in CRT_NAMES}

NOT_WINDOWS = "未在 Windows 上读取"


def whitelist_env(source: dict[str, str]) -> dict[str, str]:
    return {key: source.get(key, "") for key in ENV_WHITELIST}


def root_is_bounded(root: Path) -> bool:
    if root.parent == root:
        return False
    anchor = Path(root.anchor)
    return root != anchor and root.parent != anchor


def install_root_from_tools(tools: Path) -> Path | None:
    parts = tools.parts
    for index in range(len(parts) - 2):
        if (
            parts[index].lower() == "vc"
            and parts[index + 1].lower() == "tools"
            and parts[index + 2].lower() == "msvc"
        ):
            if index == 0:
                return None
            return Path(*parts[:index])
    return None


def derive_root(env: dict[str, str]) -> tuple[Path | None, str]:
    install = env.get("VSINSTALLDIR", "").strip().strip('"')
    if install:
        path = Path(install)
        if path.is_dir() and not path.is_symlink() and root_is_bounded(path):
            return path, "VSINSTALLDIR"
        return None, "VSINSTALLDIR 不是有界安装目录，拒绝全盘扫描"
    tools = env.get("VCToolsInstallDir", "").strip().strip('"')
    if tools:
        root = install_root_from_tools(Path(tools))
        if root is not None and root.is_dir() and not root.is_symlink() and root_is_bounded(root):
            return root, "VCToolsInstallDir"
        return None, "VCToolsInstallDir 不能安全推出安装根，拒绝全盘扫描"
    return None, "没有 VSINSTALLDIR 或 VCToolsInstallDir，拒绝全盘扫描"


def contained(root: Path, path: Path) -> bool:
    try:
        path.absolute().relative_to(root.absolute())
    except ValueError:
        return False
    return True


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def windows_file_facts(path: Path) -> tuple[str, str]:
    if sys.platform != "win32":
        return NOT_WINDOWS, NOT_WINDOWS
    completed = subprocess.run(
        [
            "powershell",
            "-NoProfile",
            "-Command",
            "$item = Get-Item -LiteralPath $env:REDIST_PROBE_FILE; "
            "$sig = Get-AuthenticodeSignature -LiteralPath $env:REDIST_PROBE_FILE; "
            "$signer = ''; "
            "if ($sig.SignerCertificate) { $signer = $sig.SignerCertificate.Subject }; "
            "Write-Output ('file_version=' + $item.VersionInfo.FileVersion); "
            "Write-Output ('authenticode_status=' + $sig.Status); "
            "Write-Output ('authenticode_signer=' + $signer)",
        ],
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        env={**os.environ, "REDIST_PROBE_FILE": str(path)},
    )
    if completed.returncode != 0:
        detail = (completed.stderr or completed.stdout).strip().replace("\n", " ")
        return "读取失败", "读取失败 " + detail
    version = "未输出"
    status = "未输出"
    signer = ""
    for line in completed.stdout.splitlines():
        if line.startswith("file_version="):
            version = line.split("=", 1)[1]
        elif line.startswith("authenticode_status="):
            status = line.split("=", 1)[1]
        elif line.startswith("authenticode_signer="):
            signer = line.split("=", 1)[1]
    return version, status + ((" signer=" + signer) if signer else "")


def child_names(directory: Path) -> list[str]:
    names: list[str] = []
    try:
        children = sorted(directory.iterdir(), key=lambda item: item.name.lower())
    except OSError as exc:
        return ["读取失败 " + str(exc)]
    for child in children:
        if child.is_symlink():
            names.append(child.name + " -> 未跟随")
        else:
            names.append(child.name)
    return names


def three_levels(root: Path) -> list[str]:
    redist = root / "VC" / "Redist"
    if redist.is_symlink() or not redist.is_dir():
        return ["vc_redist=目录不存在或是链接，未展开"]
    lines = ["vc_redist=" + str(redist)]
    lines.append("level1=" + ",".join(child_names(redist)))
    try:
        level1 = [item for item in sorted(redist.iterdir(), key=lambda item: item.name.lower()) if item.is_dir() and not item.is_symlink()]
    except OSError as exc:
        return lines + ["level2=读取失败 " + str(exc)]
    for directory in level1:
        lines.append("level2 " + directory.name + "=" + ",".join(child_names(directory)))
        try:
            level2 = [item for item in sorted(directory.iterdir(), key=lambda item: item.name.lower()) if item.is_dir() and not item.is_symlink()]
        except OSError as exc:
            lines.append("level3 " + directory.name + "=读取失败 " + str(exc))
            continue
        for nested in level2:
            lines.append("level3 " + directory.name + "/" + nested.name + "=" + ",".join(child_names(nested)))
    return lines


def iter_inside(root: Path) -> list[tuple[str, Path]]:
    found: list[tuple[str, Path]] = []
    stack = [root]
    while stack:
        current = stack.pop()
        if not contained(root, current):
            found.append(("outside", current))
            continue
        if current.is_symlink():
            found.append(("symlink", current))
            continue
        if not current.is_dir():
            found.append(("file", current))
            continue
        try:
            children = list(current.iterdir())
        except OSError as exc:
            found.append(("error", current))
            found.append(("error-text", Path(str(exc))))
            continue
        for child in children:
            if not contained(root, child):
                found.append(("outside", child))
                continue
            if child.is_symlink():
                found.append(("symlink", child))
                continue
            if child.is_dir():
                stack.append(child)
            else:
                found.append(("file", child))
    return found


def matching_lines(path: Path) -> str:
    wanted = {name.lower() for name in CRT_NAMES}
    hits: list[str] = []
    text = path.read_text(encoding="utf-8", errors="replace")
    for line in text.splitlines():
        lowered = line.lower()
        if any(name in lowered for name in wanted):
            hits.append(line.strip())
    if not hits:
        return "原文未点名这六个候选"
    return " | ".join(hits)


def build_report(root: Path | None, env: dict[str, str], fact_reader=windows_file_facts) -> tuple[str, int]:
    visible = whitelist_env(env)
    lines = [
        "result=probe",
        "qualification=待确认",
        "note=这是采件原文，不是分发许可，也不是 package.py 的放行结论",
        "candidates=" + ",".join(CRT_NAMES),
    ]
    for key in ENV_WHITELIST:
        lines.append(key + "=" + (visible[key] or "未设置"))
    if root is None:
        lines.append("scan_root=")
        lines.append("scan=未扫描")
        return "\n".join(lines) + "\n", 1
    lines.append("scan_root=" + str(root))
    lines.extend(three_levels(root))
    redist_dir = visible["VCToolsRedistDir"].strip().strip('"')
    redist_path = Path(redist_dir) if redist_dir else None
    if redist_path is None:
        lines.append("crt_dir=VCToolsRedistDir 未设置")
    elif not contained(root, redist_path):
        lines.append("crt_dir=VCToolsRedistDir 越出安装树，未读取")
    else:
        crt_dir = redist_path / "x64" / "Microsoft.VC143.CRT"
        same = redist_path / "Redist.txt"
        lines.append("same_level_redist_txt=" + ("存在" if same.is_file() and not same.is_symlink() else "不存在"))
        if crt_dir.is_symlink() or not crt_dir.is_dir():
            lines.append("crt_dir=" + str(crt_dir))
            lines.append("crt_dir_exists=false")
        else:
            lines.append("crt_dir=" + str(crt_dir))
            lines.append("crt_dir_exists=true")
            lines.append("crt_dir_entries=" + ",".join(child_names(crt_dir)))
    entries = iter_inside(root)
    texts = [path for kind, path in entries if kind == "file" and path.name.lower() == "redist.txt"]
    if not texts:
        lines.append("redist_txt=安装树无 Redist.txt")
    for path in texts:
        lines.append(
            "redist_txt path="
            + str(path)
            + " bytes="
            + str(path.stat().st_size)
            + " sha256="
            + sha256_file(path)
            + " named="
            + matching_lines(path)
        )
    for path in [path for kind, path in entries if kind == "symlink"]:
        lines.append("skipped_link path=" + str(path) + " reason=不跟随链接")
    seen: set[Path] = set()
    dlls = []
    for kind, path in entries:
        if kind == "file" and path.name.lower() in CRT_NAME_KEYS:
            dlls.append(path)
    dlls.sort(key=lambda item: str(item).lower())
    found_names = {path.name.lower() for path in dlls}
    for path in dlls:
        resolved = path.absolute()
        if resolved in seen:
            continue
        seen.add(resolved)
        version, signature = fact_reader(path)
        lines.append(
            "crt path="
            + str(path)
            + " bytes="
            + str(path.stat().st_size)
            + " sha256="
            + sha256_file(path)
            + " file_version="
            + version
            + " authenticode="
            + signature
        )
    for name in CRT_NAMES:
        if name.lower() not in found_names:
            lines.append("crt_absent name=" + name)
    return "\n".join(lines) + "\n", 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="只读采 VS 安装树中的 Redist 证据")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--install-root", type=Path)
    args = parser.parse_args(argv)
    env = whitelist_env(os.environ)
    reason = "参数"
    root = args.install_root
    if root is None:
        root, reason = derive_root(env)
    elif not root.is_dir() or root.is_symlink() or not root_is_bounded(root):
        root = None
        reason = "--install-root 不是有界目录"
    if root is None:
        text, code = build_report(None, env)
        text += "scan_reason=" + reason + "\n"
    else:
        text, code = build_report(root, env)
        text += "scan_reason=" + reason + "\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text, encoding="utf-8", newline="\n")
    return code


if __name__ == "__main__":
    sys.exit(main())
