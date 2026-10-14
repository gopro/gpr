"""Header-only GVID indexing and standalone v1 validator conformance."""
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

# PYTHONPATH may select a production worktree before changes are integrated.
sys.path.append(str(Path(__file__).resolve().parents[1]))

from gvid_metadata import read_gvid_frame_tags, read_gvid_frames
from render_gvid_preview_rgb_stream import read_gvid_frames as preview_reader
from verify_labs_bundle import validate_gvid


def clip_header(**changes):
    fields = dict(
        magic=0x44495647, version=1, flags=0, pixel_format=0, quality=0,
        reserved2=0, width=4096, height=3072, fps_x1000=24000,
        target_kbps=0, frame_count_hint=0,
    )
    fields.update(changes)
    return struct.pack("<IBBHHHIIIII", *fields.values())


def frame(tag=0, payload=b"abc", magic=0x004D5246):
    return struct.pack("<IIQ", magic, len(payload), tag) + payload


class GvidReaderTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.path = Path(temporary.name) / "sample.gvid"

    def assert_rejected(self, data):
        self.path.write_bytes(data)
        with self.assertRaises(ValueError):
            read_gvid_frames(self.path)
        with self.assertRaises(ValueError):
            validate_gvid(self.path)

    def test_preview_uses_canonical_reader(self):
        self.assertIs(preview_reader, read_gvid_frames)

    def test_valid_frames_preserve_list_keys_offsets_and_tags(self):
        for count in (0, 2):
            with self.subTest(count=count):
                self.path.write_bytes(clip_header(frame_count_hint=count) + frame(3) + frame(9, b"hello"))
                self.assertEqual(read_gvid_frames(self.path), [
                    {"frame_index": 0, "frame_tag": 3, "payload_offset": 48, "payload_size": 3},
                    {"frame_index": 1, "frame_tag": 9, "payload_offset": 67, "payload_size": 5},
                ])
                self.assertEqual(read_gvid_frame_tags(self.path), [3, 9])
                self.assertEqual(validate_gvid(self.path)["frame_count"], 2)

    def test_valid_header_boundaries(self):
        for flags in range(4):
            for pixel_format in (0, 5):
                for quality in (0, 11):
                    with self.subTest(flags=flags, pixel_format=pixel_format, quality=quality):
                        self.path.write_bytes(clip_header(
                            flags=flags, pixel_format=pixel_format, quality=quality,
                            target_kbps=1000 if flags & 1 else 0,
                            width=1, height=1, fps_x1000=1, frame_count_hint=1,
                        ) + frame((1 << 64) - 1, b"x"))
                        self.assertEqual(len(read_gvid_frames(self.path)), 1)
                        self.assertEqual(validate_gvid(self.path)["frame_count"], 1)

    def test_invalid_headers_match_standalone_validator(self):
        cases = [
            {"magic": 0}, {"version": 0}, {"version": 2},
            {"flags": 4}, {"flags": 128}, {"pixel_format": 6},
            {"quality": 12}, {"reserved2": 1}, {"width": 0},
            {"height": 0}, {"fps_x1000": 0},
            {"flags": 1, "target_kbps": 0},
            {"flags": 0, "target_kbps": 1},
            {"flags": 2, "target_kbps": 1},
            {"flags": 3, "target_kbps": 0},
            {"frame_count_hint": 2},
        ]
        for changes in cases:
            with self.subTest(changes=changes):
                self.assert_rejected(clip_header(**changes) + frame())

    def test_truncated_clip_headers(self):
        for length in range(32):
            with self.subTest(length=length):
                self.assert_rejected(clip_header()[:length])

    def test_invalid_frames_match_standalone_validator(self):
        cases = {
            "no frames": b"",
            "bad magic": frame(magic=0),
            "empty payload": frame(payload=b""),
            "missing payload": struct.pack("<IIQ", 0x004D5246, 1, 0),
            "short payload": frame()[:-1],
            "oversized payload": struct.pack("<IIQ", 0x004D5246, 0xFFFFFFFF, 0) + b"x",
            "duplicate tags": frame(2) + frame(2),
            "decreasing tags": frame(2) + frame(1),
        }
        for name, body in cases.items():
            with self.subTest(name=name):
                self.assert_rejected(clip_header() + body)
        for length in range(1, 16):
            with self.subTest(truncated_frame_header=length):
                self.assert_rejected(clip_header() + frame() + frame(1)[:length])
        self.assert_rejected(clip_header(frame_count_hint=1) + frame(0) + frame(1))

    def test_sparse_payloads_are_never_read(self):
        payload_size = 1024 * 1024
        frame_count = 3
        expected_reads = [(0, 32)]
        with self.path.open("wb") as stream:
            stream.write(clip_header(frame_count_hint=frame_count))
            for index in range(frame_count):
                expected_reads.append((stream.tell(), 16))
                stream.write(struct.pack("<IIQ", 0x004D5246, payload_size, index))
                stream.seek(payload_size, 1)
            stream.truncate()

        actual_reads = []
        real_open = Path.open
        test = self

        class HeaderOnlyFile:
            def __init__(self, stream):
                self.stream = stream

            def __enter__(self):
                return self

            def __exit__(self, *args):
                self.stream.close()

            def seek(self, *args):
                return self.stream.seek(*args)

            def tell(self):
                return self.stream.tell()

            def read(self, size=-1):
                request = (self.stream.tell(), size)
                test.assertIn(request, expected_reads, "payload or unbounded read")
                actual_reads.append(request)
                return self.stream.read(size)

        def spy_open(path, mode="r", *args, **kwargs):
            test.assertEqual(path, self.path)
            test.assertEqual(mode, "rb")
            test.assertEqual(kwargs.get("buffering"), 0)
            return HeaderOnlyFile(real_open(path, mode, *args, **kwargs))

        with patch.object(Path, "open", new=spy_open):
            rows = read_gvid_frames(self.path)
        self.assertEqual(actual_reads, expected_reads)
        self.assertEqual(sum(size for _, size in actual_reads), 32 + 16 * frame_count)
        self.assertEqual(len(rows), frame_count)
        for index, row in enumerate(rows):
            self.assertEqual(row, {
                "frame_index": index, "frame_tag": index,
                "payload_offset": 48 + index * (16 + payload_size),
                "payload_size": payload_size,
            })


if __name__ == "__main__":
    unittest.main()
