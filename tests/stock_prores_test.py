#!/usr/bin/env python3
"""Independent deep TIFF/calibration, ProRes timing and stock pipeline contracts."""
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
from unittest.mock import patch
import zlib

import numpy as np
import tifffile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools'))
import raw_media as media
import stock_prores as prores
import stock_video as video
from raw_media_test import metadata
from stock_mov_test import wav
import stock_mov


def replace(meta, code, dtype, count, data):
    meta['image'] = [t for t in meta['image'] if t['code'] != code]+[media.record(code, dtype, count, data)]


def fixture(payload, calibrations, times):
    header = bytearray(video.CLIP.pack(b'GVID', 3, 64, b'VC5\0', 512, 384, 30000, 1001, 3,
                                     b'RGGB', 14, 48, struct.pack('<III', 0, 1, 0)+bytes(8)))
    struct.pack_into('<I', header, 56, zlib.crc32(header[:56]))
    data = bytes(header)
    for i, (calibration, pts) in enumerate(zip(calibrations, times)):
        meta = json.dumps(dict(raw_metadata=calibration)).encode()
        record = bytearray(video.RECORD3.pack(b'FRM3', 48, i, pts, len(payload), len(meta), zlib.crc32(payload), zlib.crc32(meta), 0))
        struct.pack_into('<I', record, 44, zlib.crc32(record[:44]))
        data += record+meta+payload
    end = bytearray(video.RECORD3.pack(b'END3', 48, len(times), times[-1], len(times)*len(payload), zlib.crc32(data), 0, 0, 0))
    struct.pack_into('<I', end, 36, zlib.crc32(end[:36]))
    return data+end


