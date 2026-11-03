#!/usr/bin/env python3
"""粗略扫描 STEP 文件头与实体数量级（文本计数，非精确解析）。"""
import mmap, os, re, sys, json, zipfile, io
TOK = {"entities": b"\n#", "ADVANCED_FACE": b"ADVANCED_FACE(", "B_SPLINE_SURF": b"B_SPLINE_SURFACE",
       "MANIFOLD_SOLID_BREP": b"MANIFOLD_SOLID_BREP(", "SHELL_BASED_SURF": b"SHELL_BASED_SURFACE_MODEL(", "PRODUCT": b"PRODUCT(", "NAUO": b"NEXT_ASSEMBLY_USAGE_OCCURRENCE(",
       "MAPPED_ITEM": b"MAPPED_ITEM(", "COLOUR_RGB": b"COLOUR_RGB(", "STYLED_ITEM": b"STYLED_ITEM(",
       "TESSELLATED": b"TRIANGULATED_FACE"}
def scan(name, buf):
    head = bytes(buf[:4000]).decode("latin1", "replace")
    m = re.search(r"FILE_SCHEMA\s*\(\s*\(\s*'([^']+)'", head)
    p = re.search(r"originating_system \*/\s*'([^']*)'|FILE_NAME\s*\((?:[^;]*?,){5}\s*'([^']*)'", head, re.S)
    r = {"file": name, "bytes": len(buf), "schema": m.group(1) if m else "?",
         "orig_sys": ((p.group(1) or p.group(2) or "?")[:40]) if p else "?"}
    for k, t in TOK.items():
        r[k] = buf.count(t) if k == "entities" else len(re.findall(re.escape(t[:-1]) + rb"\s*\(" if t.endswith(b"(") else re.escape(t), buf))
    return r
out = []
d = "/workspace/samples/dl"
for f in sorted(os.listdir(d)):
    if not re.search(r"\.(stp|step)$", f, re.I): continue
    with open(os.path.join(d, f), "rb") as fh:
        out.append(scan(f, fh.read()))
with zipfile.ZipFile("/workspace/samples/NIST-PMI-STEP-Files.zip") as z:
    for n in ["nist_ctc_02_asme1_ap242-e2.stp", "nist_ftc_09_asme1_ap242-e1.stp", "nist_stc_10_asme1_ap242-e2.stp"]:
        cand = [i for i in z.namelist() if i.endswith("/" + n)]
        if cand: out.append(scan(n, z.read(cand[0])))
json.dump(out, open("/workspace/design/data/step-scan.json", "w"), indent=1, ensure_ascii=False)
for r in sorted(out, key=lambda r: r["bytes"]):
    print(f"{r['file'][:34]:34} {r['bytes']/2**20:8.1f}MiB {r['schema'][:38]:38} ent={r['entities']:>9} face={r['ADVANCED_FACE']:>7} bsps={r['B_SPLINE_SURF']:>6} brep={r['MANIFOLD_SOLID_BREP']:>5} sbsm={r['SHELL_BASED_SURF']:>4} prod={r['PRODUCT']:>4} nauo={r['NAUO']:>5} col={r['COLOUR_RGB']:>6} styl={r['STYLED_ITEM']:>6} tess={r['TESSELLATED']} sys={r['orig_sys']}")
