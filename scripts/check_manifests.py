#!/usr/bin/env python3
"""根清单和 vcpkg-configs/occt-7.9.3 除 overrides 外必须一致。

根清单不写 overrides（基线里的 OCCT 8.0.1）。第二份只多锁 7.9.3#1。
"""

from __future__ import annotations

import json
import sys
from pathlib import Path


ROOT = Path("vcpkg.json")
ALT = Path("vcpkg-configs/occt-7.9.3/vcpkg.json")
EXPECTED_OVERRIDE = [{"name": "opencascade", "version": "7.9.3#1"}]


def load(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def without_overrides(data: dict) -> dict:
    copied = dict(data)
    copied.pop("overrides", None)
    return copied


def main() -> int:
    root = load(ROOT)
    alt = load(ALT)
    if "overrides" in root:
        print("根 vcpkg.json 不能写 overrides", file=sys.stderr)
        return 1
    if without_overrides(root) != without_overrides(alt):
        print("两份清单除 overrides 外不一致", file=sys.stderr)
        print("root:", json.dumps(without_overrides(root), ensure_ascii=False, sort_keys=True), file=sys.stderr)
        print("alt:", json.dumps(without_overrides(alt), ensure_ascii=False, sort_keys=True), file=sys.stderr)
        return 1
    if alt.get("overrides") != EXPECTED_OVERRIDE:
        print("7.9.3 清单的 overrides 必须是 opencascade 7.9.3#1", file=sys.stderr)
        return 1
    print("vcpkg manifests: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
