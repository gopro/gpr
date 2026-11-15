#!/usr/bin/env python3
"""Prepare calibrated native wavelet-preview profiles from stock recordings.

The DNG SDK is used only by the offline color-profile compiler. Resulting GPRP
files contain explicit matrix/black/white/crop/orientation/GainMap parameters;
the native preview library and player remain SDK/JSON/Torch independent.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import time
import zlib

import raw_media as media
import stock_still
import stock_video as video

HEADER = struct.Struct('<4sIIII4sII26fI')
MAP = struct.Struct('<III8f')
BINDING = struct.Struct('<4sIQQQIIII')


def parameters(metadata, color, downsample=4):
    image, _, _ = media.validate(metadata)
    if downsample not in (4, 8, 16) or metadata['cfa'] not in ('RGGB', 'GBRG', 'GRBG', 'BGGR') or metadata['sample_bits'] not in (12, 14, 16):
        raise ValueError('native preview requires stock four-phase Bayer 12/14/16 and reduction 4/8/16')
    if (color.get('schema'), color.get('width'), color.get('height')) != ('gpr.preview_color.v1', metadata['width'], metadata['height']):
        raise ValueError('color compiler and source metadata disagree')
    if any(code in image for code in (50712, 50715, 50716)):
        raise ValueError('wavelet preview does not support source linearization tables or black deltas')
    if (50718 in image and media.values(image[50718]) != (1., 1.) or
        51125 in image and media.values(image[51125]) != (0., 0., 1., 1.)):
        raise ValueError('wavelet preview requires square default scale and a full DefaultUserCrop')
    w, h = metadata['width'], metadata['height']
    area = media.values(image[50829]) if 50829 in image else (0, 0, h, w)
    if any(int(value) != value for value in area):
        raise ValueError('preview requires integral ActiveArea')
    top, left, bottom, right = map(int, area)
    origin = media.values(image[50719]) if 50719 in image else (0, 0)
    size = media.values(image[50720]) if 50720 in image else (right-left, bottom-top)
    crop = (left+origin[0], top+origin[1], left+origin[0]+size[0], top+origin[1]+size[1])
    active = (left, top, right, bottom)
    repeat_rows, repeat_cols = map(int, media.values(image[50713]))
    black = media.values(image[50714])
    # DNG repeat offsets begin at ActiveArea, not necessarily sensor (0,0).
    sites = [black[((site//2-top) % repeat_rows)*repeat_cols+(site % 2-left) % repeat_cols] for site in range(4)]
    white, = media.values(image[50717])
    orientation = media.values(image[274])[0] if 274 in image else 1
    if int(orientation) != orientation or not 1 <= orientation <= 8:
        raise ValueError('invalid source orientation')
    matrix = color.get('camera_to_srgb', [])
    camera_white = color.get('camera_white', [])
    exposure_ev = color.get('baseline_exposure_ev')
    if len(matrix) != 9 or any(type(x) not in (float, int) or not math.isfinite(x) or abs(x) > 256 for x in matrix):
        raise ValueError('invalid camera-to-sRGB transform')
    if len(camera_white) != 3 or any(type(x) not in (float, int) or not math.isfinite(x) or not 0 < x <= 1 for x in camera_white):
        raise ValueError('invalid normalized camera white')
    if type(exposure_ev) not in (float, int) or not math.isfinite(exposure_ev) or not -16 <= exposure_ev <= 8:
        raise ValueError('unsupported baseline exposure')
    maps = []
    for code, stage in ((51008, 1), (51009, 2), (51022, 3)):
        if code not in image:
            continue
        data = media.unpack_record(image[code])
        if len(data) < 4:
            raise ValueError('truncated opcode list')
        count, = struct.unpack_from('>I', data)
        if count > 32:
            raise ValueError('too many preview opcodes')
        position = 4
        for _ in range(count):
            if position+16 > len(data):
                raise ValueError('truncated opcode header')
            op, version, flags, length = struct.unpack_from('>4I', data, position)
            position += 16
            if position+length > len(data) or flags & ~3 or version > 0x01040000:
                raise ValueError('invalid preview opcode')
            body = data[position:position+length]; position += length
            if stage != 2 or op != 9:
                raise ValueError(f'wavelet preview does not support opcode {op} in list {stage}')
            if len(body) < 76:
                raise ValueError('truncated GainMap')
            t, l, b, r, plane, planes, rp, cp = struct.unpack_from('>8I', body)
            rows, cols, dy, dx, oy, ox, map_planes = struct.unpack_from('>2I4dI', body, 32)
            if (not 0 <= t < b <= bottom-top or not 0 <= l < r <= right-left or plane != 0 or planes != 1 or
                rp not in (1, 2) or cp not in (1, 2) or not 1 <= rows <= 256 or not 1 <= cols <= 256 or map_planes != 1 or
                len(body) != 76+rows*cols*4 or not all(math.isfinite(x) for x in (dy, dx, oy, ox)) or
                min(dy, dx) < 1e-12 or max(abs(oy), abs(ox)) > 1e6):
                raise ValueError('unsupported GainMap geometry')
            entries = struct.unpack_from('>'+str(rows*cols)+'f', body, 76)
            if any(not math.isfinite(x) or not 0 < x <= 64 for x in entries):
                raise ValueError('unsupported GainMap gain')
            phase_mask = sum(1 << site for site in range(4)
                if ((site//2-top-t) % rp == 0 and (site % 2-left-l) % cp == 0))
            maps.append(dict(phase_mask=phase_mask, rows=rows, cols=cols, spacing_y=dy, spacing_x=dx,
                origin_y=oy, origin_x=ox, area=(left+l, top+t, left+r, top+b), values=entries))
        if position != len(data):
            raise ValueError('trailing opcode bytes')
    floats = (*sites, white, *matrix, *camera_white, 2.**exposure_ev, *active, *crop)
    result = HEADER.pack(b'GPRP', 1, w, h, metadata['sample_bits'], metadata['cfa'].encode(), downsample, int(orientation), *floats, len(maps))
    for grid in maps:
        result += MAP.pack(grid['phase_mask'], grid['rows'], grid['cols'], grid['spacing_y'], grid['spacing_x'],
                           grid['origin_y'], grid['origin_x'], *grid['area'])
        result += struct.pack('<'+str(len(grid['values']))+'f', *grid['values'])
    result += struct.pack('<I', zlib.crc32(result))
    if len(result) > 16*1024*1024:
        raise ValueError('preview profile exceeds native limit')
    return result, dict(orientation=int(orientation), downsample=downsample, black_sites=sites, white=white,
        active=list(active), crop=list(crop), gain_map_count=len(maps), color=color,
        omitted_look_tags=sorted(set(image).intersection((50937, 50938, 50939, 50940, 50981, 50982))))


def prepare(source, output, compiler, downsample=4, metadata=None):
    started = time.perf_counter()
    source = Path(source).resolve(); compiler = Path(compiler).resolve()
    if not source.is_file() or not compiler.is_file():
        raise ValueError('requires regular recording and color compiler files')
    output = Path(os.path.abspath(output))
    if output.exists() or output.is_symlink():
        raise FileExistsError(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    source_sha = stock_still.digest(source)
    with tempfile.TemporaryDirectory(prefix='.stock-preview-', dir=output.parent) as temporary:
        root = Path(temporary); cache = {}; profiles = []; frames = []
        with source.open('rb') as file:
            header = video.read_header(file)
            start = file.tell(); file.seek(64)
            clip_crc = zlib.crc32(file.read(header.get('_metadata_size', 0))); file.seek(start)
            for record in video.records(file, header):
                cal = record['metadata'].get('raw_metadata', header.get('metadata', {}).get('raw_metadata', metadata))
                media.validate(cal, header)
                identity = hashlib.sha256(stock_still.canonical(cal)).hexdigest()
                if identity not in cache:
                    # Reject unsupported spatial metadata before invoking the
                    # legacy SDK parser. Actual color still comes from the SDK.
                    parameters(cal, dict(schema='gpr.preview_color.v1', width=cal['width'], height=cal['height'],
                        camera_to_srgb=[1., 0., 0., 0., 1., 0., 0., 0., 1.], camera_white=[1., 1., 1.], baseline_exposure_ev=0.), downsample)
                    carrier = root/'source.gpr'
                    media.write(carrier, cal, record['payload'], 'gpr')
                    process = subprocess.run([str(compiler), str(carrier)], capture_output=True, text=True)
                    if process.returncode:
                        raise RuntimeError(f'color compiler failed: {process.stderr[-8000:]}')
                    color = video.decode_metadata(process.stdout.encode())
                    carrier.unlink()
                    data, settings = parameters(cal, color, downsample)
                    number = len(profiles); path = root/f'profile-{number}.gprp'; path.write_bytes(data)
                    profiles.append(dict(index=number, metadata_sha256=identity, profile_sha256=hashlib.sha256(data).hexdigest(),
                                         bytes=len(data), settings=settings))
                    cache[identity] = (number, path)
                number, path = cache[identity]
                frame = root/f'{record["index"]:06}.gprp'
                stock_still.publish([(path, frame)])
                end = file.tell(); size = record['metadata_bytes']
                file.seek(end-len(record['payload'])-size); metadata_crc = zlib.crc32(file.read(size)); file.seek(end)
                profile_crc = struct.unpack_from('<I', path.read_bytes(), path.stat().st_size-4)[0]
                binding = BINDING.pack(b'GPFB', 1, record['index'], record['pts_ns'], len(record['payload']),
                                      zlib.crc32(record['payload']), metadata_crc, clip_crc, profile_crc)
                binding += struct.pack('<I', zlib.crc32(binding))
                frame.with_suffix('.gprp.bind').write_bytes(binding)
                frames.append(dict(index=record['index'], pts_ns=record['pts_ns'], profile=number,
                                   payload_sha256=hashlib.sha256(record['payload']).hexdigest(), binding_sha256=hashlib.sha256(binding).hexdigest()))
                if len(frames) > 100000:
                    raise ValueError('preview preparation frame limit exceeded')
        if not frames or stock_still.digest(source) != source_sha:
            raise ValueError('empty or changing recording')
        report = dict(schema='gpr.stock_preview_profiles.v1', source_sha256=source_sha,
            compiler_sha256=stock_still.digest(compiler), profiles=profiles, frames=frames,
            frame_count=len(frames), complete=header['_complete'], end_marker_verified=header['_end_marker_verified'],
            color_space='sRGB', mode='calibrated_wavelet_approximation', elapsed_before_publication_s=time.perf_counter()-started)
        receipt = root/'receipt.json'; receipt.write_text(json.dumps(report, indent=2)+'\n')
        output.mkdir()
        try:
            stock_still.publish([(root/f'{frame["index"]:06}.{suffix}', output/f'{frame["index"]:06}.{suffix}')
                for frame in frames for suffix in ('gprp', 'gprp.bind')]+[(receipt, output/'receipt.json')])
        except BaseException:
            # publish removes only files it created. Preserve unexpected files
            # if another process placed something in this newly created folder.
            try: output.rmdir()
            except OSError: pass
            raise
        return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--metadata-file', type=Path)
    parser.add_argument('--downsample', type=int, choices=(4, 8, 16), default=4)
    args = parser.parse_args()
    meta = video.decode_metadata(args.metadata_file.read_bytes()) if args.metadata_file else None
    print(json.dumps(prepare(args.input, args.output_dir, args.compiler, args.downsample, meta), indent=2))


if __name__ == '__main__':
    main()
