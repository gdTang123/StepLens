#!/usr/bin/env python3
"""打包分类正反例。不启动 Windows，不调用 dumpbin。"""

from __future__ import annotations

import importlib.util
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("steplens_package", ROOT / "scripts" / "package.py")
pkg = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(pkg)
ORIGINAL_SHA = pkg.file_sha256

SYMBOLS = (
    "ucnv_open",
    "ucnv_close",
    "UCNV_FROM_U_CALLBACK_SUBSTITUTE",
    "UCNV_TO_U_CALLBACK_SUBSTITUTE",
    "ucnv_reset",
    "ucnv_getMaxCharSize",
    "ucnv_getName",
    "ucnv_getToUCallBack",
    "ucnv_getFromUCallBack",
    "ucnv_setToUCallBack",
    "ucnv_setFromUCallBack",
    "ucnv_cbToUWriteUChars",
    "ucnv_toUnicode",
    "ucnv_cbFromUWriteUChars",
    "ucnv_toUCountPending",
    "ucnv_fromUCountPending",
    "ucnv_getStandardName",
    "ucnv_getAvailableName",
    "ucnv_countAvailable",
    "ucnv_fromUnicode",
)


def fail(message: str) -> None:
    raise SystemExit(message)


def rows(symbols: set[str] | frozenset[str], delay: int = 0, dll: str = "icuuc.dll") -> list[tuple[str, str, int]]:
    return [(dll, symbol, delay) for symbol in symbols]


def dump_text(symbols: tuple[str, ...] | set[str], delay: bool = False, other: str | None = None) -> str:
    lines = [
        "Section contains the following imports:",
        "",
        "icuuc.dll",
        "0 time date stamp",
        "0000000180001000 Import Address Table",
    ]
    for symbol in symbols:
        lines.append("5A " + symbol)
    if other is not None:
        lines.extend(["", other, "5B u_init"])
    if delay:
        lines.extend(
            [
                "Section contains the following delay load imports:",
                "",
                "icuuc.dll",
                "5C ucnv_open",
            ]
        )
    lines.extend(["", "opengl32.dll", "10 wglGetCurrentContext"])
    return "\n".join(lines) + "\n"


def make_vcpkg(root: Path, port: str = "zlib", version: str = "1.3.2", licenses: bool = True, extra_list: bool = False, dll: str = "z.dll") -> Path:
    bindir = root / "installed" / "x64-windows" / "bin"
    bindir.mkdir(parents=True)
    (bindir / dll).write_bytes(b"MZ")
    info = root / "installed" / "vcpkg" / "info"
    info.mkdir(parents=True)
    (info / f"{port}_{version}_x64-windows.list").write_text(f"x64-windows/bin/{dll}\n", encoding="utf-8")
    if extra_list:
        (info / f"other_{version}_x64-windows.list").write_text(f"x64-windows/bin/{dll}\n", encoding="utf-8")
    if licenses:
        share = root / "installed" / "x64-windows" / "share" / port
        share.mkdir(parents=True)
        (share / "copyright").write_text("copyright\n", encoding="utf-8")
        (share / "vcpkg.spdx.json").write_text("{}\n", encoding="utf-8")
    return bindir


def install_fakes(qt_text: str, tk_text: str, depmap: dict[str, list[str]]) -> None:
    def run_text(command: list[str]):
        target = command[-1]
        stdout = qt_text if target.endswith("Qt6Core.dll") else tk_text

        class Result:
            returncode = 0
            stderr = ""

        result = Result()
        result.stdout = stdout
        return result

    def dependents_of(dumpbin: Path, binary: Path) -> list[str]:
        del dumpbin
        return list(depmap.get(binary.name, []))

    pkg.run_text = run_text
    pkg.dependents_of = dependents_of


def collect(app: Path, bindir: Path) -> set[str] | None:
    return pkg.collect_dependencies(app, bindir, Path("dumpbin"))


def observe_text(bindir: Path) -> str:
    return pkg.observe_log_path(bindir).read_text(encoding="utf-8")


for name in ("AUTHZ.dll", "UIAutomationCore.DLL", "d3d9.dll", "mscms.dll"):
    if not pkg.is_system_dll(name):
        fail("system name rejected: " + name)
if pkg.is_system_dll("icuuc.dll") or "icuuc.dll" in pkg.SYSTEM_DLLS:
    fail("icuuc is unconditional")
for name in ("icuuc64.dll", "icuin.dll", "icu.dll", "icuuc1.dll", "mystery.dll"):
    if pkg.is_system_dll(name):
        fail("similar or unknown allowed: " + name)
if set(SYMBOLS) != pkg.ICUUC_ENTRIES or len(pkg.ICUUC_ENTRIES) != 20:
    fail("entry set drifted")
