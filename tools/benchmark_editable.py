#!/usr/bin/env python3
"""Measure and independently verify calibrated replay -> editable RAW export.

This is still-derived replay, not a sensor/video-quality benchmark. Independent
verification needs numpy/tifffile in addition to the stdlib production tools.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time
import tifffile
import raw_media
import stock_video


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', type=Path, action='append', required=True)
    p.add_argument('--raw', type=Path, action='append', required=True)
    p.add_argument('--capture', type=Path, required=True)
    p.add_argument('--decoder', type=Path, required=True)
    p.add_argument('--sdk', type=Path, required=True)
    p.add_argument('--output-dir', type=Path, required=True)
    p.add_argument('--frames', type=int, default=12)
    args = p.parse_args()
    if len(args.source) != len(args.raw) or not 0 < args.frames <= 10000:
        p.error('one RAW per source and a valid frame count required')
    args.output_dir.mkdir(parents=True, exist_ok=False)
    root = args.output_dir; clip = root/'capture.gvid'; export = root/'editable'
    calibrations = [raw_media.extract(path, 14) for path in args.source]
    cfg = calibrations[0]
    if any((m['width'], m['height'], m['cfa'], m['sample_bits']) !=
           (cfg['width'], cfg['height'], 'RGGB', 14) for m in calibrations):
        p.error('benchmark currently requires equal-size RGGB14 inputs')
    command = [str(args.capture.resolve()), '--output', str(clip), '--width', str(cfg['width']),
               '--height', str(cfg['height']), '--quality', '3', '--frames', str(args.frames),
               '--workspace-mib', '192', '--leased', '--submit-timeout-ms', '10000']
    for i, (m, raw) in enumerate(zip(calibrations, args.raw)):
        metadata = root/f'calibration-{i}.json'; metadata.write_text(json.dumps(m))
        command += ['--input', str(raw.resolve()), '--source-metadata', str(metadata)]
    start = time.perf_counter()
    run = subprocess.run(command, capture_output=True, text=True, check=True)
    capture_seconds = time.perf_counter()-start; (root/'capture.jsonl').write_text(run.stdout)
    start = time.perf_counter()
    run = subprocess.run([sys.executable, str(Path(__file__).with_name('stock_video.py')), 'extract', str(clip),
                          '--out-dir', str(export), '--decoder', str(args.decoder.resolve()), '--editable', 'both'],
                         capture_output=True, text=True, check=True)
    export_seconds = time.perf_counter()-start; (root/'export.log').write_text(run.stdout)
    verified = []
    with clip.open('rb') as f:
        header = stock_video.read_header(f)
        for row in stock_video.records(f, header):
            i = row['index']; source = row['metadata']['source_index']; expected = calibrations[source]
            assert row['metadata']['raw_metadata'] == expected
            gpr, dng, raw = (export/f'{i:06d}.{ext}' for ext in ('gpr', 'dng', 'raw'))
            with tifffile.TiffFile(gpr) as packaged:
                page = packaged.pages[0]; offset, length = page.dataoffsets[0], page.databytecounts[0]
                assert gpr.read_bytes()[offset:offset+length] == row['payload']
            with tifffile.TiffFile(dng) as editable, tifffile.TiffFile(args.source[source]) as original:
                assert editable.asarray().astype('<u2').tobytes() == raw.read_bytes()
                # Check every supported calibration tag through an independent reader.
                for t in expected['image']:
                    assert editable.pages[0].tags[t['code']].value == original.pages[0].tags[t['code']].value
                if 34665 in original.pages[0].tags:
                    assert editable.pages[0].tags[34665].value == original.pages[0].tags[34665].value
                if 34853 in original.pages[0].tags:
                    assert editable.pages[0].tags[34853].value == original.pages[0].tags[34853].value
            sdk_raw = export/f'{i:06d}.sdk.raw'
            subprocess.run([str(args.sdk.resolve()), '-i', str(gpr), '-o', str(sdk_raw)], capture_output=True, check=True)
            assert stock_video.sha256(sdk_raw) == stock_video.sha256(raw)
            verified.append(dict(index=i, source_index=source, payload_sha256=hashlib.sha256(row['payload']).hexdigest(),
                                 decoded_sha256=stock_video.sha256(raw), dng_sha256=stock_video.sha256(dng),
                                 gpr_sha256=stock_video.sha256(gpr),
                                 raw_metadata_sha256=hashlib.sha256(json.dumps(expected, sort_keys=True, separators=(',', ':')).encode()).hexdigest()))
    assert len(verified) == args.frames and header['_end_marker_verified']
    receipt = dict(schema='gpr.editable_benchmark.v1', source_kind='still_derived_replay', frames=verified,
                   capture_process_wall_s=capture_seconds, export_process_wall_s=export_seconds,
                   export_wall_fps=len(verified)/export_seconds, cnn=False,
                   sources=[dict(source_sha256=stock_video.sha256(s), raw_sha256=stock_video.sha256(r))
                            for s, r in zip(args.source, args.raw)],
                   independent_checks='tifffile calibration/EXIF/pixels and SDK GPR decode; GPR payloads exact',
                   clip_sha256=stock_video.sha256(clip))
    (root/'benchmark.json').write_text(json.dumps(receipt, indent=2)+'\n')
    print(json.dumps(receipt, indent=2))


if __name__ == '__main__': main()
