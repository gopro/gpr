#!/usr/bin/env python3
"""Calibrated profile preparation, source color and per-frame native playback."""
import argparse
import copy
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools'))
import raw_media as media
import stock_preview as preview
import stock_video as video
from stock_mov_test import calibration


def replace(meta, code, dtype, count, data):
    meta['image'] = [t for t in meta['image'] if t['code'] != code]+[media.record(code, dtype, count, data)]


def recording(calibrations):
    payload = (ROOT/'tests/fixtures/stock_preview/128x96-q3.vc5').read_bytes()
    header = bytearray(video.CLIP.pack(b'GVID', 3, 64, b'VC5\0', 128, 96, 30000, 1001, 3,
                                     b'RGGB', 14, 48, struct.pack('<III', 0, 1, 0)+bytes(8)))
    struct.pack_into('<I', header, 56, zlib.crc32(header[:56])); data = bytes(header)
    for i, cal in enumerate(calibrations):
        meta = json.dumps(dict(raw_metadata=cal)).encode(); pts = (2**64-1_000_000_000)+i*33366700
        frame = bytearray(video.RECORD3.pack(b'FRM3', 48, i, pts, len(payload), len(meta), zlib.crc32(payload), zlib.crc32(meta), 0))
        struct.pack_into('<I', frame, 44, zlib.crc32(frame[:44])); data += frame+meta+payload
    end = bytearray(video.RECORD3.pack(b'END3', 48, len(calibrations), pts, len(payload)*len(calibrations), zlib.crc32(data), 0, 0, 0))
    struct.pack_into('<I', end, 36, zlib.crc32(end[:36])); return data+end


