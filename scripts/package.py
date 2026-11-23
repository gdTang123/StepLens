#!/usr/bin/env python3
"""把 cmake --install 的结果打成绿色版 zip。

Qt 运行时仍用 windeployqt。OCCT、FreeType 以及 dumpbin 列出的 DLL 只从
--vcpkg-bin 复制。VC++ 运行库只从本机 Visual Studio 的官方再发行目录复制，
目录版本可以新于工具集，但不能更旧；文件还必须出现在该目录的 Redist.txt 里。
不从 System32 复制，也不从网络下载 DLL。缺目录或 Redist.txt 时失败。
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path


SYSTEM_DLLS = frozenset(
    {
        "advapi32.dll",
        "bcrypt.dll",
        "bcryptprimitives.dll",
        "cabinet.dll",
        "cfgmgr32.dll",
        "combase.dll",
        "comctl32.dll",
        "comdlg32.dll",
        "crypt32.dll",
        "cryptbase.dll",
        "d3d11.dll",
        "d3d12.dll",
        "d3dcompiler_47.dll",
        "dbghelp.dll",
        "dhcpcsvc.dll",
        "dnsapi.dll",
        "dwmapi.dll",
        "dwrite.dll",
        "dxgi.dll",
        "gdi32.dll",
        "gdiplus.dll",
        "glu32.dll",
        "hid.dll",
        "imagehlp.dll",
        "imm32.dll",
        "iphlpapi.dll",
        "kernel32.dll",
        "kernelbase.dll",
        "mpr.dll",
        "msimg32.dll",
        "msvcrt.dll",
        "mswsock.dll",
        "ncrypt.dll",
        "netapi32.dll",
        "normaliz.dll",
        "nsi.dll",
        "ntdll.dll",
        "ole32.dll",
        "oleacc.dll",
        "oleaut32.dll",
        "opengl32.dll",
        "powrprof.dll",
        "profapi.dll",
        "propsys.dll",
        "psapi.dll",
        "rpcrt4.dll",
        "sechost.dll",
        "secur32.dll",
        "setupapi.dll",
        "shcore.dll",
        "shell32.dll",
        "shlwapi.dll",
        "ucrtbase.dll",
        "urlmon.dll",
        "user32.dll",
        "userenv.dll",
        "uxtheme.dll",
        "version.dll",
        "webio.dll",
        "winhttp.dll",
        "wininet.dll",
        "winmm.dll",
        "winspool.drv",
        "wintrust.dll",
        "wldp.dll",
        "ws2_32.dll",
        "wsock32.dll",
        "wtsapi32.dll",
    }
)

# 这些前缀在 packaging/licenses 里有对应正文。名单以外的 vcpkg DLL 使打包失败。
LICENSED_PREFIXES = (
    "tk",
    "freetype",
    "brotli",
    "bz2",
    "libbz2",
    "libpng",
    "zlib",
    "qt6",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="打包 StepLens 绿色版目录")
    parser.add_argument("--prefix", type=Path, required=True, help="cmake --install 的前缀，下面应有 StepLens/")
    parser.add_argument("--qt-dir", type=Path, required=True, help="QTDIR，例如 C:/Qt/6.12.0/msvc2022_64")
    parser.add_argument("--vcpkg-bin", type=Path, required=True, help="vcpkg_installed/x64-windows/bin")
    parser.add_argument(
        "--vc-redist",
        type=Path,
        help="Microsoft.VC143.CRT 目录。不传则在本机 VC\\Redist 中选不旧于工具集的官方目录",
    )
    parser.add_argument("--output", type=Path, required=True, help="输出 zip 路径")
    return parser.parse_args()


def fail(message: str) -> int:
    print(message, file=sys.stderr)
    return 1


def _configure_stdio() -> None:
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is None:
            continue
        reconfigure(encoding="utf-8", errors="backslashreplace")


def is_system_dll(name: str) -> bool:
    lowered = name.lower()
    if lowered in SYSTEM_DLLS:
        return True
    return lowered.startswith("api-ms-win-") or lowered.startswith("ext-ms-")


def is_vc_runtime(name: str) -> bool:
    lowered = name.lower()
    return lowered.endswith(".dll") and lowered.startswith(
        ("vcruntime140", "msvcp140", "concrt140", "vccorlib140")
    )


def license_ready(name: str) -> bool:
    lowered = name.lower()
    return lowered.startswith(LICENSED_PREFIXES)


def run_text(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, check=False, capture_output=True, text=True, encoding="utf-8", errors="replace")


def vswhere_exe() -> Path | None:
    program_files = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
    candidate = Path(program_files) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if candidate.is_file():
        return candidate
    found = shutil.which("vswhere")
    return Path(found) if found else None


def vs_install_path() -> Path | None:
    vswhere = vswhere_exe()
    if vswhere is None:
        return None
    completed = run_text(
        [
            str(vswhere),
            "-latest",
            "-products",
            "*",
            "-requires",
            "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
            "-property",
            "installationPath",
        ]
    )
    if completed.returncode != 0:
        return None
    lines = [line.strip() for line in completed.stdout.splitlines() if line.strip()]
    if not lines:
        return None
    return Path(lines[-1])


def find_dumpbin() -> Path | None:
    found = shutil.which("dumpbin")
    if found:
        return Path(found)
    install = vs_install_path()
    if install is None:
        return None
    tools = sorted(path for path in (install / "VC" / "Tools" / "MSVC").glob("*") if path.is_dir())
    for tool in reversed(tools):
        candidate = tool / "bin" / "Hostx64" / "x64" / "dumpbin.exe"
        if candidate.is_file():
            return candidate
    return None


def version_key(name: str) -> tuple[int, ...] | None:
    numbers: list[int] = []
    for part in name.split("."):
        if not part.isdigit():
            return None
        numbers.append(int(part))
    if len(numbers) < 2:
        return None
    return tuple(numbers)


def toolset_from_dumpbin(dumpbin: Path) -> tuple[str, tuple[int, ...] | None]:
    parts = dumpbin.parts
    for index, part in enumerate(parts):
        if part.lower() == "msvc" and index + 1 < len(parts):
            label = parts[index + 1]
            return label, version_key(label)
    return "", None


def append_redist(found: list[tuple[Path, Path, str]], version_dir: Path) -> None:
    if not version_dir.is_dir():
        return
    crt = version_dir / "x64" / "Microsoft.VC143.CRT"
    redist_txt = version_dir / "Redist.txt"
    if "debug_nonredist" in {part.lower() for part in crt.parts}:
        return
    if crt.is_dir() and redist_txt.is_file():
        found.append((crt, redist_txt, version_dir.name))


def find_redist(explicit: Path | None, dumpbin: Path) -> tuple[Path, Path, str, str] | None:
    tool_label, tool_key = toolset_from_dumpbin(dumpbin)
    if explicit is not None:
        crt = explicit.resolve()
        if "debug_nonredist" in {part.lower() for part in crt.parts}:
            print(f"拒绝调试运行库目录 {crt}", file=sys.stderr)
            return None
        redist_txt = None
        label = crt.parent.name
        for parent in [crt, *crt.parents]:
            candidate = parent / "Redist.txt"
            if candidate.is_file():
                redist_txt = candidate
                label = parent.name
                break
            if parent.name.lower() == "msvc":
                break
        if redist_txt is None:
            print(f"{crt} 向上找不到 Redist.txt，不能确认这些 DLL 允许再分发", file=sys.stderr)
            return None
        return crt, redist_txt, label, tool_label

    if tool_key is None:
        print(
            f"dumpbin 在 {dumpbin}，路径里没有 VC\\Tools\\MSVC\\<版本>。"
            "读不到实际工具集版本，不能判断再发行目录是否更旧。不猜测补丁号。",
            file=sys.stderr,
        )
        return None
    install = vs_install_path()
    if install is None:
        print("没有找到带 VC 工具的 Visual Studio，不能定位官方再发行目录", file=sys.stderr)
        return None
    found: list[tuple[Path, Path, str]] = []
    redist_root = install / "VC" / "Redist" / "MSVC"
    if redist_root.is_dir():
        for version_dir in redist_root.iterdir():
            append_redist(found, version_dir)
    tools_redist = os.environ.get("VCToolsRedistDir")
    if tools_redist:
        append_redist(found, Path(tools_redist))
    if not found:
        print(
            f"工具集版本是 {tool_label}，在 {redist_root} 没有带 x64\\Microsoft.VC143.CRT 和 Redist.txt 的目录。"
            "不从系统目录复制，也不下载 DLL。",
            file=sys.stderr,
        )
        return None
    dotted = []
    named = []
    for crt, redist_txt, label in found:
        key = version_key(label)
        if key is None:
            named.append((crt, redist_txt, label))
        elif key >= tool_key:
            dotted.append((key, crt, redist_txt, label))
    if dotted:
        dotted.sort()
        _, crt, redist_txt, label = dotted[-1]
        return crt, redist_txt, label, tool_label
    if named:
        def named_rank(label: str) -> tuple[int, int | str]:
            lowered = label.lower()
            if lowered.startswith("v") and lowered[1:].isdigit():
                return (1, int(lowered[1:]))
            return (0, lowered)

        named.sort(key=lambda item: named_rank(item[2]))
        crt, redist_txt, label = named[-1]
        return crt, redist_txt, label, tool_label
    older = ", ".join(sorted(label for _, _, label in found))
    print(
        f"工具集版本是 {tool_label}。微软只支持相同或更新的再发行文件，找到的目录都更旧：{older}。"
        "不下载更新的 DLL，也不从系统目录复制。",
        file=sys.stderr,
    )
    return None


def redist_names(redist_txt: Path) -> set[str]:
    names: set[str] = set()
    for line in redist_txt.read_text(encoding="utf-8", errors="replace").splitlines():
        token = line.strip().strip('"')
        if not token or token.startswith(("#", ";", "[")):
            continue
        filename = Path(token).name
        if filename.lower().endswith(".dll"):
            names.add(filename.lower())
    return names


def dependents_of(dumpbin: Path, binary: Path) -> list[str] | None:
    completed = run_text([str(dumpbin), "/nologo", "/dependents", str(binary)])
    if completed.returncode != 0:
        print(f"dumpbin 不能读取 {binary}：{completed.stderr.strip()}", file=sys.stderr)
        return None
    names: list[str] = []
    for line in completed.stdout.splitlines():
        token = line.strip()
        if token.lower().endswith(".dll") and " " not in token:
            names.append(token)
    return names


def collect_dependencies(app_dir: Path, vcpkg_bin: Path, dumpbin: Path) -> set[str] | None:
    if not vcpkg_bin.is_dir():
        print(f"vcpkg bin 不存在：{vcpkg_bin}", file=sys.stderr)
        return None
    queue = [path for path in app_dir.rglob("*") if path.suffix.lower() in {".exe", ".dll"}]
    seen: set[Path] = set()
    needed_vc: set[str] = set()
    missing: list[str] = []
    refused: list[str] = []
    while queue:
        binary = queue.pop()
        resolved = binary.resolve()
        if resolved in seen:
            continue
        seen.add(resolved)
        names = dependents_of(dumpbin, binary)
        if names is None:
            return None
        for name in names:
            if is_system_dll(name):
                continue
            if is_vc_runtime(name):
                needed_vc.add(name)
                continue
            destination = app_dir / name
            if destination.is_file():
                queue.append(destination)
                continue
            source = vcpkg_bin / name
            if not source.is_file():
                missing.append(name)
                continue
            if not license_ready(name):
                refused.append(name)
                continue
            shutil.copy2(source, destination)
            queue.append(destination)
    if missing or refused:
        if missing:
            print("这些依赖不在 vcpkg bin，也不是系统 DLL，已停止：" + ", ".join(sorted(set(missing))), file=sys.stderr)
        if refused:
            print(
                "这些 DLL 没有对应的许可证材料，不复制：" + ", ".join(sorted(set(refused))),
                file=sys.stderr,
            )
        return None
    return needed_vc


def copy_vc_runtime(app_dir: Path, needed: set[str], explicit: Path | None, dumpbin: Path) -> bool:
    if not needed:
        return True
    located = find_redist(explicit, dumpbin)
    if located is None:
        print("需要 VC++ 运行库，但没有可用的官方再发行目录：" + ", ".join(sorted(needed)), file=sys.stderr)
        return False
    crt, redist_txt, redist_label, tool_label = located
    allowed = redist_names(redist_txt)
    if not allowed:
        print(f"{redist_txt} 没有列出 DLL，不能判断哪些文件允许再分发", file=sys.stderr)
        return False
    copied: list[str] = []
    for name in sorted(needed):
        if name.lower() not in allowed:
            print(f"{redist_txt} 没有点名 {name}，不从系统目录拿它", file=sys.stderr)
            return False
        source = crt / name
        if not source.is_file():
            print(f"官方目录 {crt} 里没有 {name}", file=sys.stderr)
            return False
        shutil.copy2(source, app_dir / name)
        copied.append(name)
    licenses = app_dir / "licenses"
    licenses.mkdir(parents=True, exist_ok=True)
    shutil.copy2(redist_txt, licenses / "MSVC-Redist.txt")
    tool_text = tool_label or "未从 dumpbin 路径读到"
    (licenses / "MSVC-Redist-来源.txt").write_text(
        "这是打包时读到的来源，不是仓库里自写的许可证，也不是法律审查结论。\n"
        f"工具集目录版本: {tool_text}\n"
        f"再发行目录名: {redist_label}\n"
        f"DLL 目录: {crt}\n"
        f"Redist.txt: {redist_txt}\n"
        "复制的文件: " + ", ".join(copied) + "\n"
        "规则: https://learn.microsoft.com/en-us/cpp/windows/determining-which-dlls-to-redistribute\n"
        "目录补丁号不必与工具集相同。只使用相同或更新的官方再发行目录；没有 Redist.txt 点名的文件不复制。\n",
        encoding="utf-8",
    )
    return True


def copy_license_tree(app_dir: Path) -> None:
    source = Path(__file__).resolve().parents[1] / "packaging" / "licenses"
    destination = app_dir / "licenses"
    shutil.copytree(source, destination, dirs_exist_ok=True)


def main() -> int:
    _configure_stdio()
    if sys.platform != "win32":
        return fail("绿色版打包只在 Windows 上收集 DLL。这次没有执行收集。")
    args = parse_args()
    app_dir = args.prefix / "StepLens"
    exe = app_dir / "StepLens.exe"
    if not exe.is_file():
        return fail(f"未找到 {exe}。先执行 cmake --install。")

    windeployqt = args.qt_dir / "bin" / "windeployqt.exe"
    if not windeployqt.is_file():
        return fail(f"未找到 {windeployqt}")

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
        return fail(f"windeployqt 退出码 {completed.returncode}")

    repo_qt_conf = Path(__file__).resolve().parents[1] / "packaging" / "qt.conf"
    (app_dir / "qt.conf").write_bytes(repo_qt_conf.read_bytes())

    dumpbin = find_dumpbin()
    if dumpbin is None:
        return fail("没有找到 dumpbin，不能按实际依赖收集 DLL。不改为整目录复制。")
    needed_vc = collect_dependencies(app_dir, args.vcpkg_bin, dumpbin)
    if needed_vc is None:
        return 1
    if not copy_vc_runtime(app_dir, needed_vc, args.vc_redist, dumpbin):
        return 1
    copy_license_tree(app_dir)

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
