#!/usr/bin/env python3
"""把 T02b 的 results/<Tag>/*.txt 汇总成 spike-m0b.md 用的 Markdown 表格。
用法：python summarize.py results/801 results/793  > tables.md   （Windows 或盒子上都能跑，只用标准库）"""
import sys, pathlib, statistics as st

def load(p):
    d = {}
    text = p.read_text(encoding="utf-8-sig", errors="replace")
    for line in text.splitlines():
        if "=" in line:
            k, v = line.split("=", 1); d[k.strip()] = v.strip()
    return d

def env_notes(tagdir):
    p = tagdir / "env.txt"
    if not p.exists():
        return []
    keep = ("pdb_missing=", "skipped=", "transfer_time_s_from=", "transfer_time_s=")
    notes = []
    for line in p.read_text(encoding="utf-8-sig", errors="replace").splitlines():
        stripped = line.strip()
        if stripped.startswith(keep):
            notes.append(stripped)
    return notes

def f(d, k, nd=1):
    try: return f"{float(d[k]):.{nd}f}"
    except (KeyError, ValueError): return "—"

def peak(d):
    v = [float(d[k]) for k in ("read_peak_rss_mb", "transfer_peak_rss_mb", "peak_rss_process_mb") if k in d]
    return f"{max(v):.0f}" if v else "—"

def status(d, err):
    code = d.get("exit", "?")
    crashed = "crash_frame_00" in err or code not in ("0", "?")
    top = next((l.split("=", 1)[1] for l in err.splitlines() if l.startswith("crash_frame_00")), "")
    return ("崩溃 " + code + (" @ " + top if top else "")) if crashed else "成功"

for tagdir in map(pathlib.Path, sys.argv[1:]):
    tag = tagdir.name
    runs = {p.stem: (load(p), (p.with_suffix(".err").read_text(encoding="utf-8", errors="replace") if p.with_suffix(".err").exists() else ""))
            for p in sorted(tagdir.glob("*.txt")) if p.stem not in ("test_compat", "env")}
    ver = next((d.get("occt_version_at_start") for d, _ in runs.values() if d.get("occt_version_at_start")), tag)
    tc = (tagdir / "test_compat.txt")
    print(f"\n### OCCT {ver}（{tag}）\n\n兼容单测：{'PASS' if tc.exists() and 'RESULT=PASS' in tc.read_text(encoding='utf-8-sig', errors='replace') else '未通过/未运行'}\n")
    notes = env_notes(tagdir)
    if notes:
        print("旁注（来自 env.txt）：")
        for line in notes:
            print(f"- `{line}`")
        print()
    print("| 运行 | 结果 | 读取 s | 转换 s | 释放 s | 方案B网格 s | 合计 s | 读取峰值工作集 MB | 全程峰值工作集 MB | 全程峰值提交 MB | 最长 UserBreak 间隔 s | 原型数 |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|")
    # 注：合计 s 含粗/细/方案B 三级网格（与 M0 报告的 total_time_s 口径一致）
    for k, (d, e) in runs.items():
        if k.startswith("cancel"): continue
        print(f"| {k} | {status(d, e)} | {f(d,'read_time_s')} | {f(d,'transfer_time_s')} | {f(d,'release_reader_s')} | {f(d,'mesh_planB_time_s')} | "
              f"{f(d,'total_time_s')} | {f(d,'read_peak_rss_mb',0)} | {peak(d)} | {f(d,'peak_commit_process_mb',0)} | "
              f"{f(d,'transfer_max_userbreak_gap_s',2)} | {d.get('prototypes','—')} |")
    for s in ("X-02", "L-03"):
        tt = [float(d["total_time_s"]) for k, (d, _) in runs.items() if k.startswith(s + "-r") and "total_time_s" in d]
        if tt: print(f"\n{s} 合计中位数：**{st.median(tt):.1f} s**（{len(tt)} 次）", end="")
    print("\n\n| 取消测试 | 结果 | 阶段 | 请求时刻 s | 返回延迟 ms | 释放 ms | 释放后工作集 MB |")
    print("|---|---|---|---|---|---|---|")
    for k, (d, e) in runs.items():
        if not k.startswith("cancel"): continue
        lat = d.get("cancel_latency_readstream_return_ms") or d.get("cancel_latency_transfer_return_ms") or "—"
        stage = d.get("cancel_stage") or "—"
        print(f"| {k} | {status(d, e)} | {stage} | {f(d,'cancel_req_at_s')} | {lat} | {f(d,'cancel_release_ms',0)} | {f(d,'rss_after_release_mb',0)} |")
