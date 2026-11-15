#!/usr/bin/env python3
"""Alternate fresh-process stock and streaming encodes; time only codec calls."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import tempfile

p = argparse.ArgumentParser()
p.add_argument('--reference', type=Path, required=True)
p.add_argument('--candidate', type=Path, required=True)
p.add_argument('--samples', type=Path, required=True)
p.add_argument('--runs', type=int, default=5)
a = p.parse_args()
with tempfile.TemporaryDirectory(prefix='gpr-wall-bench-') as directory:
    output = Path(directory) / 'output.vc5'
    for sample in sorted(a.samples.glob('*.raw')):
        size = sample.stat().st_size
        if size == 8192 * 6144 * 2:
            width, height = 8192, 6144
        elif size == 4096 * 3072 * 2:
            width, height = 4096, 3072
        else:
            continue
        measurements = {'reference': [], 'candidate': []}
        expected = None
        for run in range(a.runs + 1):
            order = ('reference', 'candidate') if run % 2 else ('candidate', 'reference')
            for version in order:
                tool = a.reference if version == 'reference' else a.candidate
                env = os.environ.copy()
                for name in ('GPR_BENCH_ARENA', 'GPR_BENCH_RUNS', 'GPR_PROFILE', 'GPR_BENCH_QUALITY'):
                    env.pop(name, None)
                env['GPR_STREAM_UNPACK'] = '1'
                result = subprocess.run([str(tool.resolve()), '-i', str(sample.resolve()),
                    '-o', str(output), '-w', str(width), '-h', str(height), '-p', str(width*2)],
                    env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=True)
                text = result.stdout.decode()
                wall = float(re.search(r'Encode wall ([0-9.]+) seconds', text).group(1)) * 1000
                digest = hashlib.sha256(output.read_bytes()).hexdigest()
                if expected is None:
                    expected = digest
                assert digest == expected, (sample.name, version, run, digest, expected)
                if run:
                    measurements[version].append(wall)
        summary = {'input': sample.name, 'dimensions': [width, height], 'sha256': expected}
        for version, values in measurements.items():
            summary[version] = {'median_ms': statistics.median(values),
                'min_ms': min(values), 'max_ms': max(values), 'samples_ms': values}
        summary['speedup'] = summary['reference']['median_ms'] / summary['candidate']['median_ms']
        print(json.dumps(summary), flush=True)
