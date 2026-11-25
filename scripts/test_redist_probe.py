#!/usr/bin/env python3
"""redist-probe 的静态正反例。不启动 Windows，不编译产品。"""

from __future__ import annotations

import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CI = (ROOT / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8")
SPEC = importlib.util.spec_from_file_location("redist_probe", ROOT / "scripts" / "redist_probe.py")
probe = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(probe)


def fail(message: str) -> None:
    raise SystemExit(message)


def core_runs(event: str, mode: str) -> bool:
    return event != "workflow_dispatch" or mode != "redist-probe"


def probe_runs(event: str, mode: str) -> bool:
    return event == "workflow_dispatch" and mode == "redist-probe"


cases = (
    ("pull_request", "", True, False),
    ("push", "", True, False),
    ("workflow_dispatch", "default", True, False),
    ("workflow_dispatch", "redist-probe", False, True),
    ("workflow_dispatch", "redist-probe-extra", True, False),
    ("workflow_dispatch", "REDIST-PROBE", True, False),
    ("workflow_dispatch", "", True, False),
)
for event, mode, want_core, want_probe in cases:
    if core_runs(event, mode) != want_core or probe_runs(event, mode) != want_probe:
        fail(f"mode gate {event} {mode}")

if CI.count("github.event_name != 'workflow_dispatch' || github.event.inputs.mode != 'redist-probe'") != 2:
    fail("default jobs missing exact negative gate")
if CI.count("github.event_name == 'workflow_dispatch' && github.event.inputs.mode == 'redist-probe'") != 1:
    fail("probe job missing exact gate")
job = CI.split("\n  redist-probe:\n", 1)[1]
for banned in ("cmake", "ctest", "vcpkg", "clang-format", "package.py", "OCCT"):
    if banned in job:
        fail("probe job starts full CI via " + banned)
if "ilammy/msvc-dev-cmd@v1" not in job or "arch: x64" not in job:
    fail("probe job does not reuse the MSVC init")
if "scripts/redist_probe.py" not in job:
    fail("probe command missing")
if "package.py" in CI.split("redist-probe", 1)[0] and False:
    fail("unused")

outside = ROOT / ".." / "redist-probe-outside"
outside.mkdir(exist_ok=True)
secret_file = outside / "Redist.txt"
secret_file.write_text("MSVCP140.dll\n", encoding="utf-8")
tree = ROOT / ".." / "redist-probe-tree"
if tree.exists():
    import shutil
    shutil.rmtree(tree)
install = tree / "Enterprise"
redist = install / "VC" / "Redist" / "MSVC" / "14.44.35112"
crt = redist / "x64" / "Microsoft.VC143.CRT"
crt.mkdir(parents=True)
(redist / "Redist.txt").write_text("other.dll\n", encoding="utf-8")
(crt / "VCRUNTIME140.dll").write_bytes(b"MZ-runtime")
(crt / "MSVCP140.dll").write_bytes(b"MZ-msvcp")
link = install / "VC" / "escape"
link.symlink_to(outside, target_is_directory=True)
env = {
    "ImageVersion": "20261125.1",
    "VCToolsVersion": "14.44.35207",
    "VCToolsRedistDir": str(redist),
    "VCToolsInstallDir": str(install / "VC" / "Tools" / "MSVC" / "14.44.35207"),
    "SECRET_TOKEN": "supersecret-value",
    "PATH": "/usr/bin",
}
(install / "VC" / "Tools" / "MSVC" / "14.44.35207").mkdir(parents=True)
text, code = probe.build_report(install, env, lambda path: (probe.NOT_WINDOWS, probe.NOT_WINDOWS))
if code != 0 or "supersecret-value" in text or "SECRET_TOKEN" in text:
    fail("secret leaked or scan failed")
if "ImageVersion=20261125.1" not in text or "VCToolsVersion=14.44.35207" not in text:
    fail("whitelist missing")
if "sha256=" not in text or "安装树无 Redist.txt" in text:
    fail("redist text missing")
if "原文未点名这六个候选" not in text:
    fail("unrelated redist text was treated as a hit")
if "VCRUNTIME140.dll" not in text or "MSVCP140.dll" not in text:
    fail("crt files missing")
if "file_version=" + probe.NOT_WINDOWS not in text:
    fail("windows fact was invented")
if "skipped_link" not in text or str(secret_file) in text:
    fail("symlink was followed")
if "same_level_redist_txt=存在" not in text:
    fail("same-level file not recorded")
if "level1=" not in text or "level3 " not in text:
    fail("three levels missing")
absent = tree / "Enterprise2"
(absent / "VC" / "Redist").mkdir(parents=True)
text, code = probe.build_report(absent, {"VCToolsRedistDir": str(absent / "VC" / "Redist" / "missing")}, lambda path: ("", ""))
if "安装树无 Redist.txt" not in text or "crt_dir_exists=false" not in text:
    fail("absence not explicit")
if "crt_absent name=MSVCP140_ATOMIC_WAIT.dll" not in text:
    fail("missing crt name not explicit")
derived, reason = probe.derive_root({"VCToolsInstallDir": str(install / "VC" / "Tools" / "MSVC" / "14.44.35207")})
if derived != install or reason != "VCToolsInstallDir":
    fail("derive root " + str(derived))
unsafe, unsafe_reason = probe.derive_root({"VCToolsInstallDir": str(Path("/VC/Tools/MSVC/14.44.35207"))})
if unsafe is not None:
    fail("drive root was accepted")
if probe.derive_root({})[0] is not None:
    fail("empty env scanned")
outside_redist = probe.build_report(install, {"VCToolsRedistDir": str(outside)}, lambda path: ("", ""))[0]
if "越出安装树" not in outside_redist:
    fail("outside redist dir was read")
import hashlib
lower = crt / "msvcp140.dll"
mixed = crt / "VcRuntime140_1.dll"
noise = crt / "notacrt.dll"
lower.write_bytes(b"mz-lower")
mixed.write_bytes(b"mz-mixed")
noise.write_bytes(b"mz-noise")
text2, code2 = probe.build_report(install, env, lambda path: (probe.NOT_WINDOWS, probe.NOT_WINDOWS))
if code2 != 0:
    fail("case scan failed")

def crt_hit(body: str, filename: str, payload: bytes) -> None:
    digest = hashlib.sha256(payload).hexdigest()
    for line in body.splitlines():
        if not line.startswith("crt path="):
            continue
        if not line.split(" bytes=", 1)[0].endswith("/" + filename) and not line.split(" bytes=", 1)[0].endswith("\\" + filename):
            continue
        if "sha256=" + digest not in line:
            fail("hash miss " + filename)
        if "file_version=" + probe.NOT_WINDOWS not in line:
            fail("version stub miss " + filename)
        if "authenticode=" + probe.NOT_WINDOWS not in line:
            fail("signature stub miss " + filename)
        return
    fail("case name not collected " + filename)

crt_hit(text2, "msvcp140.dll", b"mz-lower")
crt_hit(text2, "VcRuntime140_1.dll", b"mz-mixed")
if any(line.startswith("crt path=") and "notacrt.dll" in line for line in text2.splitlines()):
    fail("non-crt collected")
if "crt_absent name=MSVCP140.dll" in text2 or "crt_absent name=VCRUNTIME140_1.dll" in text2:
    fail("case variant still marked absent")
print("case_names_collected_not_windows")
print("redist_probe_static_ok")
