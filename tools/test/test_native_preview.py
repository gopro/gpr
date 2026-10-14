#!/usr/bin/env python3
"""Small native preview regression tests; never builds binaries.

BUILD_DIR=/absolute/build python3 tools/test/test_native_preview.py
Optional --reference-bin compares valid RGB bytes against a baseline CLI.
Explicit --bench-bin uses the legacy benchmark fixture writer.
"""

import argparse
import errno
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
BUILD = Path(os.environ.get("BUILD_DIR", ROOT / "build")).resolve()
OPTIONS = None
WIDTH, HEIGHT = 256, 128


def run(command, env):
    return subprocess.run(
        [str(arg) for arg in command], env=env, capture_output=True, timeout=60
    )


class NativePreviewTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory(prefix="gpr-native-preview-")
        cls.addClassCleanup(cls.work.cleanup)
        cls.root = Path(cls.work.name)
        # Inherited codec tuning must not alter the fixture's wire format.
        cls.env = {
            k: v for k, v in os.environ.items()
            if not k.startswith(("GPR_", "FUSED_"))
        }
        raw = cls.root / "input.raw"
        samples = [
            (2048 + x * 9 + y * 13 + ((x * y) & 255)) & 4095
            for y in range(HEIGHT) for x in range(WIDTH)
        ]
        raw.write_bytes(struct.pack(f"<{len(samples)}H", *samples))
        cls.clips = {}
        for pf in (0, 1, 4):
            clip = cls.root / f"valid-{pf}.gvid"
            env = dict(cls.env, GPR_BENCH_GVID=str(clip),
                       GPR_BENCH_PIXEL_FORMAT=str(pf), GPR_INCLUDE_LL="1",
                       FUSED_MULTI_LEVEL="0", GPR_COL_DECIMATE="1",
                       GPR_ROW_DECIMATE="1", FUSED_QUALITY="3", FUSED_THREADS="1")
            command = ([OPTIONS.bench_bin, raw, WIDTH, HEIGHT, 2]
                       if OPTIONS.bench_bin else
                       [OPTIONS.fixture_bin, raw, WIDTH, HEIGHT, pf, clip])
            result = run(command, env)
            if result.returncode != 0:
                raise RuntimeError(f"fixture writer failed: {result.stderr.decode(errors='replace')}")
            data = clip.read_bytes()
            if data[:4] != b"GVID" or struct.unpack_from("<I", data, 28)[0] != 2:
                raise RuntimeError("fixture writer did not emit the expected two-frame GVID")
            cls.clips[pf] = data
        cls.valid = cls.clips[0]
        cls.second = 48 + struct.unpack_from("<I", cls.valid, 36)[0]

    def setUp(self):
        self.work_case = tempfile.TemporaryDirectory(dir=self.root)
        self.addCleanup(self.work_case.cleanup)
        self.case = Path(self.work_case.name)
        self.serial = 0

    def invoke(self, data=None, *, width=WIDTH, height=HEIGHT, limit=2,
               write=1, out=None, name="input.gvid", binary=None):
        self.serial += 1
        clip = self.case / name
        clip.write_bytes(self.valid if data is None else data)
        out = out if out is not None else self.case / f"out-{self.serial}"
        result = run([binary or OPTIONS.preview_bin, clip, width, height,
                      out, limit, write], self.env)
        return result, out, clip

    def success(self, *args, **kwargs):
        result, out, clip = self.invoke(*args, **kwargs)
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
        receipt = json.loads((out / "receipt.json").read_text(encoding="utf-8"))
        self.assertEqual(receipt["gvid"], str(clip))
        self.assertEqual((receipt["width"], receipt["height"]), (WIDTH // 4, HEIGHT // 4))
        return out, receipt

    def rejected(self, *args, **kwargs):
        result, out, _ = self.invoke(*args, **kwargs)
        self.assertGreater(result.returncode, 0, "must fail without a signal/crash")
        self.assertTrue(result.stderr, "failure should have a diagnostic")
        self.assertFalse((out / "receipt.json").is_file(), "must not emit a success receipt")

    def mutate(self, offset, fmt, value):
        data = bytearray(self.valid)
        struct.pack_into(fmt, data, offset, value)
        return data

    def test_valid_full_rgb_frames(self):
        for pf, data in self.clips.items():
            with self.subTest(pixel_format=pf):
                out, receipt = self.success(data)
                self.assertEqual(receipt["frames"], 2)
                frames = sorted((out / "frames_rgb").glob("*.rgb"))
                self.assertEqual([p.name for p in frames], ["frame_000000.rgb", "frame_000001.rgb"])
                pixels = frames[0].read_bytes()
                self.assertEqual(len(pixels), (WIDTH // 4) * (HEIGHT // 4) * 3)
                self.assertEqual(pixels, frames[1].read_bytes())
                self.assertGreater(len(set(pixels)), 1)
                stride = WIDTH // 4 * 3
                for y in range(0, HEIGHT // 4, 2):
                    row = pixels[y * stride:(y + 1) * stride]
                    self.assertEqual(row, pixels[(y + 1) * stride:(y + 2) * stride])
                    for x in range(0, stride, 6):
                        self.assertEqual(row[x:x + 3], row[x + 3:x + 6])
                if OPTIONS.reference_bin:
                    reference, _ = self.success(data, binary=OPTIONS.reference_bin)
                    for frame in frames:
                        self.assertEqual(frame.read_bytes(),
                                         (reference / "frames_rgb" / frame.name).read_bytes())

    def test_fixture_rejects_malformed_inputs(self):
        if OPTIONS.bench_bin:
            self.skipTest("legacy benchmark fixture writer selected")
        raw = self.root / "input.raw"
        output = self.case / "fixture.gvid"
        cases = [[], [raw, WIDTH, HEIGHT, 0]]
        for value in ("", "junk", "256junk", "-128", "+256", " 256", "0",
                      "127", "255", "1025", "99999999999999999999"):
            cases.append([raw, value, HEIGHT, 0, output])
            cases.append([raw, WIDTH, value, 0, output])
        for pf in ("", "junk", "-1", "2", "3", "5", "4x"):
            cases.append([raw, WIDTH, HEIGHT, pf, output])
        for name, data in (("empty.raw", b""), ("short.raw", b"\0" * 4),
                           ("long.raw", raw.read_bytes() + b"\0")):
            malformed = self.case / name
            malformed.write_bytes(data)
            cases.append([malformed, WIDTH, HEIGHT, 0, output])
        cases.append([self.case / "missing.raw", WIDTH, HEIGHT, 0, output])
        for args in cases:
            with self.subTest(args=args):
                result = run([OPTIONS.fixture_bin, *args], self.env)
                self.assertGreater(result.returncode, 0, "must reject without a crash")
                self.assertTrue(result.stderr)
                self.assertFalse(output.exists())

    def test_fixture_output_failures(self):
        if OPTIONS.bench_bin:
            self.skipTest("legacy benchmark fixture writer selected")
        outputs = [self.case]  # A directory cannot be opened as a file.
        if Path("/dev/full").exists():
            outputs.append(Path("/dev/full"))
        for output in outputs:
            with self.subTest(output=output):
                result = run([OPTIONS.fixture_bin, self.root / "input.raw",
                              WIDTH, HEIGHT, 0, output], self.env)
                self.assertGreater(result.returncode, 0)
                self.assertTrue(result.stderr)

    def test_measure_only_and_json_paths(self):
        out, receipt = self.success(write=0, name='quote"back\\tab\tline\n\u00e9.gvid')
        self.assertEqual(receipt["frames"], 2)
        self.assertEqual(receipt["write_frames"], 0)
        self.assertFalse((out / "frames_rgb").exists())

    def test_frame_tags_and_trailing_bytes(self):
        self.rejected(self.mutate(self.second + 8, "<Q", 0))
        self.rejected(self.valid + b"extra")

    def test_invalid_utf8_path_still_has_valid_json(self):
        try:
            result, out, _ = self.invoke(name=os.fsdecode(b"invalid-\xff.gvid"), write=0)
        except OSError as error:
            if error.errno == errno.EILSEQ:
                self.skipTest("filesystem rejects invalid UTF-8 filenames")
            raise
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads((out / "receipt.json").read_text())["frames"], 2)

    def test_arguments_and_dimensions(self):
        for value in ("", "abc", "256junk", "-8", "0", "7", "12", "255", "260",
                      "8193", "2147483648", "9999999999999999999999", "+256", " 256"):
            for dimension in ("width", "height"):
                with self.subTest(value=value, dimension=dimension):
                    self.rejected(**{dimension: value})
        self.rejected(width=128)  # Valid alignment, wrong caller dimensions.
        self.rejected(width=8192, height=8192)
        for value in ("-1", "junk", "1x", "1000001", "4294967295"):
            with self.subTest(limit=value):
                self.rejected(limit=value)
        for value in ("-1", "2", "junk", "1x"):
            with self.subTest(write=value):
                self.rejected(write=value)
        result, _, _ = self.invoke(out=self.case / ("x" * 1100))
        self.assertGreater(result.returncode, 0)
        self.assertIn(b"output path too long", result.stderr)

    def test_clip_headers(self):
        for offset, fmt, value in (
            (0, "<I", 0), (4, "B", 2), (5, "B", 4), (5, "B", 1),
            (6, "<H", 6), (6, "<H", 2), (6, "<H", 3), (6, "<H", 5),
            (8, "<H", 12), (10, "<H", 1), (12, "<I", 0),
            (12, "<I", 0xffffffff), (16, "<I", 136),
            (20, "<I", 0), (24, "<I", 1), (28, "<I", 0xffffffff),
        ):
            with self.subTest(offset=offset, value=value):
                self.rejected(self.mutate(offset, fmt, value), limit=0)

    def test_fused_headers_before_decode(self):
        # Mutate both the first frame and a frame following a successful decode.
        for start in (48, self.second + 16):
            for offset, value in (
                (0, 0), (4, 2), (8, 0), (8, WIDTH * 2), (8, 0xffffffff),
                (12, 0), (12, HEIGHT * 2), (12, 0xffffffff), (16, 2),
                (20, 12), (24, 0), (24, 2), (28, 0), (28, 16), (28, 32),
                (32, 1), (36, 1), (40, 12), (40, 0xffffffff), (44, 2), (44, 3),
            ):
                with self.subTest(start=start, offset=offset, value=value):
                    self.rejected(self.mutate(start + offset, "<I", value))

    def test_frame_headers_and_band_bounds(self):
        for start in (32, self.second):
            for offset, value in ((0, 0), (4, 0), (4, 47), (4, 111),
                                  (4, 128 * 1024 * 1024 + 1), (4, 0xffffffff),
                                  (16 + 48, 0xffffffff)):
                with self.subTest(start=start, offset=offset, value=value):
                    self.rejected(self.mutate(start + offset, "<I", value))

    def test_truncation_and_frame_limit(self):
        for length in (0, 4, 31, 32, 33, 47, 48, 95, self.second - 1,
                       self.second, self.second + 1, self.second + 15,
                       self.second + 16, len(self.valid) - 1):
            for limit in (0, 2):
                with self.subTest(length=length, limit=limit):
                    self.rejected(self.valid[:length], limit=limit)
        out, receipt = self.success(self.valid[:self.second], limit=1)
        self.assertEqual(receipt["frames"], 1)
        self.assertEqual(len(list((out / "frames_rgb").glob("*.rgb"))), 1)
        self.rejected(limit=3)
        unknown = self.mutate(28, "<I", 0)
        _, receipt = self.success(unknown, limit=0)
        self.assertEqual(receipt["frames"], 2)
        self.rejected(unknown[:-1], limit=0)
        self.rejected(unknown[:32], limit=0)
        self.rejected(unknown[:self.second] + b"F", limit=0)

    def test_output_open_failures(self):
        out = self.case / "not-a-directory"
        out.write_text("occupied")
        self.rejected(out=out)
        out = self.case / "bad-frame"
        (out / "frames_rgb" / "frame_000000.rgb").mkdir(parents=True)
        self.rejected(out=out)
        out = self.case / "bad-receipt"
        (out / "receipt.json").mkdir(parents=True)
        self.rejected(out=out, write=0)

    @unittest.skipUnless(Path("/dev/full").exists(), "requires /dev/full")
    def test_buffered_write_failures(self):
        for frame_output in (False, True):
            with self.subTest(frame_output=frame_output):
                out = self.case / str(frame_output)
                out.mkdir()
                target = out / "receipt.json"
                if frame_output:
                    (out / "frames_rgb").mkdir()
                    target = out / "frames_rgb" / "frame_000000.rgb"
                target.symlink_to("/dev/full")
                self.rejected(out=out, write=int(frame_output))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--preview-bin", type=Path, default=BUILD / "bin/gvid_preview_rgb_cli")
    fixtures = parser.add_mutually_exclusive_group()
    fixtures.add_argument("--fixture-bin", type=Path)
    fixtures.add_argument("--bench-bin", type=Path,
                          help="explicit legacy bench_fused fixture writer")
    parser.add_argument("--reference-bin", type=Path)
    OPTIONS, unittest_args = parser.parse_known_args()
    if OPTIONS.fixture_bin is None and OPTIONS.bench_bin is None:
        OPTIONS.fixture_bin = BUILD / "bin/test_preview_fixture"
    for key in ("preview_bin", "fixture_bin", "bench_bin", "reference_bin"):
        value = getattr(OPTIONS, key)
        if value is not None:
            value = value.resolve()
            if not value.is_file() or not os.access(value, os.X_OK):
                parser.error(f"{key}: executable not found: {value}")
            setattr(OPTIONS, key, value)
    unittest.main(argv=[__file__, *unittest_args])
