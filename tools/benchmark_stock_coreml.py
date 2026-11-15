#!/usr/bin/env python3
"""Compare native Core ML with Torch on the same decoded stock capture.

Backend numerical agreement is distinct from sensor/detail fidelity approval.
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
import raw_media
import stock_coreml
import stock_still
import stock_video


def benchmark(source, checkpoint, coreml_package, sdk, output_dir, units=('cpu','gpu','ane','all'),
              threads=4,tile=256,halo=64):
    if not units or any(unit not in ('cpu','gpu','ane','all') for unit in units):raise ValueError('invalid Core ML units')
    if not 1<=threads<=64:raise ValueError('threads must be 1..64')
    torch.set_num_threads(threads);source=Path(source).resolve();root=Path(output_dir).resolve()
    root.mkdir(parents=True,exist_ok=False)
    decoded=stock_still.decode(source,14,sdk,root/'decoded.dng')
    with tifffile.TiffFile(root/'decoded.dng') as file:pixels=file.asarray()
    h,w=pixels.shape;raw=root/'decoded.raw';pixels.astype('<u2').tofile(raw);del pixels
    model,info=stock_video.load_cnn(None,checkpoint,torch.device('cpu'));scale=info['output_scale']
    reference,portable_s=stock_video.restore(raw,w,h,model,torch.device('cpu'),tile,halo,scale)
    report=dict(schema='gpr.stock_coreml.benchmark.v1',complete=False,quality_approval=False,
        source_name=source.name,source_sha256=stock_still.digest(source),decoded_raw_sha256=stock_still.digest(raw),
        source_metadata_sha256=decoded['metadata_sha256'],input_width=w,input_height=h,output_scale=scale,
        torch_model=info,torch_s=portable_s,threads=threads,tile=tile,halo=halo,host=platform.platform(),
        sdk_sha256=stock_still.digest(sdk),rows=[],
        reference_kind='same_checkpoint_torch_backend',
        limitations='One measured inference pass per requested setting, model load timed separately; compiled-model caches are not flushed and this is not sustained throughput evidence. Core ML may assign operations to any allowed compute unit; ANE/GPU execution is not proven by a units selection. Numeric agreement does not establish sensor fidelity, motion quality or model approval.')
    def save():
        (root/'benchmark.json').write_text(json.dumps(report,indent=2,allow_nan=False)+'\n')
    save()
    for unit in units:
        started=time.perf_counter()
        native,native_info=stock_video.load_cnn(None,checkpoint,torch.device('cpu'),coreml_package,unit)
        load_s=time.perf_counter()-started
        actual,elapsed=stock_video.restore(raw,w,h,native,torch.device('cpu'),tile,halo,scale)
        error=actual.astype(np.int32)-reference.astype(np.int32)
        report['rows'].append(dict(units=unit,model=native_info,load_s=load_s,inference_s=elapsed,
            speedup_vs_torch=portable_s/elapsed,max_abs_count_difference=int(np.max(np.abs(error))),
            changed_samples=int(np.count_nonzero(error)),mean_abs_count_difference=float(np.mean(np.abs(error))),
            rmse_counts=float(np.mean(np.square(error,dtype=np.float64)))**.5,
            torch_raw_sha256=hashlib.sha256(reference.astype('<u2').tobytes()).hexdigest(),
            native_raw_sha256=hashlib.sha256(actual.astype('<u2').tobytes()).hexdigest(),quality_approval=False))
        save();del actual,error,native
    report.update(complete=True,source_files={name:stock_still.digest(Path(__file__).with_name(name)) for name in
        ('benchmark_stock_coreml.py','stock_coreml.py','stock_video.py','stock_cnn_models.py')})
    if stock_still.digest(source)!=report['source_sha256']:raise ValueError('source changed during benchmark')
    save();return report


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('source','checkpoint','coreml-package','sdk','output-dir'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--units',nargs='+',choices=('cpu','gpu','ane','all'),default=['cpu','gpu','ane','all'])
    parser.add_argument('--threads',type=int,default=4);parser.add_argument('--tile',type=int,default=256)
    parser.add_argument('--halo',type=int,default=64)
    print(json.dumps(benchmark(**vars(parser.parse_args())),indent=2))
