#!/usr/bin/env python3
"""Measure MPSGraph and hybrid Metal agreement with the same Torch checkpoint."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import time
import numpy as np
import tifffile
import torch
import stock_metal
import stock_still
import stock_video


def benchmark(source,checkpoint,weights,library,kernels,sdk,output_dir,threads=4,tile=256,halo=64):
    if not 1<=threads<=64:raise ValueError('threads must be 1..64')
    torch.set_num_threads(threads);root=Path(output_dir).resolve();root.mkdir(parents=True,exist_ok=False)
    source=Path(source).resolve();decoded=stock_still.decode(source,14,sdk,root/'decoded.dng')
    with tifffile.TiffFile(root/'decoded.dng') as file:pixels=file.asarray()
    h,w=pixels.shape;raw=root/'decoded.raw';pixels.astype('<u2').tofile(raw);del pixels
    portable,model_info=stock_video.load_cnn(None,checkpoint,torch.device('cpu'));scale=model_info['output_scale']
    reference,torch_s=stock_video.restore(raw,w,h,portable,torch.device('cpu'),tile,halo,scale)
    report=dict(schema='gpr.stock_metal.benchmark.v1',complete=False,quality_approval=False,
        source_name=source.name,source_sha256=stock_still.digest(source),decoded_raw_sha256=stock_still.digest(raw),
        source_metadata_sha256=decoded['metadata_sha256'],input_width=w,input_height=h,output_scale=scale,
        host=platform.platform(),torch=torch.__version__,torch_model=model_info,torch_s=torch_s,
        threads=threads,tile=tile,halo=halo,rows=[],sdk_sha256=stock_still.digest(sdk),
        reference_kind='same_checkpoint_torch_backend',
        limitations='One measured pass, no cache flush or sustained-throughput qualification. Native inference includes graph/context creation on spatial cache misses; two-context LRU. Native residuals use fp16 with BN folded into convolutions; baseline and integer conversion stay on the current host path. Backend agreement is not sensor/detail/motion quality approval.')
    def save():(root/'benchmark.json').write_text(json.dumps(report,indent=2,allow_nan=False)+'\n')
    save()
    for backend in ('mpsgraph','hybrid'):
        config=dict(weights=weights,library=library,kernels=kernels,backend=backend)
        native,info=stock_video.load_cnn(None,checkpoint,torch.device('cpu'),metal=config)
        actual,elapsed=stock_video.restore(raw,w,h,native,torch.device('cpu'),tile,halo,scale)
        error=actual.astype(np.int32)-reference.astype(np.int32)
        report['rows'].append(dict(backend=backend,model=info,inference_s=elapsed,speedup_vs_torch=torch_s/elapsed,
            max_abs_count_difference=int(np.abs(error).max()),mean_abs_count_difference=float(np.abs(error).mean()),
            rmse_counts=float(np.mean(np.square(error,dtype=np.float64)))**.5,changed_samples=int(np.count_nonzero(error)),
            native_raw_sha256=hashlib.sha256(actual.astype('<u2').tobytes()).hexdigest(),
            torch_raw_sha256=hashlib.sha256(reference.astype('<u2').tobytes()).hexdigest(),quality_approval=False))
        native.close();del native,actual,error;save()
    report.update(complete=True,source_files={name:stock_metal.digest(Path(__file__).with_name(name)) for name in
        ('benchmark_stock_metal.py','stock_metal.py','stock_video.py','stock_cnn_models.py')})
    report['native_sources']={path.name:stock_metal.digest(path) for path in
        (Path(__file__).resolve().parents[1]/'source/app/stock_metal').iterdir() if path.is_file()}
    if stock_still.digest(source)!=report['source_sha256']:raise ValueError('source changed during benchmark')
    save();return report


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('source','checkpoint','weights','library','kernels','sdk','output-dir'):p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--threads',type=int,default=4);p.add_argument('--tile',type=int,default=256)
    p.add_argument('--halo',type=int,default=64)
    print(json.dumps(benchmark(**vars(p.parse_args())),indent=2))
