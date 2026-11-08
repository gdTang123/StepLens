#!/usr/bin/env python3
"""默认依赖是 OCCT 7.9.3。

根 vcpkg.json 不写 overrides，只给对照预设 msvc-occt801（基线端口当前是 8.0.1，不是默认）。
vcpkg-configs/occt-7.9.3 只多锁 opencascade 7.9.3#1，其余字段必须与根清单一致。
CMake 默认预设 base，以及 workflow ci 用的 msvc，必须指向这份 7.9.3 清单。
"""

from __future__ import annotations

import json
import sys
from pathlib import Path


ROOT = Path("vcpkg.json")
ALT = Path("vcpkg-configs/occt-7.9.3/vcpkg.json")
PRESETS = Path("CMakePresets.json")
CMAKE_LISTS = Path("CMakeLists.txt")
EXPECTED_OVERRIDE = [{"name": "opencascade", "version": "7.9.3#1"}]
DEFAULT_MANIFEST_SUFFIX = "vcpkg-configs/occt-7.9.3"


def load(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def without_overrides(data: dict) -> dict:
    copied = dict(data)
    copied.pop("overrides", None)
    return copied


def manifest_dir(value: object) -> str:
    return str(value).replace("\\", "/")


def check_manifests() -> int:
    root = load(ROOT)
    alt = load(ALT)
    if "overrides" in root:
        print("根 vcpkg.json 不能写 overrides（它是 8.0.1 对照清单，不是默认）", file=sys.stderr)
        return 1
    if without_overrides(root) != without_overrides(alt):
        print("两份清单除 overrides 外不一致", file=sys.stderr)
        print("root:", json.dumps(without_overrides(root), ensure_ascii=False, sort_keys=True), file=sys.stderr)
        print("alt:", json.dumps(without_overrides(alt), ensure_ascii=False, sort_keys=True), file=sys.stderr)
        return 1
    if alt.get("overrides") != EXPECTED_OVERRIDE:
        print("默认清单的 overrides 必须是 opencascade 7.9.3#1", file=sys.stderr)
        return 1
    return 0


def check_presets() -> int:
    presets = load(PRESETS)
    configure = {item["name"]: item for item in presets["configurePresets"]}
    base_cache = configure["base"]["cacheVariables"]
    if base_cache.get("STEPLENS_EXPECTED_OCCT") != "7.9.3":
        print("base 预设的 STEPLENS_EXPECTED_OCCT 必须是 7.9.3", file=sys.stderr)
        return 1
    if not manifest_dir(base_cache.get("VCPKG_MANIFEST_DIR", "")).endswith(DEFAULT_MANIFEST_SUFFIX):
        print("base 预设的 VCPKG_MANIFEST_DIR 必须是 vcpkg-configs/occt-7.9.3", file=sys.stderr)
        return 1

    workflow = next(item for item in presets["workflowPresets"] if item["name"] == "ci")
    configure_name = next(step["name"] for step in workflow["steps"] if step["type"] == "configure")
    if configure_name != "msvc":
        print("ci workflow 的 configure 预设必须是 msvc", file=sys.stderr)
        return 1
    if configure["msvc"].get("inherits") != "base":
        print("msvc 预设必须继承 base", file=sys.stderr)
        return 1
    msvc_cache = configure["msvc"].get("cacheVariables", {})
    if msvc_cache.get("STEPLENS_EXPECTED_OCCT", "7.9.3") != "7.9.3":
        print("msvc 预设不能把 STEPLENS_EXPECTED_OCCT 改离 7.9.3", file=sys.stderr)
        return 1
    if "VCPKG_MANIFEST_DIR" in msvc_cache and not manifest_dir(msvc_cache["VCPKG_MANIFEST_DIR"]).endswith(
        DEFAULT_MANIFEST_SUFFIX
    ):
        print("msvc 预设不能改离 vcpkg-configs/occt-7.9.3", file=sys.stderr)
        return 1

    lists = CMAKE_LISTS.read_text(encoding="utf-8")
    if 'set(STEPLENS_EXPECTED_OCCT "7.9.3" CACHE STRING' not in lists:
        print('CMakeLists.txt 的 STEPLENS_EXPECTED_OCCT 默认值必须是 "7.9.3"', file=sys.stderr)
        return 1

    for preset in presets["configurePresets"]:
        expected = preset.get("cacheVariables", {}).get("STEPLENS_EXPECTED_OCCT")
        if expected != "8.0.1":
            continue
        if "对照" not in preset.get("displayName", ""):
            print(f"{preset['name']} 使用 8.0.1，但 displayName 没有标明「对照」", file=sys.stderr)
            return 1
    return 0


def main() -> int:
    if check_manifests() != 0 or check_presets() != 0:
        return 1
    print("vcpkg manifests: ok (default OCCT 7.9.3)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
