#!/usr/bin/env python3
"""Standard-library benchmark of scalar versus vector native reconstruction."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import time
import raw_media
import stock_still
import stock_native


def benchmark(source,checkpoint,artifact,backend,native,sdk,output_dir,kernels=None,units='cpu',threads=4,tile=256,halo=64):
    root=Path(output_dir).resolve();root.mkdir(parents=True,exist_ok=False)
    meta,payload=raw_media.extract(Path(source),14,with_payload=True)
    if meta['cfa']!='RGGB':raise ValueError('requires RGGB14 source')
    carrier=root/'carrier.gpr';raw_media.write(carrier,stock_still.carrier(meta),payload,'gpr')
    raw=root/'decoded.raw';stock_still.run(sdk,['-i',carrier,'-o',raw],os.environ.copy())
    report=dict(schema='gpr.native_reconstruct.benchmark.v1',complete=False,quality_approval=False,
        source_name=Path(source).name,source_sha256=stock_still.digest(source),decoded_raw_sha256=stock_still.digest(raw),
        metadata_sha256=hashlib.sha256(stock_still.canonical(meta)).hexdigest(),
        host=platform.platform(),backend=backend,sdk_sha256=stock_still.digest(sdk),rows=[],
        limitations='One sequential pass per host kernel; compiled-kernel caches not flushed. Restore timer includes tile/context work, host conversion and writes to mapped output; model load, input validation and final durability/publication are separate. Scalar/vector identity is not model/detail/motion approval.')
    def save():(root/'benchmark.json').write_text(json.dumps(report,indent=2)+'\n')
    save()
    for kernel in ('scalar','auto'):
        output=root/f'{kernel}.raw'
        command=[Path(native).resolve(),'--input',raw,'--output',output,'--width',meta['width'],'--height',meta['height'],
            '--scale',2,'--checkpoint',Path(checkpoint).resolve(),'--artifact',Path(artifact).resolve(),
            '--backend',backend,'--units',units,'--threads',threads,'--tile',tile,'--halo',halo,'--host-kernel',kernel]
        if kernels:command+=['--kernels',Path(kernels).resolve()]
        start=time.perf_counter();result=subprocess.run(list(map(str,command)),check=True,capture_output=True,text=True)
        elapsed=time.perf_counter()-start;receipt=json.loads(result.stdout)
        if stock_still.digest(output)!=receipt['output_sha256']:raise ValueError('native raw output receipt differs')
        report['rows'].append(dict(host_kernel=receipt['host_kernel'],restore_s=receipt['restore_s'],
            process_wall_s=elapsed,receipt=receipt));save()
    if report['rows'][0]['receipt']['output_sha256']!=report['rows'][1]['receipt']['output_sha256']:
        raise ValueError('scalar/vector native counts differ')
    report.update(complete=True,scalar_vector_counts_exact=True,
        host_kernel_speedup=report['rows'][0]['restore_s']/report['rows'][1]['restore_s'],
        source_files={name:stock_still.digest(Path(__file__).with_name(name)) for name in
            ('benchmark_stock_native.py','stock_native.py','stock_still.py')})
    folder=Path(__file__).resolve().parents[1]/'source/app/stock_metal'
    report['native_sources']={path.name:stock_still.digest(path) for path in folder.iterdir() if path.is_file()}
    if stock_still.digest(source)!=report['source_sha256']:raise ValueError('source changed during benchmark')
    save();return report


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('source','checkpoint','artifact','native','sdk','output-dir'):p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--backend',choices=('coreml','mpsgraph','hybrid'),required=True)
    p.add_argument('--kernels',type=Path);p.add_argument('--units',choices=('cpu','gpu','ane','all'),default='cpu')
    p.add_argument('--threads',type=int,default=4);p.add_argument('--tile',type=int,default=256);p.add_argument('--halo',type=int,default=64)
    print(json.dumps(benchmark(**vars(p.parse_args())),indent=2))
