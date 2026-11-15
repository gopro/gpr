#!/usr/bin/env python3
"""Evaluate optional 1x reconstruction against independently decoded sensor DNG.

Consumes a completed benchmark_stills.py directory. No training, model download,
or promotion occurs. Torch/numpy/tifffile/imagecodecs are development dependencies.
The current checkpoint adapter requires RGGB14 and produces same-size Bayer.
"""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import time

import numpy as np
import tifffile
import torch

from benchmark_stills import bayer_page, quality
import raw_media
import stock_still
import stock_video


def evaluate(source, benchmark_dir, cnn_root, checkpoint, output_dir, sdk, device,
             qualities=(0, 1, 3, 5), tile=256, halo=64):
    source, benchmark_dir = Path(source).resolve(), Path(benchmark_dir).resolve()
    output_dir = Path(output_dir).resolve()
    if tile < 8 or tile % 8 or halo < 0 or halo % 8:
        raise ValueError('tile/halo must be multiples of eight; tile must be positive')
    if not qualities or len(set(qualities)) != len(qualities) or any(type(q) is not int or not 0 <= q <= 5 for q in qualities):
        raise ValueError('qualities must be distinct stock presets 0..5')
    manifest_path = benchmark_dir/'benchmark.json'
    manifest = json.loads(manifest_path.read_text())
    if (manifest.get('schema') != 'gpr.stock_still_benchmark.v1' or
        manifest.get('reference_kind') != 'lossless_sensor_dng' or
        manifest.get('independent_source_decode_verified') is not True):
        raise ValueError('requires a verified lossless sensor-DNG benchmark')
    if (manifest['cfa'], manifest['sample_bits']) != ('RGGB', 14):
        raise ValueError('current CNN checkpoint adapter requires RGGB14')
    if stock_still.digest(source) != manifest['source_sha256']:
        raise ValueError('source does not match benchmark')
    meta = raw_media.extract(source, 14)
    w, h = meta['width'], meta['height']
    metadata_sha = hashlib.sha256(stock_still.canonical(meta)).hexdigest()
    if (w, h, metadata_sha) != (manifest['width'], manifest['height'], manifest['metadata_sha256']):
        raise ValueError('source calibration does not match benchmark')
    reference_path = benchmark_dir/'reference.raw'
    if stock_still.digest(reference_path) != manifest['raw_sha256']:
        raise ValueError('reference RAW does not match benchmark')
    reference = np.memmap(reference_path, mode='r', dtype='<u2', shape=(h, w))
    with tifffile.TiffFile(source) as file:
        page = bayer_page(file.pages)
        if int(page.compression) not in (1, 7, 8, 32946) or not np.array_equal(page.asarray(), reference):
            raise ValueError('independent lossless source decode differs from reference')
    output_dir.mkdir(parents=True, exist_ok=False)
    rows = []
    for q in qualities:
        native_row = next(row for row in manifest['rows'] if row['quality'] == q)
        gpr, dng = benchmark_dir/f'q{q}-r0.gpr', benchmark_dir/f'q{q}-r0.dng'
        decoded_receipt = json.loads(dng.with_suffix('.dng.json').read_text())
        if (stock_still.digest(gpr) != native_row['attempts'][0]['gpr_sha256'] or
            stock_still.digest(dng) != decoded_receipt['output_sha256'] or
            decoded_receipt['metadata_sha256'] != metadata_sha or
            stock_still.canonical(raw_media.extract(dng, 14)) != stock_still.canonical(meta)):
            raise ValueError('decoded benchmark output/calibration changed')
        with tifffile.TiffFile(dng) as file: decoded = file.asarray()
        if decoded.shape != (h, w) or decoded.dtype != np.dtype('<u2'):
            raise ValueError('unexpected decoded Bayer shape/count format')
        raw_sha = hashlib.sha256(decoded.astype('<u2').tobytes()).hexdigest()
        if raw_sha != native_row['attempts'][0]['decoded_raw_sha256']:
            raise ValueError('decoded pixels do not match benchmark')
        base = quality(reference, decoded, meta)
        start = time.perf_counter()
        destination = output_dir/f'q{q}.restored.dng'
        receipt = stock_still.decode(gpr, 14, sdk, destination, reconstruction=dict(
            root=cnn_root, checkpoint=checkpoint, device=device, tile=tile, halo=halo))
        inference_s, info = receipt['cnn_s'], receipt['model']
        if receipt['raw_sha256'] != raw_sha: raise ValueError('fresh decoder differs from benchmark pixels')
        with tifffile.TiffFile(destination) as file:
            restored = file.asarray()
        if hashlib.sha256(restored.astype('<u2').tobytes()).hexdigest() != receipt['restored_raw_sha256']:
            raise ValueError('editable pixels changed')
        if stock_still.canonical(raw_media.extract(destination, 14)) != stock_still.canonical(meta):
            raise ValueError('editable calibration changed')
        restored_metrics = quality(reference, restored, meta)
        row = dict(quality=q, gpr_bytes=native_row['gpr_bytes'], input_raw_sha256=raw_sha,
                   restored_raw_sha256=hashlib.sha256(restored.astype('<u2').tobytes()).hexdigest(),
                   editable_dng_sha256=stock_still.digest(destination), inference_s=inference_s,
                   inference_and_editable_validation_s=time.perf_counter()-start,
                   changed_samples=int(np.count_nonzero(restored != decoded)),
                   stock=base, restored=restored_metrics,
                   active_psnr_delta_db=restored_metrics['active']['psnr_db']-base['active']['psnr_db'])
        rows.append(row)
        print(json.dumps(dict(quality=q, stock_psnr_db=base['active']['psnr_db'],
                              restored_psnr_db=restored_metrics['active']['psnr_db'],
                              delta_db=row['active_psnr_delta_db'], inference_s=inference_s)), flush=True)
        # A partial report is useful if a later preset fails, but it must never
        # look like a completed quality gate.
        report = dict(schema='gpr.stock_cnn_benchmark.v1', complete=False, quality_approval=False,
                      reference_kind='lossless_sensor_dng', independent_source_decode_verified=True,
                      source_sha256=manifest['source_sha256'], raw_sha256=manifest['raw_sha256'],
                      benchmark_sha256=stock_still.digest(manifest_path), metadata_sha256=metadata_sha,
                      sdk_sha256=receipt['sdk_sha256'],
                      width=w, height=h, cfa='RGGB', sample_bits=14, output_scale=1,
                      tile_plane_pixels=tile, halo_plane_pixels=halo, runtime_raw_norm=16383,
                      model=info, torch=torch.__version__, host=platform.platform(), rows=rows,
                      source_files={name: stock_still.digest(Path(__file__).parent/name)
                                    for name in ('benchmark_stock_cnn.py', 'benchmark_stills.py', 'stock_video.py', 'raw_media.py', 'stock_still.py')},
                      limitations='Training-split membership is unknown. No temporal reference, rendered color gate, or model promotion. Full count-domain RAW distortion; shadows/highlights/detail use reference-only masks. Inference includes tile transfer and synchronization; first invocation is not warmed up.')
        (output_dir/'benchmark.json').write_text(json.dumps(report, indent=2)+'\n')
    report['complete'] = True
    (output_dir/'benchmark.json').write_text(json.dumps(report, indent=2)+'\n')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--benchmark-dir', type=Path, required=True)
    parser.add_argument('--cnn-root', type=Path, help='optional external architecture override')
    parser.add_argument('--checkpoint', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--sdk', type=Path, required=True)
    parser.add_argument('--device', choices=('cpu', 'mps', 'cuda'), default='cpu')
    parser.add_argument('--quality', type=int, choices=range(6), action='append')
    parser.add_argument('--tile', type=int, default=256)
    parser.add_argument('--halo', type=int, default=64)
    args = parser.parse_args()
    evaluate(args.source, args.benchmark_dir, args.cnn_root, args.checkpoint, args.output_dir,
             args.sdk, args.device, args.quality or (0, 1, 3, 5), args.tile, args.halo)


if __name__ == '__main__':
    main()
