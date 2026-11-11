#!/usr/bin/env python3
"""按架构 7.5 办法一，把现有 STEP 文本拼成更大的文件。

1. 用第一个文件的 HEADER（DATA 之前的全部内容）。
2. 每个文件的 DATA 段里，所有 #数字 加上前面文件的最大实体号。
3. 各段 DATA 依次写出。结果是一个有多个顶层产品的 STEP。

局限（架构原文）：各份几何在空间上重叠；字符串里的 #数字 也会被改掉。
办法二 tools/stepgen 不在这里。生成文件默认写到 local/bench/，不要提交。
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from pathlib import Path

_ID = re.compile(br"#(\d+)")
_DEF = re.compile(br"#(\d+)\s*=")

# 约 500 MiB / 约 1 GiB。优先用架构 7.5 举的组合；缺文件时换下一组。
_PRESETS: dict[str, dict] = {
    "500mib": {
        "minimum_bytes": 500 * 1024 * 1024,
        "default_name": "big-500mib.step",
        "recipes": [
            ["X-04", "X-05"],
            ["X-01", "X-05"],
        ],
    },
    "1gib": {
        "minimum_bytes": 900 * 1024 * 1024,
        "default_name": "big-1gib.step",
        "recipes": [
            ["X-02", "X-02", "X-02"],
            ["X-02", "X-01", "X-04"],
        ],
    },
}


class StepTextError(Exception):
    pass


def manifest_path() -> Path:
    return Path(__file__).resolve().with_name("samples_manifest.json")


def load_id_filenames(path: Path) -> dict[str, str]:
    document = json.loads(path.read_text(encoding="utf-8"))
    mapping: dict[str, str] = {}
    for entry in document["samples"]:
        mapping[entry["id"]] = entry["filename"]
    return mapping


def locate_data(blob: bytes) -> tuple[bytes, bytes]:
    data_at = blob.find(b"DATA;")
    if data_at < 0:
        raise StepTextError("没有 DATA; 段")
    end_iso = blob.rfind(b"END-ISO-10303-21")
    if end_iso < 0:
        raise StepTextError("没有 END-ISO-10303-21")
    endsec = blob.rfind(b"ENDSEC;", 0, end_iso)
    body_start = data_at + len(b"DATA;")
    if endsec < body_start:
        raise StepTextError("DATA 段没有 ENDSEC;")
    return blob[:data_at], blob[body_start:endsec]


def max_entity_id(body: bytes) -> int:
    found = [int(match) for match in _ID.findall(body)]
    if not found:
        raise StepTextError("DATA 段里没有 #数字")
    return max(found)


def shift_ids(body: bytes, offset: int) -> bytes:
    if offset == 0:
        return body

    def replace(match: re.Match[bytes]) -> bytes:
        return b"#" + str(int(match.group(1)) + offset).encode("ascii")

    return _ID.sub(replace, body)


def definition_ids(body: bytes) -> list[int]:
    return [int(match) for match in _DEF.findall(body)]


def concatenate(paths: list[Path], output: Path) -> dict:
    if not paths:
        raise StepTextError("没有输入文件")
    output.parent.mkdir(parents=True, exist_ok=True)
    partial = output.with_name(output.name + ".partial")
    offset = 0
    total_defs: list[int] = []
    inputs: list[str] = []
    with partial.open("wb") as handle:
        for index, path in enumerate(paths):
            blob = path.read_bytes()
            preamble, body = locate_data(blob)
            original_max = max_entity_id(body)
            shifted = shift_ids(body, offset)
            shifted_ids = definition_ids(shifted)
            total_defs.extend(shifted_ids)
            if index == 0:
                handle.write(preamble)
                if not preamble.endswith(b"\n"):
                    handle.write(b"\n")
                handle.write(b"DATA;")
            if shifted and not shifted.startswith((b"\n", b"\r")):
                handle.write(b"\n")
            handle.write(shifted)
            if shifted and not shifted.endswith((b"\n", b"\r")):
                handle.write(b"\n")
            inputs.append(str(path))
            offset += original_max
        handle.write(b"ENDSEC;\nEND-ISO-10303-21;\n")
    os.replace(partial, output)
    unique = set(total_defs)
    if len(unique) != len(total_defs):
        raise StepTextError(f"拼接后实体号冲突: 定义 {len(total_defs)} 个，不同编号 {len(unique)} 个")
    size = output.stat().st_size
    return {"output": str(output), "bytes": size, "inputs": inputs, "max_entity_id": offset}


def _samples_dir(explicit: str | None) -> Path | None:
    if explicit:
        return Path(explicit)
    env = os.environ.get("STEPLENS_SAMPLES")
    if env:
        return Path(env)
    return None


def resolve_token(token: str, samples: Path | None, names: dict[str, str]) -> Path:
    direct = Path(token)
    if direct.is_file():
        return direct
    filename = names.get(token)
    if filename is None:
        raise StepTextError(f"找不到输入 {token}")
    if samples is None:
        raise StepTextError(f"{token} 需要 --samples 或环境变量 STEPLENS_SAMPLES")
    path = samples / filename
    if not path.is_file():
        raise StepTextError(f"{token} 不在样例目录里: {path}")
    return path


def choose_preset(name: str, samples: Path | None, names: dict[str, str]) -> tuple[list[Path], int]:
    preset = _PRESETS.get(name)
    if preset is None:
        known = ", ".join(sorted(_PRESETS))
        raise StepTextError(f"未知 preset {name}。可用: {known}")
    tried: list[str] = []
    for recipe in preset["recipes"]:
        missing = []
        paths: list[Path] = []
        try:
            for token in recipe:
                paths.append(resolve_token(token, samples, names))
        except StepTextError as exc:
            missing.append(str(exc))
        if missing:
            tried.append(f"{'+'.join(recipe)}: {missing[0]}")
            continue
        raw = sum(path.stat().st_size for path in paths)
        if raw < preset["minimum_bytes"]:
            tried.append(f"{'+'.join(recipe)}: 原始大小 {raw} 小于 {preset['minimum_bytes']}")
            continue
        return paths, preset["minimum_bytes"]
    detail = "\n".join(tried) if tried else "没有配方"
    raise StepTextError(
        f"preset {name} 拼不出足够大的文件。需要样例目录里有对应编号。\n"
        f"复现：python3 scripts/make_big_step.py --preset {name} "
        f"--samples \"$STEPLENS_SAMPLES\" --output local/bench/{preset['default_name']}\n"
        f"{detail}"
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="用文本拼接生成更大的 STEP 文件（架构 7.5 办法一）")
    parser.add_argument("--preset", choices=sorted(_PRESETS), help="500mib 或 1gib")
    parser.add_argument("--samples", help="样例目录。默认 STEPLENS_SAMPLES")
    parser.add_argument("--inputs", nargs="*", default=[], help="文件路径或样例编号，按顺序拼接")
    parser.add_argument("--input", help="和 --repeat 一起用的单个文件或编号")
    parser.add_argument("--repeat", type=int, default=1, help="把 --input 重复几次。默认 1")
    parser.add_argument("--output", type=Path, help="输出路径。preset 默认写到 local/bench/")
    parser.add_argument("--manifest", type=Path, default=None)
    args = parser.parse_args(argv)
    try:
        if args.repeat < 1:
            raise StepTextError("--repeat 至少为 1")
        names = load_id_filenames(args.manifest or manifest_path())
        samples = _samples_dir(args.samples)
        minimum = 0
        paths: list[Path]
        if args.preset:
            if args.inputs or args.input:
                raise StepTextError("--preset 不要和 --inputs / --input 一起用")
            paths, minimum = choose_preset(args.preset, samples, names)
            output = args.output or Path("local/bench") / _PRESETS[args.preset]["default_name"]
        elif args.input:
            one = resolve_token(args.input, samples, names)
            paths = [one] * args.repeat
            output = args.output
        else:
            if not args.inputs:
                raise StepTextError("请给出 --preset、--inputs，或 --input")
            paths = [resolve_token(token, samples, names) for token in args.inputs]
            output = args.output
        if output is None:
            raise StepTextError("请用 --output 指定输出文件")
        result = concatenate(paths, output)
        if result["bytes"] < minimum:
            raise StepTextError(
                f"写出 {result['bytes']} 字节，小于 preset 要求的 {minimum} 字节: {output}"
            )
        print(
            f"output={result['output']} bytes={result['bytes']} "
            f"parts={len(result['inputs'])} max_entity_id={result['max_entity_id']}",
            flush=True,
        )
        for item in result["inputs"]:
            print(f"input={item}", flush=True)
        return 0
    except (StepTextError, OSError, json.JSONDecodeError, KeyError) as exc:
        sys.stderr.write(f"make_big_step.py 失败: {exc}\n")
        return 1


if __name__ == "__main__":
    sys.exit(main())