class CalibratedPreview(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='gpr-cal-preview-test-')
        self.addCleanup(self.temporary.cleanup); self.root = Path(self.temporary.name)
        self.meta = calibration(15000)

    def compile_color(self, metadata):
        source = self.root/'color.gpr'
        if source.exists(): source.unlink()
        media.write(source, metadata, (ROOT/'tests/fixtures/stock_preview/128x96-q3.vc5').read_bytes(), 'gpr')
        return video.decode_metadata(subprocess.check_output([str(args.compiler), str(source)]))

    def invoke(self, source, directory, *extra, okay=True):
        result = subprocess.run([str(args.player), '--input', str(source), '--profile-dir', str(directory), *map(str, extra)],
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode == 0, okay, result.stderr)
        return [json.loads(line) for line in result.stdout.splitlines()]

    def test_source_white_balance_and_dual_illuminant_transform(self):
        first = self.compile_color(self.meta)
        self.assertEqual(first['schema'], 'gpr.preview_color.v1')
        # A source camera-neutral white must map to neutral linear sRGB.
        rgb = [sum(first['camera_to_srgb'][i*3+j]*first['camera_white'][j] for j in range(3)) for i in range(3)]
        for value in rgb: self.assertAlmostEqual(value, 1., places=4)
        changed = copy.deepcopy(self.meta)
        replace(changed, 50728, 5, 3, struct.pack('<6I', 1, 1, 1, 1, 1, 1))
        second = self.compile_color(changed)
        self.assertNotEqual(first['camera_to_srgb'], second['camera_to_srgb'])
        dual = copy.deepcopy(self.meta)
        replace(dual, 50728, 5, 3, struct.pack('<6I', 9642, 10000, 1, 1, 8252, 10000))
        single = self.compile_color(dual)
        replace(dual, 50722, 10, 9, struct.pack('<18i', 102, 100, 0, 1, 0, 1, 0, 1, 98, 100, 0, 1, 0, 1, 0, 1, 101, 100))
        replace(dual, 50779, 3, 1, struct.pack('<H', 17))
        self.assertNotEqual(single['camera_to_srgb'], self.compile_color(dual)['camera_to_srgb'])

    def test_dynamic_frame_profiles_orientation_and_cached_playback(self):
        changed = copy.deepcopy(self.meta)
        replace(changed, 50728, 5, 3, struct.pack('<6I', 1, 1, 1, 1, 1, 1))
        replace(changed, 274, 3, 1, struct.pack('<H', 6))
        source = self.root/'source.gvid'; source.write_bytes(recording([self.meta, changed, self.meta]))
        directory = self.root/'profiles'; report = preview.prepare(source, directory, args.compiler)
        self.assertEqual(report['frame_count'], 3); self.assertEqual(len(report['profiles']), 2)
        self.assertTrue(report['end_marker_verified'])
        self.assertEqual((directory/'000000.gprp').read_bytes(), (directory/'000002.gprp').read_bytes())
        output = self.root/'ppm'
        rows = self.invoke(source, directory, '--runs', '2', '--output-dir', output)
        self.assertEqual(rows[-1]['rendered'], 6); self.assertEqual(rows[-1]['prepared_plans'], 2)
        self.assertTrue(rows[-1]['calibrated']); self.assertTrue(rows[-1]['end_marker_verified'])
        self.assertEqual([(r['width'], r['height']) for r in rows[:3]], [(32, 24), (24, 32), (32, 24)])
        self.assertEqual((output/'run0-frame0.ppm').read_bytes(), (output/'run0-frame2.ppm').read_bytes())
        self.assertEqual((output/'run0-frame0.ppm').read_bytes(), (output/'run1-frame0.ppm').read_bytes())
        self.assertNotEqual(rows[0]['rgb_crc32'], rows[1]['rgb_crc32'])
        with self.assertRaises(FileExistsError): preview.prepare(source, directory, args.compiler)
        original = (directory/'000000.gprp').read_bytes()
        (directory/'000000.gprp').write_bytes(original[:-1]+bytes([original[-1] ^ 1]))
        self.invoke(source, directory, okay=False)
        (directory/'000000.gprp').write_bytes(original)
        binding = directory/'000000.gprp.bind'; original_binding = binding.read_bytes()
        binding.write_bytes((directory/'000001.gprp.bind').read_bytes())
        self.invoke(source, directory, okay=False)
        binding.write_bytes(original_binding)
        stale = self.root/'stale.gvid'; stale.write_bytes(recording([changed, changed, self.meta]))
        self.invoke(stale, directory, okay=False)
        self.invoke(source, directory, '--downsample', '4', okay=False)
        bad = self.root/'bad.gvid'; bad.write_bytes(source.read_bytes()[:-48])
        self.invoke(bad, directory, '--limit', '1', okay=False)

    def test_profile_encoding_bayer_sites_and_active_origin(self):
        meta = copy.deepcopy(self.meta)
        replace(meta, 50829, 4, 4, struct.pack('<4I', 1, 3, 95, 127))
        color = self.compile_color(meta)
        packed, report = preview.parameters(meta, color, 8)
        self.assertEqual(len(packed), 144)
        self.assertEqual(zlib.crc32(packed[:-4]), struct.unpack_from('<I', packed, len(packed)-4)[0])
        self.assertEqual(report['black_sites'], [19.5, 18.5, 17.5, 16.5])
        self.assertEqual(report['crop'], [3, 1, 127, 95])
        self.assertEqual(preview.HEADER.unpack_from(packed)[6:8], (8, 1))

    def test_gain_maps_and_unsupported_metadata_rejection(self):
        color = self.compile_color(self.meta)
        meta = copy.deepcopy(self.meta)
        body = struct.pack('>8I', 0, 1, 96, 128, 0, 1, 2, 2)+struct.pack('>2I4dI4f', 2, 2, 1., 1., 0., 0., 1, 1., 1.1, 1.2, 1.3)
        data = struct.pack('>I4I', 1, 9, 0x01040000, 1, len(body))+body
        replace(meta, 51009, 7, len(data), data)
        packed, report = preview.parameters(meta, color)
        self.assertEqual(report['gain_map_count'], 1)
        self.assertEqual(preview.MAP.unpack_from(packed, 140)[:3], (2, 2, 2))
        for code, value in ((51022, struct.pack('>I4I', 1, 3, 0x01040000, 1, 0)),
                            (51009, data[:-1]), (50715, struct.pack('<2I', 0, 1))):
            bad = copy.deepcopy(meta); replace(bad, code, 7 if code > 51000 else 5, len(value) if code > 51000 else 1, value)
            with self.assertRaises(ValueError): preview.parameters(bad, color)
        bad = copy.deepcopy(color); bad['camera_white'][0] = float('nan')
        with self.assertRaises(ValueError): preview.parameters(meta, bad)
        for code, count, values in ((50718, 2, (2, 1, 1, 1)), (51125, 4, (0, 1, 0, 1, 9, 10, 1, 1))):
            bad = copy.deepcopy(meta); replace(bad, code, 5, count, struct.pack('<'+'I'*len(values), *values))
            with self.assertRaises(ValueError): preview.parameters(bad, color)

    def test_complete_input_before_publication_and_protected_paths(self):
        source = self.root/'source.gvid'; data = recording([self.meta])
        for suffix, content in (('truncated', data[:-1]), ('trailing', data+b'x')):
            source.write_bytes(content); output = self.root/suffix
            with self.assertRaises(ValueError): preview.prepare(source, output, args.compiler)
            self.assertFalse(output.exists())
        source.write_bytes(data); output = self.root/'link'
        output.symlink_to(self.root/'missing', target_is_directory=True)
        with self.assertRaises(FileExistsError): preview.prepare(source, output, args.compiler)
        self.assertTrue(output.is_symlink()); self.assertFalse(list(self.root.glob('.stock-preview-*')))
        with self.assertRaises(ValueError): preview.prepare('/dev/null', self.root/'null', args.compiler)
        fifo = self.root/'fifo'; os.mkfifo(fifo)
        self.invoke(fifo, self.root, okay=False)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--player', type=Path, required=True)
    args, remaining = parser.parse_known_args(); args.compiler = args.compiler.resolve(); args.player = args.player.resolve()
    unittest.main(argv=[sys.argv[0]]+remaining)
