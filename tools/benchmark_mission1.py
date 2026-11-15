#!/usr/bin/env python3
"""Verify matched Mission 1 stock replay with and without calibrated preview.

Uses caller-supplied 4096x3072 RGGB14 RAWs and calibration, plus a separately
built stock encoder reference. Reports the 20 fps handoff floor independently
of functional success. The preview sink is memory CRC, not a physical display.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import zlib

import raw_media
import stock_preview
import stock_still
import stock_video


def run(command, log, environment=None):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True,
                            env=environment, timeout=3600)
    log.write_text(result.stdout)
    log.with_suffix(log.suffix+'.stderr').write_text(result.stderr)
    if result.returncode:
        raise RuntimeError(f'{Path(command[0]).name} failed: {result.stderr[-3000:]}')
    return [json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')]


def verify(clip, events, metadata, payloads, offline=None):
    final = next(row for row in events if row['event'] == 'summary')
    preview_rows = [row for row in events if row['event'] == 'preview']
    previews = {row['index']: row for row in preview_rows}
    if len(previews) != len(preview_rows):
        raise ValueError('duplicate delivered preview index')
    count = delivered = 0
    previous_attempt = -1
    with clip.open('rb') as file:
        header = stock_video.read_header(file)
        for frame in stock_video.records(file, header):
            source = frame['metadata']['source_index']
            attempt = frame['metadata']['attempt_index']
            if (attempt <= previous_attempt or not 0 <= attempt < final['attempts'] or
                    source != attempt % len(metadata)):
                raise ValueError('recording source/attempt association differs')
            previous_attempt = attempt
            if hashlib.sha256(frame['payload']).hexdigest() != payloads[source]:
                raise ValueError('recorded payload differs from independent stock encode')
            if frame['metadata'].get('raw_metadata') != metadata[source]:
                raise ValueError('recorded source calibration differs')
            if frame['index'] in previews:
                row = previews[frame['index']]
                if offline is None or (row['timestamp_ns'], row['source_index'], row['payload_crc32']) != (
                        frame['pts_ns'], source, zlib.crc32(frame['payload'])):
                    raise ValueError('delivered preview has wrong source/frame binding')
                expected = offline[source]
                if any(row[key] != expected[key] for key in ('rgb_crc32', 'width', 'height')):
                    raise ValueError('delivered preview pixels differ from offline calibrated playback')
                delivered += 1
            count += 1
    if (not header['_end_marker_verified'] or not final['complete'] or
            count != final['written'] or count != final['accepted'] or
            final['attempts'] != count+final['overruns'] or delivered != len(previews)):
        raise ValueError('capture/end-marker/frame accounting differs')
    preview = next((row for row in events if row['event'] == 'preview_summary'), None)
    if preview is not None and (preview['error'] or preview['rendered'] != delivered or
            preview['offered'] != preview['rendered']+preview['discarded'] or
            preview['inflight_high_water'] > 2):
        raise ValueError('preview queue accounting/finalization differs')
    period_ms = 1000*final['fps_den']/final['fps_num']
    late_previews = sum(row['capture_to_sink_ms'] > period_ms for row in preview_rows)
    floor = dict(recording=(not final['overruns'] and not final['deadline_misses'] and
                            final['durable_fps'] >= final['fps_num']/final['fps_den']),
                 preview=(preview is not None and delivered == final['attempts'] and
                          not preview['discarded'] and not late_previews),
                 preview_late_deliveries=late_previews, deadline_ms=period_ms)
    return dict(frames_verified=count, previews_verified=delivered,
                all_payloads_exact_independent_stock=True, all_source_calibration_exact=True,
                all_delivered_preview_pixels_exact_offline=offline is not None,
                end_marker_verified=True, capture=final, preview=preview, floor=floor,
                clip_sha256=stock_still.digest(clip))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ('capture', 'compiler', 'player', 'reference', 'out-dir'):
        parser.add_argument('--'+flag, type=Path, required=True)
    parser.add_argument('--input', type=Path, action='append', required=True)
    parser.add_argument('--metadata', type=Path, action='append', required=True)
    parser.add_argument('--frames', type=int, default=600)
    parser.add_argument('--fps', type=int, default=20)
    parser.add_argument('--quality', type=int, choices=range(6), default=3)
    parser.add_argument('--downsample', type=int, choices=(4, 8, 16), default=4)
    parser.add_argument('--preview-from-encoder', action='store_true', help='reconstruct retained encoder bands before queued display; requires downsample 4')
    parser.add_argument('--fsync-each', action='store_true', help='sync every completed frame as well as final completion; measure storage stalls explicitly')
    parser.add_argument('--writeback-mib', type=int, default=0, help='Linux asynchronous writeback interval; final fsync remains required')
    parser.add_argument('--preview-foreground', action='store_true', help='use normal display worker priority instead of best-effort background')
    parser.add_argument('--preview-sync-stages', action='store_true', help='wait for each offered preview before starting the next encode')
    parser.add_argument('--require-floor', action='store_true', help='exit 2 if replay timing misses the requested floor')
    args = parser.parse_args()
    if not 0<=args.writeback_mib<=1024 or args.fsync_each and args.writeback_mib:
        parser.error('writeback interval must be 0..1024 MiB and cannot accompany --fsync-each')
    if args.preview_from_encoder and args.downsample != 4:
        parser.error('encoder preview requires --downsample 4')
    if len(args.input) != len(args.metadata) or not 1 <= len(args.input) <= 16:
        parser.error('supply one --metadata per --input, in input order (1..16 sources)')
    if not 1 <= args.fps <= 240 or not len(args.input) <= args.frames <= 1000000:
        parser.error('fps must be 1..240 and frames must be source count..1000000')
    paths = [*args.input, *args.metadata, args.capture, args.compiler, args.player, args.reference]
    if any(not path.is_file() for path in paths):
        parser.error('all sources, metadata and executable inputs must be regular files')
    metadata = [stock_video.decode_metadata(path.read_bytes()) for path in args.metadata]
    for source, calibration in zip(args.input, metadata):
        raw_media.validate(calibration, dict(width=4096, height=3072, sample_bits=14, bayer='RGGB'))
        if source.stat().st_size != 4096*3072*2:
            parser.error('Mission 1 replay requires tightly packed 4096x3072 RGGB14 words')
    root = args.out_dir.resolve()
    root.mkdir(parents=True, exist_ok=False)
    environment = {k: v for k, v in os.environ.items() if not k.startswith('GPR_')}
    reference_env = dict(environment, GPR_BENCH_QUALITY=str(args.quality))
    expected = []
    for index, source in enumerate(args.input):
        payload = root/f'reference-{index}.vc5'
        run([args.reference.resolve(), '-i', source.resolve(), '-o', payload,
             '-w', 4096, '-h', 3072, '-p', 8192], root/f'reference-{index}.log', reference_env)
        expected.append(stock_still.digest(payload))
    base = [args.capture.resolve(), '--width', 4096, '--height', 3072, '--cfa', 'RGGB',
            '--sample-bits', 14, '--quality', args.quality, '--fps-num', args.fps,
            '--fps-den', 1, '--queue', 4, '--workspace-mib', 192, '--leased']
    if args.fsync_each:
        base += ['--fsync-each']
    if args.writeback_mib:
        base += ['--writeback-mib', args.writeback_mib]
    for source, calibration in zip(args.input, args.metadata):
        base += ['--input', source.resolve(), '--source-metadata', calibration.resolve()]
    seed = root/'sources.gvid'
    seed_events = run(base+['--output', seed, '--frames', len(metadata), '--submit-timeout-ms', 10000],
                      root/'sources.jsonl', environment)
    verify(seed, seed_events, metadata, expected)
    profiles = root/'profiles'
    stock_preview.prepare(seed, profiles, args.compiler, args.downsample)
    events = run([args.player.resolve(), '--input', seed, '--profile-dir', profiles], root/'offline.jsonl', environment)
    offline = {row['index']: row for row in events if row['event'] == 'preview'}
    if len(offline) != len(metadata):
        raise ValueError('offline preview did not render all sources')
    runs = {}
    for name in ('recording', 'recording-preview'):
        command = base+['--output', root/(name+'.gvid'), '--frames', args.frames, '--paced']
        if name == 'recording-preview':
            command += ['--preview-every', 1]
            if args.preview_from_encoder:
                command += ['--preview-from-encoder']
            if args.preview_foreground:
                command += ['--preview-foreground']
            if args.preview_sync_stages:
                command += ['--preview-sync-stages']
            for index in range(len(metadata)):
                command += ['--preview-profile', profiles/f'{index:06}.gprp']
        events = run(command, root/(name+'.jsonl'), environment)
        runs[name] = verify(root/(name+'.gvid'), events, metadata, expected,
                            offline if name == 'recording-preview' else None)
        print(json.dumps(dict(run=name, **runs[name])), flush=True)
    combined = runs['recording-preview']['floor']
    result = dict(schema='gpr.mission1_replay.v1', platform=platform.platform(),
                  replay=True, source_kind='still_derived_raw', sink='memory_crc',
                  source_geometry=[4096, 3072], cfa='RGGB', sample_bits=14,
                  quality=args.quality, requested_fps=args.fps, preview_downsample=args.downsample,
                  frames_requested=args.frames, functional_checks_passed=True,
                  fsync_each=args.fsync_each, writeback_mib=args.writeback_mib,
                  preview_background=not args.preview_foreground,
                  preview_sync_stages=args.preview_sync_stages,
                  preview_pipeline='encoder_wavelets' if args.preview_from_encoder else 'compressed',
                  combined_replay_floor_passed=combined['recording'] and combined['preview'],
                  camera_production_ready=False, model_quality_approved=False,
                  timing_scope='paced warm-cache replay; recording includes final fsync; preview includes queue drain; source loading and plan setup excluded',
                  inputs=[dict(raw_sha256=stock_still.digest(raw), metadata_sha256=stock_still.digest(meta),
                               reference_payload_sha256=payload)
                          for raw, meta, payload in zip(args.input, args.metadata, expected)],
                  binaries={name: stock_still.digest(getattr(args, name))
                            for name in ('capture', 'compiler', 'player', 'reference')}, runs=runs)
    (root/'receipt.json').write_text(json.dumps(result, indent=2)+'\n')
    if args.require_floor and not result['combined_replay_floor_passed']:
        raise SystemExit(2)


if __name__ == '__main__':
    main()