class Rendering(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='gpr-prores-test-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.meta = metadata(512, 384)
        self.meta['image'].append(media.record(50707, 1, 4, b'\1\3\0\0'))
        replace(self.meta, 50717, 4, 1, struct.pack('<I', 12000))
        y, x = np.indices((384, 512))
        self.raw = (1000+x*9+y*6+((x//64+y//64)%2)*800).astype('<u2')

    def render(self, name, meta=None, raw=None):
        source = self.root/(name+'.dng')
        output = source.with_suffix('.tiff')
        media.write(source, self.meta if meta is None else meta, (self.raw if raw is None else raw).tobytes())
        receipt = json.loads(prores.run([args.renderer, source, output]).stdout)
        with tifffile.TiffFile(output) as file:
            self.assertEqual(file.pages[0].bitspersample, 16)
            self.assertIn(34675, file.pages[0].tags)  # embedded ICC profile
            pixels = file.asarray()[..., :3]
        self.assertEqual(pixels.dtype, np.uint16)
        self.assertEqual(receipt['bits_per_component'], 16)
        return output, pixels, receipt

    def test_all_exif_orientations(self):
        _, base, _ = self.render('base')
        transforms = (lambda x:x, np.fliplr, lambda x:np.rot90(x, 2), np.flipud,
                      lambda x:x.transpose(1, 0, 2), lambda x:np.rot90(x, -1),
                      lambda x:np.flip(x.transpose(1, 0, 2), axis=(0, 1)), lambda x:np.rot90(x, 1))
        for orientation, transform in enumerate(transforms, 1):
            with self.subTest(orientation=orientation):
                meta = copy.deepcopy(self.meta)
                replace(meta, 274, 3, 1, struct.pack('<H', orientation))
                _, actual, report = self.render(str(orientation), meta)
                expected = transform(base)
                self.assertEqual(actual.shape, expected.shape)
                self.assertLessEqual(np.abs(actual.astype(int)-expected).max(), 3)
                self.assertEqual(report['source_orientation'], orientation)

    def test_source_white_balance_matrix_black_white_and_crop(self):
        _, base, _ = self.render('base')
        for code, data in ((50728, struct.pack('<6I', 1, 1, 1, 1, 1, 1)),
                           (50721, struct.pack('<18i', 2, 1, 0, 1, 0, 1, 0, 1, 1, 1, 0, 1, 0, 1, 0, 1, 1, 1))):
            meta = copy.deepcopy(self.meta)
            replace(meta, code, 5 if code == 50728 else 10, 3 if code == 50728 else 9, data)
            _, actual, _ = self.render(str(code), meta)
            self.assertGreater(np.abs(actual.astype(float)-base).mean(), 100)
        meta = copy.deepcopy(self.meta)
        replace(meta, 50714, 5, 4, struct.pack('<8I', 1040, 1, 1041, 1, 1042, 1, 1043, 1))
        replace(meta, 50717, 4, 1, struct.pack('<I', 13024))
        _, shifted, _ = self.render('shifted', meta, self.raw+1024)
        self.assertLess(np.abs(shifted.astype(float)-base).mean(), 3)
        meta = copy.deepcopy(self.meta)
        meta['image'] += [media.record(50829, 4, 4, struct.pack('<4I', 8, 12, 376, 500)),
                          media.record(50719, 4, 2, struct.pack('<2I', 10, 8)),
                          media.record(50720, 4, 2, struct.pack('<2I', 400, 320))]
        _, crop, _ = self.render('crop', meta)
        self.assertEqual(crop.shape[:2], (320, 400))

    def test_prores_profiles_deep_pixels_and_irregular_nanosecond_timing(self):
        image, expected, _ = self.render('frame')
        timestamps = [0, 33333333, 83333339]
        for profile, times in (('hq', timestamps), ('4444', timestamps), ('hq', [0, 6_000_000_007])):
            with self.subTest(profile=profile, timestamps=times):
                name = profile+str(len(times))
                control = self.root/(name+'.json')
                timing = prores.clock(times, 30000, 1001)
                control.write_text(json.dumps(dict(schema='gpr.prores_frames.v1', profile=profile,
                    timescale=timing['timescale'], end_pts=timing['end_pts'],
                    frames=[dict(path=str(image), pts=t) for t in timing['pts']])))
                movie = control.with_suffix('.mov')
                prores.run([args.encoder, control, movie])
                prores.verify(movie, args.ffprobe, timing, 512, 384, profile)
                raw = self.root/(name+'.rgb')
                prores.run([args.ffmpeg, '-v', 'error', '-i', movie, '-frames:v', 1,
                            '-pix_fmt', 'rgb48le', '-f', 'rawvideo', raw])
                actual = np.fromfile(raw, dtype='<u2').reshape(expected.shape)
                # Independent decoder checks channel ordering/range, endian and
                # color tags. Lossy ProRes/YUV conversion is not pixel-exact.
                mse = np.mean((actual.astype(float)-expected)**2)
                self.assertGreater(10*np.log10(65535.**2/mse), 40)

    def test_stage_two_bayer_gain_maps(self):
        _, base, _ = self.render('base')
        meta = copy.deepcopy(self.meta)
        opcodes = []
        for sy, sx in ((0, 0), (0, 1), (1, 0), (1, 1)):
            area = struct.pack('>8I', sy, sx, 384, 512, 0, 1, 2, 2)
            grid = struct.pack('>2I4dI4f', 2, 2, 1., 1., 0., 0., 1, 1.25, 1.25, 1.25, 1.25)
            body = area+grid
            opcodes.append(struct.pack('>4I', 9, 0x01030000, 1, len(body))+body)
        packed = struct.pack('>I', 4)+b''.join(opcodes)
        replace(meta, 51009, 7, len(packed), packed)
        _, actual, _ = self.render('map', meta)
        y, x = np.indices(self.raw.shape)
        black = np.array([[16, 17], [18, 19]])[y % 2, x % 2]
        expected_raw = np.rint((self.raw.astype(float)-black)*1.25+black).astype('<u2')
        _, expected, _ = self.render('scaled', raw=expected_raw)
        self.assertGreater(np.abs(actual.astype(float)-base).mean(), 1000)
        self.assertLess(np.abs(actual.astype(float)-expected).mean(), 12)

    def test_oriented_crop_deep_pitch_and_independent_decode(self):
        meta = copy.deepcopy(self.meta)
        replace(meta, 274, 3, 1, struct.pack('<H', 6))
        meta['image'] += [media.record(50719, 4, 2, struct.pack('<2I', 10, 8)),
                          media.record(50720, 4, 2, struct.pack('<2I', 402, 318))]
        image, expected, _ = self.render('portrait', meta)
        self.assertEqual(expected.shape[:2], (402, 318))
        # Width 318 is deliberately unsuitable for assuming tight SIMD rows.
        control = self.root/'portrait.json'
        timing = prores.clock([0], 30, 1)
        control.write_text(json.dumps(dict(schema='gpr.prores_frames.v1', profile='4444',
            timescale=timing['timescale'], end_pts=timing['end_pts'], frames=[dict(path=str(image), pts=0)])))
        movie = control.with_suffix('.mov')
        prores.run([args.encoder, control, movie])
        prores.verify(movie, args.ffprobe, timing, 318, 402, '4444')
        raw = self.root/'portrait.rgb'
        prores.run([args.ffmpeg, '-v', 'error', '-i', movie, '-frames:v', 1, '-pix_fmt', 'rgb48le', '-f', 'rawvideo', raw])
        actual = np.fromfile(raw, dtype='<u2').reshape(expected.shape)
        mse = np.mean((actual.astype(float)-expected)**2)
        self.assertGreater(10*np.log10(65535.**2/mse), 40)

    def test_complete_recording_pipeline_and_metadata_changes(self):
        raw = self.root/'input.raw'; raw.write_bytes(self.raw.tobytes())
        replay = self.root/'replay.gvid'
        prores.run([args.decoder, '--input', raw, '--output', replay, '--width', 512,
                    '--height', 384, '--frames', 1, '--quality', 3, '--arena-mib', 16])
        with replay.open('rb') as file:
            header = video.read_header(file)
            payload = list(video.records(file, header))[0]['payload']
        changed = copy.deepcopy(self.meta)
        replace(changed, 50728, 5, 3, struct.pack('<6I', 1, 1, 1, 1, 1, 1))
        clip = self.root/'input.gvid'
        clip.write_bytes(fixture(payload, [self.meta, changed, self.meta], [2**64-200000000, 2**64-166666667, 2**64-116666661]))
        output = self.root/'output.mov'
        render_scratch = []
        original_run = prores.run
        def observe(command):
            result = original_run(command)
            if Path(command[0]) == args.renderer:
                images = list(Path(command[-1]).parent.glob('*.tiff'))
                render_scratch.append((len(images), sum(path.stat().st_size for path in images)))
            return result
        with patch.object(prores, 'run', side_effect=observe):
            result = prores.convert(clip, output, args.decoder, args.renderer, args.encoder, args.ffprobe)
        self.assertEqual(len(render_scratch), 3)
        self.assertTrue(all(count == 1 for count, size in render_scratch))
        self.assertEqual(result['scratch']['rendered_tiff_peak_bytes'], max(size for count, size in render_scratch))
        self.assertEqual(result['scratch']['rendered_tiff_total_bytes'], sum(size for count, size in render_scratch))
        self.assertEqual(result['frame_count'], 3)
        self.assertTrue(result['end_marker_verified'])
        self.assertEqual(result['timing']['maximum_timestamp_error_ns'], 0)
        self.assertEqual(result['frames'][0]['raw_sha256'], result['frames'][1]['raw_sha256'])
        self.assertNotEqual(result['frames'][0]['tiff_sha256'], result['frames'][1]['tiff_sha256'])
        self.assertEqual(result['frames'][0]['tiff_sha256'], result['frames'][2]['tiff_sha256'])
        prores.run([args.ffmpeg, '-v', 'error', '-i', output, '-f', 'null', '-'])
        bad = self.root/'bad.gvid'; bad.write_bytes(clip.read_bytes()[:-1])
        with self.assertRaises(ValueError):
            prores.convert(bad, self.root/'bad.mov', args.decoder, args.renderer, args.encoder, args.ffprobe)
        self.assertFalse((self.root/'bad.mov').exists())
        rotated = copy.deepcopy(self.meta)
        replace(rotated, 274, 3, 1, struct.pack('<H', 6))
        bad.write_bytes(fixture(payload, [self.meta, rotated], [0, 33333333]))
        with self.assertRaises(ValueError):
            prores.convert(bad, self.root/'geometry.mov', args.decoder, args.renderer, args.encoder, args.ffprobe)
        self.assertFalse((self.root/'geometry.mov').exists())
        self.assertFalse(list(self.root.glob('.stock-prores-*')))

    def test_stream_reuses_tiff_with_exact_batch_packets_and_timing(self):
        first, _, _ = self.render('first')
        second, _, _ = self.render('second', raw=self.raw+700)
        timing = prores.clock([0, 33333333, 83333339], 30000, 1001)
        ends = timing['pts'][1:]+[timing['end_pts']]
        for profile in ('hq', '4444'):
            with self.subTest(profile=profile):
                batch = self.root/(profile+'-batch.mov')
                stream_movie = self.root/(profile+'-stream.mov')
                control = self.root/(profile+'.json')
                paths = [first, second, first]
                control.write_text(json.dumps(dict(schema='gpr.prores_frames.v1', profile=profile,
                    timescale=timing['timescale'], end_pts=timing['end_pts'],
                    frames=[dict(path=str(path), pts=pts) for path, pts in zip(paths, timing['pts'])])))
                prores.run([args.encoder, control, batch])
                header = dict(schema='gpr.prores_stream.v1', profile=profile,
                    timescale=timing['timescale'], end_pts=timing['end_pts'], frame_count=len(paths))
                with prores.encoder_stream(args.encoder, stream_movie, header, self.root/'stream.stderr') as stream:
                    for path, pts, end in zip(paths, timing['pts'], ends):
                        reused = self.root/'reused.tiff'
                        reused.write_bytes(path.read_bytes())
                        stream.append(reused, pts, end)
                        reused.unlink()
                    stream.finish()
                prores.verify(stream_movie, args.ffprobe, timing, 512, 384, profile)
                def packets(movie):
                    data = json.loads(prores.run([args.ffprobe, '-v', 'error', '-select_streams', 'v',
                        '-show_packets', '-show_data_hash', 'sha256', '-of', 'json', movie]).stdout)
                    return [(p['pts'], p['dts'], p['duration'], p['data_hash']) for p in data['packets']]
                self.assertEqual(packets(batch), packets(stream_movie))
                prores.run([args.ffmpeg, '-v', 'error', '-i', stream_movie, '-f', 'null', '-'])

    def test_truncated_or_inconsistent_stream_never_publishes(self):
        image, _, _ = self.render('frame')
        header = dict(schema='gpr.prores_stream.v1', profile='hq', timescale=1000000000,
                      end_pts=100000000, frame_count=2)
        first = dict(path=str(image), pts=0, next_pts=50000000)
        cases = ([header], [header, first],
                 [header, first, dict(path=str(image), pts=50000001, next_pts=100000000)],
                 [header, first, dict(path=str(image), pts=50000000, next_pts=100000000)],
                 [header, first, dict(path=str(image), pts=50000000, next_pts=100000000), dict(event='wrong')])
        for index, records in enumerate(cases):
            with self.subTest(case=index):
                movie = self.root/f'incomplete-{index}.mov'
                result = subprocess.run([str(args.encoder), '--stream', str(movie)],
                    input=''.join(json.dumps(record)+'\n' for record in records),
                    text=True, capture_output=True, timeout=30)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(movie.exists())
                self.assertFalse(list(self.root.glob(movie.name+'.tmp.*')))

    def test_native_rejection_and_exclusive_publication(self):
        bad = self.root/'bad.dng'; bad.write_bytes(b'not a RAW')
        output = self.root/'protected.tiff'; output.write_bytes(b'keep')
        for source in (bad, Path('/dev/null')):
            with self.assertRaises(RuntimeError):
                prores.run([args.renderer, source, output])
        image, _, _ = self.render('frame')
        control = self.root/'frames.json'
        control.write_text(json.dumps(dict(schema='gpr.prores_frames.v1', profile='hq', timescale=1000000000,
                                          end_pts=33333333, frames=[dict(path=str(image), pts=0)])))
        movie = self.root/'protected.mov'; movie.write_bytes(b'keep')
        with self.assertRaises(RuntimeError):
            prores.run([args.encoder, control, movie])
        self.assertEqual(movie.read_bytes(), b'keep')
        self.assertEqual(output.read_bytes(), b'keep')
        for pts in (-1, 0.5, True, 2**63):
            manifest = json.loads(control.read_text()); manifest['frames'][0]['pts'] = pts
            control.write_text(json.dumps(manifest))
            with self.assertRaises(RuntimeError):
                prores.run([args.encoder, control, self.root/'bad.mov'])
            self.assertFalse((self.root/'bad.mov').exists())
        self.assertFalse(list(self.root.glob('*.tmp.*')))

    def test_clock_quantization_and_collisions(self):
        timing = prores.clock([0, 6_000_000_007], 30, 1)
        self.assertEqual(timing['unit_ns'], 10)
        self.assertEqual(timing['maximum_timestamp_error_ns'], 3)
        with self.assertRaises(ValueError):
            prores.clock([0, 1, 6_000_000_007], 30, 1)

    def test_pcm_timecode_and_private_mov_inheritance(self):
        raw=self.root/'input.raw';raw.write_bytes(self.raw.tobytes())
        replay=self.root/'replay.gvid'
        prores.run([args.decoder,'--input',raw,'--output',replay,'--width',512,'--height',384,
                    '--frames',1,'--quality',3,'--arena-mib',16])
        with replay.open('rb') as file:
            header=video.read_header(file);payload=list(video.records(file,header))[0]['payload']
        clip=self.root/'input.gvid';clip.write_bytes(fixture(payload,[self.meta,self.meta],[0,33410007]))
        sound=self.root/'audio.wav';expected=wav(sound,bits=24,channels=2)
        output=self.root/'av-prores.mov'
        direct=prores.convert(clip,output,args.decoder,args.renderer,args.encoder,args.ffprobe,
                              audio=sound,timecode='01:02:03;04' if args.mov_bridge else None,
                              ffmpeg=args.ffmpeg,mov_bridge=args.mov_bridge)
        self.assertEqual(direct['auxiliary']['audio']['sha256'],expected)
        if args.mov_bridge:self.assertEqual(direct['auxiliary']['timecode']['text'],'01:02:03;04')
        self.assertTrue(direct['auxiliary']['video_packets_exact'])
        prores.run([args.ffmpeg,'-v','error','-i',output,'-f','null','-'])
        if args.mov_bridge is not None:
            movie=self.root/'stock.mov'
            stock_mov.convert('wrap',clip,args.mov_bridge,movie,audio=sound,timecode='01:02:03;04',
                              ffmpeg=args.ffmpeg,probe=args.ffprobe)
            inherited=prores.convert(movie,self.root/'inherited.mov',args.decoder,args.renderer,args.encoder,
                                     args.ffprobe,mov_bridge=args.mov_bridge,ffmpeg=args.ffmpeg)
            self.assertEqual(inherited['auxiliary']['audio'],direct['auxiliary']['audio'])
            self.assertEqual(inherited['auxiliary']['timecode'],direct['auxiliary']['timecode'])
            self.assertEqual([row['tiff_sha256'] for row in inherited['frames']],
                             [row['tiff_sha256'] for row in direct['frames']])
            with self.assertRaisesRegex(ValueError,'ffmpeg'):
                prores.convert(movie,self.root/'missing.mov',args.decoder,args.renderer,args.encoder,
                               args.ffprobe,mov_bridge=args.mov_bridge)
            self.assertFalse((self.root/'missing.mov').exists())


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    for name in ('decoder', 'renderer', 'encoder', 'ffprobe', 'ffmpeg'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--mov-bridge',type=Path)
    args, remaining = parser.parse_known_args()
    args = argparse.Namespace(**{k:v.resolve() if v is not None else None for k,v in vars(args).items()})
    unittest.main(argv=[sys.argv[0]]+remaining)