if pkg.ICUUC_SHA256 != "39175459c4837c8cc0f709a637fb6eadb25d696785109c8393559ec876f02703":
    fail("sha drifted")

allowed, _basis = pkg.icuuc_system_basis(pkg.ICUUC_SHA256, [])
if not allowed:
    fail("hash gate")
allowed, _basis = pkg.icuuc_system_basis("deadbeef", rows(pkg.ICUUC_ENTRIES))
if not allowed:
    fail("exact 20 gate")
subset = set(pkg.ICUUC_ENTRIES)
subset.pop()
if pkg.icuuc_system_basis("deadbeef", rows(subset))[0]:
    fail("subset allowed")
if pkg.icuuc_system_basis("deadbeef", rows(set(pkg.ICUUC_ENTRIES) | {"ucnv_open_76"}))[0]:
    fail("extra symbol allowed")
if pkg.icuuc_system_basis("deadbeef", rows(pkg.ICUUC_ENTRIES, dll="icuin.dll"))[0]:
    fail("other icu dll allowed")
mixed = rows(pkg.ICUUC_ENTRIES) + [("icu.dll", "u_init", 0)]
if pkg.icuuc_system_basis("deadbeef", mixed)[0]:
    fail("second icu dll allowed")
if pkg.icuuc_system_basis("deadbeef", rows(pkg.ICUUC_ENTRIES, delay=1))[0]:
    fail("delay allowed")
parsed = pkg.parse_dumpbin_imports(dump_text(SYMBOLS, delay=True))
if ("icuuc.dll", "ucnv_open", 1) not in parsed or any(symbol == "stamp" for _dll, symbol, _delay in parsed):
    fail("parser mismatch: " + repr(parsed))
if pkg.license_ready("z.dll") or any(prefix == "z" for prefix in pkg.LICENSED_PREFIXES):
    fail("z prefix")

with tempfile.TemporaryDirectory() as raw:
    bindir = make_vcpkg(Path(raw))
    if not pkg.manifest_license_ok(bindir, "z.dll"):
        fail("zlib 1.3.2 should pass")
with tempfile.TemporaryDirectory() as raw:
    bindir = make_vcpkg(Path(raw), licenses=False)
    if pkg.manifest_license_ok(bindir, "z.dll"):
        fail("missing license passed")
with tempfile.TemporaryDirectory() as raw:
    bindir = make_vcpkg(Path(raw), version="9.9.9")
    if pkg.manifest_license_ok(bindir, "z.dll"):
        fail("wrong version passed")
with tempfile.TemporaryDirectory() as raw:
    bindir = make_vcpkg(Path(raw), port="notzlib")
    if pkg.manifest_license_ok(bindir, "z.dll"):
        fail("wrong port passed")
with tempfile.TemporaryDirectory() as raw:
    bindir = make_vcpkg(Path(raw), extra_list=True)
    if pkg.manifest_license_ok(bindir, "z.dll"):
        fail("two ports passed")
with tempfile.TemporaryDirectory() as raw:
    root = Path(raw)
    bindir = make_vcpkg(root, licenses=False)
    (root / "installed" / "vcpkg" / "info" / "zlib_1.3.2_x64-windows.list").unlink()
    (root / "installed" / "vcpkg" / "info" / "not-a-port-list.list").write_text("x64-windows/bin/z.dll\n", encoding="utf-8")
    if pkg.manifest_license_ok(bindir, "z.dll"):
        fail("unparsed list passed")

source = (ROOT / "scripts" / "package.py").read_text(encoding="utf-8")
if "--require-redist" not in source or "find_dumpbin" not in source:
    fail("redist or dumpbin gate removed")
if "System32" in source[source.index("def collect_dependencies") :]:
    fail("collect scans System32")
if "init_false_detail" in source or "ViewerWidget" in source:
    fail("package carries the gl diagnostic")


def prepare(root: Path, qt_text: str, deps: dict[str, list[str]], sha_patch: bool = False) -> tuple[Path, Path]:
    app = root / "app"
    app.mkdir()
    (app / "App.exe").write_bytes(b"MZ-app")
    (app / "Qt6Core.dll").write_bytes(b"qt-bytes")
    (root / "System32").mkdir()
    (root / "System32" / "icuuc.dll").write_bytes(b"not-a-system-copy")
    (root / "System32" / "mystery.dll").write_bytes(b"not-a-system-copy")
    (root / "System32" / "icuuc64.dll").write_bytes(b"not-a-system-copy")
    bindir = make_vcpkg(root)
    (bindir / "TKOpenGl.dll").write_bytes(b"tkopengl")
    install_fakes(qt_text, "Section contains the following imports:\n\nopengl32.dll\n10 wglCreateContext\n", deps)
    pkg.file_sha256 = ORIGINAL_SHA
    if sha_patch:

        def fake_sha(path: Path) -> str:
            if path.name == "Qt6Core.dll":
                return pkg.ICUUC_SHA256
            return ORIGINAL_SHA(path)

        pkg.file_sha256 = fake_sha
    return app, bindir


