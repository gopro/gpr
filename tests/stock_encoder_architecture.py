#!/usr/bin/env python3
"""Compare every stock quality preset against a separately built parent codec.

Both sample apps must accept GPR_BENCH_QUALITY. The candidate additionally
accepts GPR_STREAM_UNPACK. Output bytes, rather than decoded similarity, are
the compatibility criterion. Synthetic inputs exercise tails and zero runs.
"""
import argparse
import array
import hashlib
import json
import os
from pathlib import Path
import random
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--reference', required=True, type=Path)
parser.add_argument('--candidate', required=True, type=Path)
parser.add_argument('--samples', required=True, type=Path)
args = parser.parse_args()
reference, candidate = args.reference.resolve(), args.candidate.resolve()
cases = []
for path in sorted(args.samples.glob('*.raw')):
    size = path.stat().st_size
    if size == 8192 * 6144 * 2:
        cases.append((path, 8192, 6144))
    elif size == 4096 * 3072 * 2:
        cases.append((path, 4096, 3072))

with tempfile.TemporaryDirectory(prefix='gpr-architecture-check-') as directory:
    temporary = Path(directory)
    rng = random.Random(0x5635)
    for pattern in ('zero', 'maximum', 'alternating', 'random'):
        for width in (72, 80, 128):
            height = 96
            pixels = array.array('H', (
                0 if pattern == 'zero' else
                16383 if pattern == 'maximum' else
                (16383 if i % 2 else 0) if pattern == 'alternating' else
                rng.randrange(16384)
                for i in range(width * height)))
            path = temporary / f'{pattern}-{width}.raw'
            path.write_bytes(pixels.tobytes())
            cases.append((path, width, height))

    for path, width, height in cases:
        for quality in range(6):
            hashes = []
            for name, tool in (('reference', reference), ('candidate', candidate)):
                output = temporary / f'{name}.vc5'
                env = os.environ.copy()
                env['GPR_BENCH_QUALITY'] = str(quality)
                env['ASAN_OPTIONS'] = 'halt_on_error=1'
                env['UBSAN_OPTIONS'] = 'halt_on_error=1:print_stacktrace=1'
                env.pop('GPR_BENCH_RUNS', None)
                env.pop('GPR_BENCH_ARENA', None)
                if name == 'candidate':
                    env['GPR_STREAM_UNPACK'] = '1'
                else:
                    env.pop('GPR_STREAM_UNPACK', None)
                result = subprocess.run([str(tool), '-i', str(path), '-o', str(output),
                    '-w', str(width), '-h', str(height), '-p', str(width * 2)],
                    env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                if result.returncode:
                    raise RuntimeError(result.stdout.decode(errors='replace'))
                hashes.append(hashlib.sha256(output.read_bytes()).hexdigest())
            if hashes[0] != hashes[1]:
                raise AssertionError((path.name, quality, hashes))
            print(json.dumps({'input': path.name, 'quality': quality,
                'sha256': hashes[0], 'byte_identical': True}), flush=True)
    print(json.dumps({'passed': len(cases) * 6, 'cases': len(cases)}))
