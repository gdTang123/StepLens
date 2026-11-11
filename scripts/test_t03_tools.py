#!/usr/bin/env python3
"""fetch_samples.py 和 make_big_step.py 的离线测试。不访问网络。"""

from __future__ import annotations

import contextlib
import hashlib
import io
import json
import os
import re
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fetch_samples
import make_big_step

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "scripts" / "samples_manifest.json"
CATALOG = ROOT / "tools" / "bench" / "SampleCatalog.h"


def _step(header_token: str, body: str) -> bytes:
    return (
        "ISO-10303-21;\n"
        "HEADER;\n"
        f"FILE_NAME('{header_token}','2026-11-11T00:00:00',(''),(''),'','','');\n"
        "FILE_SCHEMA(('AUTOMOTIVE_DESIGN'));\n"
        "ENDSEC;\n"
        "DATA;\n"
        f"{body}\n"
        "ENDSEC;\n"
        "END-ISO-10303-21;\n"
    ).encode("ascii")


class ManifestTests(unittest.TestCase):
    def test_catalog_matches_manifest(self) -> None:
        document = json.loads(MANIFEST.read_text(encoding="utf-8"))
        samples = document["samples"]
        self.assertEqual(len(samples), 36)
        ids = [item["id"] for item in samples]
        self.assertEqual(len(ids), len(set(ids)))
        pairs = re.findall(r'\{"([^"]+)", "([^"]+)"\}', CATALOG.read_text(encoding="utf-8"))
        self.assertEqual([(item["id"], item["filename"]) for item in samples], pairs)
        by_id = {item["id"]: item for item in samples}
        self.assertEqual(by_id["X-02"]["bytes"], 374345560)
        self.assertEqual(
            by_id["X-02"]["sha256"],
            "3b02d0b151aa393db2a9c741b243ca34b05b973619cbe638f9a1d00cffede818",
        )
        self.assertEqual(
            by_id["X-01"]["sha256"],
            "573aa7da05462dee25a430b28a6869906775ae5e0126308956661c1e1da421b7",
        )
        self.assertEqual(
            by_id["L-03"]["sha256"],
            "c64a4ad7540ab80f7db072781e8eab034bdbe8f0506a2421dbdf139a5f9e998c",
        )
        self.assertEqual(
            by_id["X-07"]["sha256"],
            "0b4e8039a6612c003f38ca3e36f645dfede2f56aa0ba4623b1e0557182cb9a9b",
        )
        self.assertEqual(by_id["S-08"]["member"], "NIST-PMI-STEP-Files/nist_ctc_02_asme1_ap242-e2.stp")
        for item in samples:
            self.assertEqual(len(item["sha256"]), 64)
            int(item["sha256"], 16)
            self.assertIsInstance(item["bytes"], int)
            self.assertGreater(item["bytes"], 0)


class ConcatTests(unittest.TestCase):
    def test_offsets_ids_and_keeps_first_header(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            first = root / "a.step"
            second = root / "b.step"
            output = root / "out.step"
            first.write_bytes(_step("FIRST_HEADER_TOKEN", "#1=A(1);\n#2=B(#1);"))
            second.write_bytes(_step("SECOND_HEADER_TOKEN", "#1=C('#2');\n#5=D(#1);"))
            result = make_big_step.concatenate([first, second], output)
            blob = output.read_bytes()
            self.assertIn(b"FIRST_HEADER_TOKEN", blob)
            self.assertNotIn(b"SECOND_HEADER_TOKEN", blob)
            self.assertEqual(blob.count(b"DATA;"), 1)
            self.assertIn(b"#1=A(1);", blob)
            # 字符串里的 #2 也会被改成 #4。这是办法一的已知局限。
            self.assertIn(b"#3=C('#4');", blob)
            self.assertIn(b"#7=D(#3);", blob)
            definitions = [int(item) for item in re.findall(br"#(\d+)\s*=", blob)]
            self.assertEqual(len(definitions), len(set(definitions)))
            self.assertEqual(result["max_entity_id"], 7)
            self.assertGreater(result["bytes"], 0)

    def test_preset_without_samples_names_the_missing_inputs(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                code = make_big_step.main(
                    ["--preset", "1gib", "--samples", tmp, "--output", str(Path(tmp) / "big.step")]
                )
        self.assertEqual(code, 1)
        self.assertIn("X-02", stderr.getvalue())


class FetchTests(unittest.TestCase):
    def test_missing_destination_is_usage_error(self) -> None:
        previous = os.environ.pop("STEPLENS_SAMPLES", None)
        try:
            with self.assertRaises(fetch_samples.UsageError):
                fetch_samples.resolve_destination(None)
        finally:
            if previous is not None:
                os.environ["STEPLENS_SAMPLES"] = previous

    def test_file_url_and_zip_member_are_checked(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            payload = b"step-lens-sample"
            source = root / "source.bin"
            source.write_bytes(payload)
            digest = hashlib.sha256(payload).hexdigest()
            archive_dir = root / "zip-src"
            archive_dir.mkdir()
            member_bytes = b"nist-member"
            archive_path = root / "pack.zip"
            with zipfile.ZipFile(archive_path, "w") as archive:
                archive.writestr("nested/piece.stp", member_bytes)
            archive_sha = hashlib.sha256(archive_path.read_bytes()).hexdigest()
            member_sha = hashlib.sha256(member_bytes).hexdigest()
            manifest = {
                "archives": {
                    "pack": {
                        "filename": "pack.zip",
                        "url": archive_path.as_uri(),
                        "bytes": archive_path.stat().st_size,
                        "sha256": archive_sha,
                    }
                },
                "samples": [
                    {
                        "id": "S-01",
                        "filename": "direct.bin",
                        "url": source.as_uri(),
                        "bytes": len(payload),
                        "sha256": digest,
                    },
                    {
                        "id": "S-08",
                        "filename": "piece.stp",
                        "bytes": len(member_bytes),
                        "sha256": member_sha,
                        "archive": "pack",
                        "member": "nested/piece.stp",
                    },
                ],
            }
            dest = root / "out"
            self.assertEqual(fetch_samples.fetch_samples(dest, manifest, None), 0)
            self.assertEqual((dest / "direct.bin").read_bytes(), payload)
            self.assertEqual((dest / "piece.stp").read_bytes(), member_bytes)
            (dest / "direct.bin").write_bytes(b"corrupt")
            self.assertEqual(fetch_samples.fetch_samples(dest, manifest, {"S-01"}), 0)
            self.assertEqual((dest / "direct.bin").read_bytes(), payload)

    def test_hash_mismatch_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "source.bin"
            source.write_bytes(b"actual")
            manifest = {
                "archives": {},
                "samples": [
                    {
                        "id": "S-01",
                        "filename": "direct.bin",
                        "url": source.as_uri(),
                        "bytes": 6,
                        "sha256": "0" * 64,
                    }
                ],
            }
            with self.assertRaises(RuntimeError):
                fetch_samples.fetch_samples(root / "out", manifest, None)


if __name__ == "__main__":
    unittest.main()
