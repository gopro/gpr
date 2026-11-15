#!/usr/bin/env python3
"""Exercise the native reader/renderer with independently framed recordings."""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib

app = Path(sys.argv[1]).resolve()
fixtures = Path(__file__).parent / 'fixtures/stock_preview'
payload = (fixtures / '128x96-q3.vc5').read_bytes()


def recording(version):
    h = bytearray(struct.pack('<4sHH4sIIIII4sII20s', b'GVID', version, 64,
                             b'VC5\0', 128, 96, 30, 1, 3, b'RGGB', 14,
                             48 if version == 3 else 32, bytes(20)))
    if version == 3:
        struct.pack_into('<I', h, 48, 1)
        struct.pack_into('<I', h, 56, zlib.crc32(h[:56]))
    data = bytes(h)
    for i in range(2):
        fh = struct.pack('<4sIQQQ', b'FRM3' if version == 3 else b'FRM2',
                         48 if version == 3 else 32, i, i * 1000000000 // 30, len(payload))
        if version == 3:
            fh += struct.pack('<III', 0, zlib.crc32(payload), 0)
            fh += struct.pack('<I', zlib.crc32(fh))
        data += fh + payload
    if version == 3:
        end = struct.pack('<4sIQQQI', b'END3', 48, 2, 1000000000 // 30,
                          2 * len(payload), zlib.crc32(data))
        data += end + struct.pack('<I', zlib.crc32(end)) + bytes(8)
    return data


with tempfile.TemporaryDirectory() as temp:
    root = Path(temp)
    clip = root / 'clip.gvid'

    def invoke(*args, okay=True):
        p = subprocess.run([str(app), '--input', str(clip), *args], capture_output=True, text=True)
        if (p.returncode == 0) != okay:
            raise AssertionError((args, p.returncode, p.stdout, p.stderr))
        return [json.loads(line) for line in p.stdout.splitlines()]

    for version in (2, 3):
        clip.write_bytes(recording(version))
        for scale in (4, 8, 16):
            directory = root / f'v{version}-s{scale}'
            rows = invoke('--downsample', str(scale), '--limit', '1', '--output-dir', str(directory))
            assert rows[-1]['rendered'] == 1 and rows[-1]['inspected'] == 2
            assert rows[-1]['end_marker_verified'] == (version == 3)
            ppm = (directory / 'run0-frame0.ppm').read_bytes().split(b'\n', 3)[3]
            assert ppm == (fixtures / f'128x96-q3-s{scale}.rgb').read_bytes()
        invoke('--limit', '3', okay=False)
    data = recording(3)
    # A rendering limit must never hide damage to the remainder of a clip.
    for bad in (data[:-48], data + b'x', data[:-49] + bytes([data[-49] ^ 1]) + data[-48:]):
        clip.write_bytes(bad)
        invoke('--limit', '1', okay=False)
    clip.write_bytes(data)
    invoke('--downsample', '3', okay=False)
    invoke('--runs', '0', okay=False)
print('PASS: native v2/v3 playback, independent RGB, strict whole-clip validation with render limit')
