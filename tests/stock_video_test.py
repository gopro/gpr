#!/usr/bin/env python3
"""Container rejection and end-to-end sequence/stock-payload checks."""
import argparse
import importlib.util
import io
import os
from pathlib import Path
import random
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("stock_video", ROOT / "tools/stock_video.py")
video = importlib.util.module_from_spec(spec)
spec.loader.exec_module(video)


def clip(payload=b"test"):
    return (video.CLIP.pack(b"GVID", 2, 64, b"VC5\0", 128, 96, 30, 1, 4,
                            b"RGGB", 14, 32, bytes(20))
            + video.FRAME.pack(b"FRM2", 32, 0, 0, len(payload)) + payload)


def parse(data):
    stream = io.BytesIO(data)
    header = video.read_header(stream)
    return list(video.frames(stream, header))


class Container(unittest.TestCase):
    def test_roundtrip(self):
        self.assertEqual(parse(clip()), [(0, 0, b"test")])

    def test_reject_truncation(self):
        data = clip()
        for n in range(len(data)):
            with self.subTest(length=n), self.assertRaises(ValueError):
                parse(data[:n])

    def test_reject_wrong_profile_and_records(self):
        for offset, value in ((0, 0), (4, 1), (8, 0), (28, 6), (32, 0),
                              (44, 1), (64, 0), (68, 0), (72, 1), (80, 1), (88, 0)):
            data = bytearray(clip())
            data[offset] = value
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                parse(data)
        with self.assertRaises(ValueError):
            parse(clip() + clip()[64:])  # Duplicate frame number.


def integration(binary, reference):
    with tempfile.TemporaryDirectory(prefix="gpr-stock-video-test-") as work:
        work = Path(work)
        rng = random.Random(73)
        paths = []
        for i in range(2):
            path = work / f"{i}.raw"
            path.write_bytes(struct.pack("<" + "H" * (128*96),
                             *(rng.randrange(16384) for _ in range(128*96))))
            paths.append(path)
        for quality in range(6):
            common = [str(binary), "--width", "128", "--height", "96", "--frames", "4",
                      "--arena-mib", "16", "--quality", str(quality),
                      "--input", str(paths[0]), "--input", str(paths[1])]
            outputs = []
            for arena in (True, False):
                output = work / f"q{quality}-{arena}.gvid"
                command = common + ["--output", str(output)]
                if not arena:
                    command += ["--arena-mib", "0"]
                subprocess.run(command, check=True, stdout=subprocess.DEVNULL)
                outputs.append(parse(output.read_bytes()))
            assert outputs[0] == outputs[1], "arena changes payload"
            for i, _, payload in outputs[0]:
                assert payload == outputs[0][i % 2][2], "reused workspace changes repeated frame"
                vc5 = work / f"q{quality}-{i}.vc5"
                vc5.write_bytes(payload)
                raw = vc5.with_suffix(".decoded.raw")
                subprocess.run([str(binary), "--decode", str(vc5), "--output", str(raw),
                                "--width", "128", "--height", "96"], check=True)
                assert raw.stat().st_size == 128*96*2
                if reference:
                    stock = work / f"stock-q{quality}-{i}.vc5"
                    env = {k: v for k, v in os.environ.items() if not k.startswith("GPR_")}
                    env.update(GPR_BENCH_QUALITY=str(quality))
                    subprocess.run([str(reference), "-i", str(paths[i % 2]), "-o", str(stock),
                                    "-w", "128", "-h", "96", "-p", "256"],
                                   env=env, check=True, stdout=subprocess.DEVNULL)
                    assert payload == stock.read_bytes(), "stock reference payload mismatch"
            # A failed/truncated read must not be reported as a valid raw frame.
            bad = work / f"bad-{quality}.vc5"
            bad.write_bytes(b"bad")
            result = subprocess.run([str(binary), "--decode", str(bad), "--output", str(work / "bad.raw")],
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            assert result.returncode != 0
        print("PASS: six qualities, two inputs, four-frame sequences; arena/malloc, repeat, decode"
              + (", independent stock reference" if reference else ""))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--reference", type=Path)
    args = parser.parse_args()
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(Container)
    if not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful():
        raise SystemExit(1)
    if args.binary:
        integration(args.binary.resolve(), args.reference.resolve() if args.reference else None)
