#!/usr/bin/env python3
"""Independent GVID v3 conformance/recovery and native-writer interoperability."""
import argparse
import importlib.util
import io
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("stock_video", ROOT / "tools/stock_video.py")
video = importlib.util.module_from_spec(spec)
spec.loader.exec_module(video)


def fixture(metadata=b'{"camera":"test"}'):
    reserved = struct.pack("<III", len(metadata), 1, zlib.crc32(metadata)) + bytes(8)
    header = bytearray(video.CLIP.pack(b"GVID", 3, 64, b"VC5\0", 128, 96, 30000, 1001, 3,
                                     b"RGGB", 14, 48, reserved))
    struct.pack_into("<I", header, 56, zlib.crc32(header[:56]))
    data = bytes(header) + metadata
    payloads = (b"first-stock-payload", b"second-stock-payload")
    for i, payload in enumerate(payloads):
        fm = b'{"exposure_us":1000}'
        fh = bytearray(video.RECORD3.pack(b"FRM3", 48, i, 900_000_000_000 + i * 33410000,
                                          len(payload), len(fm), zlib.crc32(payload), zlib.crc32(fm), 0))
        struct.pack_into("<I", fh, 44, zlib.crc32(fh[:44]))
        data += bytes(fh) + fm + payload
    end = bytearray(video.RECORD3.pack(b"END3", 48, 2, 900_033_410_000,
                                       sum(map(len, payloads)), zlib.crc32(data), 0, 0, 0))
    struct.pack_into("<I", end, 36, zlib.crc32(end[:36]))
    return data + end


def parse(data, recover=False):
    f = io.BytesIO(data)
    header = video.read_header(f)
    return header, list(video.records(f, header, recover))


class V3(unittest.TestCase):
    def test_metadata_requires_unambiguous_finite_json_object(self):
        for metadata in (b'{"a":NaN}', b'{"a":1e999}', b'{"a":1,"a":2}', b'[]', b'{"a":"\xff"}'):
            with self.subTest(metadata=metadata), self.assertRaises(ValueError):
                parse(fixture(metadata))
    def test_metadata_timestamps_finalization(self):
        header, rows = parse(fixture())
        self.assertEqual(header["metadata"], {"camera": "test"})
        self.assertEqual((header["fps_num"], header["fps_den"]), (30000, 1001))
        self.assertEqual(rows[1]["pts_ns"], 900_033_410_000)
        self.assertEqual(rows[0]["metadata"], {"exposure_us": 1000})
        self.assertTrue(header["_complete"] and header["_end_marker_verified"])

    def test_every_truncation_rejected(self):
        data = fixture()
        for n in range(len(data)):
            with self.subTest(length=n), self.assertRaises(ValueError):
                parse(data[:n])

    def test_every_single_byte_corruption_rejected(self):
        data = fixture()
        for i in range(len(data)):
            bad = bytearray(data)
            bad[i] ^= 1
            with self.subTest(offset=i), self.assertRaises(ValueError):
                parse(bad)

    def test_recover_prefix_does_not_claim_complete(self):
        data = fixture()
        header, rows = parse(data[:-48], recover=True)
        self.assertEqual(len(rows), 2)
        self.assertFalse(header["_complete"] or header["_end_marker_verified"])
        self.assertIn("missing END3", header["_recovery_error"])
        header, rows = parse(data[:-52], recover=True)
        self.assertEqual(len(rows), 1)
        self.assertFalse(header["_complete"])

    def test_trailing_bytes_and_removed_whole_record(self):
        data = fixture()
        with self.assertRaises(ValueError):
            parse(data + b"extra")
        offset = 64 + len(b'{"camera":"test"}')
        first_size = 48 + len(b'{"exposure_us":1000}') + len(b"first-stock-payload")
        with self.assertRaises(ValueError):
            parse(data[:offset+first_size] + data[-48:])

    def test_crc32_combine_algebraic_equivalence(self):
        for a, b in ((b"header-bytes", b"payload-bytes"), (b"", b"nonempty"), (b"x" * 1024, b"y" * 4096)):
            c1 = zlib.crc32(a)
            c2 = zlib.crc32(b)
            combined = video.crc32_combine(c1, c2, len(b))
            self.assertEqual(combined, zlib.crc32(a + b))

    def test_scan_index_and_o1_seek_record(self):
        data = fixture()
        f = io.BytesIO(data)
        header = video.read_header(f)
        entries = video.scan_index(f, header)
        self.assertEqual(len(entries), 2)
        self.assertTrue(header["_complete"] and header["_end_marker_verified"])
        rec1 = video.seek_record(f, header, entries[1])
        self.assertEqual(rec1["index"], 1)
        self.assertEqual(rec1["pts_ns"], 900_033_410_000)
        self.assertEqual(rec1["payload"], b"second-stock-payload")
        self.assertEqual(rec1["metadata"], {"exposure_us": 1000})
        # Corrupting payload byte in frame 1 must fail seek_record checksum verification
        bad = bytearray(data)
        bad[entries[1]["payload_offset"]] ^= 0xFF
        with self.assertRaises(ValueError):
            video.seek_record(io.BytesIO(bad), header, entries[1])

    def test_clip_static_and_per_frame_calibration_inheritance(self):
        header = {
            "metadata": {
                "raw_metadata": {"black_level": [256, 256, 256, 256], "white_level": 16383, "iso": 100},
                "optical_stackup": {"model": "mission1_polar_24coeff_v1", "gain_db": 0.2455},
            }
        }
        frame_meta = {"raw_metadata_overrides": {"iso": 400, "exposure_us": 8333}}
        resolved = video.resolve_frame_calibration(header, frame_meta)
        self.assertEqual(resolved["black_level"], [256, 256, 256, 256])
        self.assertEqual(resolved["iso"], 400)
        self.assertEqual(resolved["exposure_us"], 8333)
        self.assertEqual(resolved["optical_stackup"]["model"], "mission1_polar_24coeff_v1")


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--native-writer", type=Path)
    args = p.parse_args()
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(V3))
    if not result.wasSuccessful():
        raise SystemExit(1)
    if args.native_writer:
        with tempfile.TemporaryDirectory() as directory:
            clip = Path(directory) / "native.gvid"
            subprocess.run([str(args.native_writer.resolve()), str(clip)], check=True)
            header, rows = parse(clip.read_bytes())
            assert header["_end_marker_verified"] and len(rows) == 2
            assert header["metadata"]["camera"] == "test"
            assert rows[1]["pts_ns"] == 900_033_410_000
            assert rows[0]["metadata"] == {"exposure_us": 1000}
            print("PASS: independent Python reader validates native C/C++ capture output")
