#!/usr/bin/env python3
"""Record, decode, export and preview every unpacked stock Bayer video format."""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import raw_media
from raw_media_test import metadata


def run(*args):
    result = subprocess.run([str(item) for item in args], capture_output=True, text=True, timeout=60)
    if result.returncode:
        raise AssertionError(f'{args}: {result.stderr[-1500:]}')
    return result.stdout


def main():
    parser = argparse.ArgumentParser()
    for name in ('capture', 'decoder', 'compiler', 'player'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--only', help='run one CFA and bit-depth pair, for instrumented debugging')
    args = parser.parse_args()
    if args.only and args.only not in {f'{cfa}{bits}' for cfa in ('RGGB', 'GBRG', 'GRBG', 'BGGR') for bits in (12, 14, 16)}:
        parser.error('invalid --only format')
    with tempfile.TemporaryDirectory(prefix='gpr-video-format-matrix-') as temporary:
        root = Path(temporary)
        for cfa in ('RGGB', 'GBRG', 'GRBG', 'BGGR'):
            for bits in (12, 14, 16):
                stem = f'{cfa}{bits}'
                if args.only and stem != args.only:
                    continue
                source, clip = root / f'{stem}.raw', root / f'{stem}.gvid'
                meta_file = root / f'{stem}.json'
                extracted, profiles = root / f'{stem}-out', root / f'{stem}-profiles'
                samples = [((x * 719 + y * 1877 + (x % 2) * 3001 + (y % 2) * 1307) % (1 << bits))
                           for y in range(48) for x in range(48)]
                if bits == 16:
                    assert max(samples) > 16383
                elif bits == 14:
                    assert max(samples) > 4095
                source.write_bytes(struct.pack('<' + str(len(samples)) + 'H', *samples))
                meta_file.write_text(json.dumps(metadata(48, 48, bits, cfa)))
                run(args.capture, '--input', source, '--output', clip, '--width', 48, '--height', 48,
                    '--cfa', cfa, '--sample-bits', bits, '--frames', 1, '--quality', 3)
                report = json.loads(run(sys.executable, ROOT / 'tools/stock_video.py', 'extract', clip,
                                        '--out-dir', extracted, '--decoder', args.decoder,
                                        '--editable', 'both', '--metadata-file', meta_file))
                assert report['complete'] and report['end_marker_verified'] and report['frame_count'] == 1
                assert (report['header']['bayer'], report['header']['sample_bits']) == (cfa, bits)
                assert (extracted / '000000.raw').stat().st_size == 48 * 48 * 2
                for suffix in ('dng', 'gpr'):
                    package = extracted / f'000000.{suffix}'
                    exported = raw_media.extract(package, bits)
                    assert (exported['cfa'], exported['sample_bits'], exported['width'], exported['height']) == (cfa, bits, 48, 48)
                prepared = json.loads(run(sys.executable, ROOT / 'tools/stock_preview.py', clip,
                                          '--output-dir', profiles, '--compiler', args.compiler,
                                          '--metadata-file', meta_file))
                assert prepared['frame_count'] == 1
                events = [json.loads(line) for line in run(args.player, '--input', clip,
                                                           '--profile-dir', profiles).splitlines()]
                assert events[0]['event'] == 'preview' and events[0]['calibrated']
                assert events[-1]['event'] == 'summary' and events[-1]['end_marker_verified']
    print('PASS: stock Bayer video formats through capture, decode, editable RAW and calibrated preview')


if __name__ == '__main__':
    main()