with tempfile.TemporaryDirectory() as raw:
    root = Path(raw)
    app, bindir = prepare(
        root,
        dump_text(SYMBOLS),
        {
            "App.exe": ["Qt6Core.dll", "AUTHZ.dll", "UIAutomationCore.DLL", "d3d9.dll", "mscms.dll", "z.dll"],
            "Qt6Core.dll": ["icuuc.dll"],
        },
    )
    result = collect(app, bindir)
    if result is None or (app / "icuuc.dll").exists() or not (app / "z.dll").is_file():
        fail("positive collect failed: " + observe_text(bindir) if (pkg.observe_log_path(bindir).is_file()) else "positive collect failed without observe")
    evidence = pkg.module_evidence_path(bindir).read_text(encoding="utf-8")
    if "qt6core_sha256=" not in evidence or "ucnv_fromUnicode" not in evidence or "TKOpenGl.dll" not in evidence:
        fail("evidence missing")
    if "System32" in evidence:
        fail("evidence used System32")

with tempfile.TemporaryDirectory() as raw:
    root = Path(raw)
    app, bindir = prepare(
        root,
        dump_text(tuple(subset)),
        {"App.exe": ["Qt6Core.dll"], "Qt6Core.dll": ["icuuc.dll"]},
        sha_patch=True,
    )
    result = collect(app, bindir)
    if result is None or (app / "icuuc.dll").exists():
        fail("hash gate collect failed")

with tempfile.TemporaryDirectory() as raw:
    root = Path(raw)
    app, bindir = prepare(
        root,
        dump_text(tuple(subset)),
        {"App.exe": ["Qt6Core.dll"], "Qt6Core.dll": ["icuuc.dll"]},
    )
    if collect(app, bindir) is not None or "icuuc.dll" not in observe_text(bindir):
        fail("subset was not rejected")

with tempfile.TemporaryDirectory() as raw:
    root = Path(raw)
    app, bindir = prepare(
        root,
        dump_text(tuple(pkg.ICUUC_ENTRIES) + ("ucnv_open_76",)),
        {"App.exe": ["Qt6Core.dll"], "Qt6Core.dll": ["icuuc.dll"]},
    )
    if collect(app, bindir) is not None:
        fail("extra symbol collect passed")

with tempfile.TemporaryDirectory() as raw:
    root = Path(raw)
    app, bindir = prepare(
        root,
        dump_text(SYMBOLS, other="icuin.dll"),
        {"App.exe": ["Qt6Core.dll"], "Qt6Core.dll": ["icuuc.dll", "icuin.dll"]},
    )
    if collect(app, bindir) is not None or "icuin.dll" not in observe_text(bindir):
        fail("icuin was released")

with tempfile.TemporaryDirectory() as raw:
    root = Path(raw)
    app, bindir = prepare(
        root,
        dump_text(SYMBOLS, delay=True),
        {"App.exe": ["Qt6Core.dll"], "Qt6Core.dll": ["icuuc.dll"]},
    )
    if collect(app, bindir) is not None:
        fail("delay collect passed")

with tempfile.TemporaryDirectory() as raw:
    root = Path(raw)
    app, bindir = prepare(
        root,
        dump_text(SYMBOLS),
        {"App.exe": ["icuuc64.dll", "mystery.dll"], "Qt6Core.dll": []},
    )
    if collect(app, bindir) is not None:
        fail("similar names passed")
    text = observe_text(bindir)
    if "icuuc64.dll" not in text or "mystery.dll" not in text or "System32" in text:
        fail("similar-name observe: " + text)
    if (app / "icuuc64.dll").exists() or (app / "mystery.dll").exists():
        fail("similar names were copied")

with tempfile.TemporaryDirectory() as raw:
    root = Path(raw)
    app, bindir = prepare(root, dump_text(SYMBOLS), {"App.exe": ["z.dll"], "Qt6Core.dll": []})
    (root / "installed" / "x64-windows" / "share" / "zlib" / "copyright").unlink()
    (root / "installed" / "x64-windows" / "share" / "zlib" / "vcpkg.spdx.json").unlink()
    if collect(app, bindir) is not None or (app / "z.dll").exists():
        fail("zlib without license was copied")
    if "z.dll" not in observe_text(bindir).split("refused_names=", 1)[1].split("\n", 1)[0]:
        fail("zlib missing evidence was not refused")

print("package_classify_ok")
