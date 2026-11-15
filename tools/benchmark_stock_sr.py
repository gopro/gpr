#!/usr/bin/env python3
"""Evaluate explicit 2x checkpoints on simulated stock-Q3 low-res RAW.

The source is independently decoded lossless RGGB14 DNG. Same-site 2x2 area
averaging generates a known low-resolution input, followed by actual stock
encoding/decoding. Reconstructed RAW is compared with the original high-res
counts and with bicubic reconstruction of the very same decoded low-res RAW.
This is a simulation/model-selection experiment, not a sensor/video approval.
"""
import argparse
import hashlib
import json
from pathlib import Path
import platform

import numpy as np
import tifffile
import torch

from benchmark_stills import bayer_page,quality
import raw_media
import stock_cnn_metadata
import stock_still
import stock_video


def simulation_metadata(source):
    return stock_cnn_metadata.simulated_half(source)


class Bicubic(torch.nn.Module):
    def forward(self,x):
        return torch.nn.functional.interpolate(x,scale_factor=2,mode='bicubic',align_corners=False).clamp(0,1)


def psnr_delta(candidate,baseline):
    # Perfect reconstruction has infinite PSNR, represented as JSON null.
    if candidate is None or baseline is None:
        return 0. if candidate is None and baseline is None else None
    return candidate-baseline


def evaluate(source,sdk,checkpoints,output_dir,device='cpu',tile=256,halo=64,cnn_root=None):
    source=Path(source).resolve(); root=Path(output_dir).resolve()
    meta=raw_media.extract(source,14); w,h=meta['width'],meta['height']
    if meta['cfa']!='RGGB' or w%4 or h%4: raise ValueError('simulation requires RGGB14 dimensions divisible by four')
    with tifffile.TiffFile(source) as file:
        page=bayer_page(file.pages)
        if int(page.compression) not in (1,7,8,32946): raise ValueError('requires a lossless sensor DNG reference')
        reference=page.asarray()
    if reference.shape!=(h,w) or reference.dtype!=np.dtype('<u2') or np.any(reference>16383):
        raise ValueError('reference is outside RGGB14 count/geometry contract')
    low_meta,omitted=simulation_metadata(meta)
    root.mkdir(parents=True,exist_ok=False)
    independent=root/'independent-sdk.raw'
    stock_still.run(sdk,['-i',source,'-o',independent],None)
    if not np.array_equal(np.fromfile(independent,dtype='<u2').reshape(h,w),reference):
        raise ValueError('independent source decoders disagree')
    planes=np.stack([reference[0::2,0::2],reference[0::2,1::2],reference[1::2,0::2],reference[1::2,1::2]]).astype(np.uint32)
    small=((planes[:,0::2,0::2]+planes[:,0::2,1::2]+planes[:,1::2,0::2]+planes[:,1::2,1::2]+2)//4).astype('<u2')
    low=np.empty((h//2,w//2),dtype='<u2')
    for site in range(4): low[site//2::2,site%2::2]=small[site]
    low_path=root/'simulated-low.raw'; low.tofile(low_path)
    gpr=root/'simulated-low-q3.gpr'; encoded=stock_still.encode(low_path,low_meta,3,sdk,gpr)
    plain=root/'decoded-low.dng'; decoded=stock_still.decode(gpr,14,sdk,plain)
    with tifffile.TiffFile(plain) as file: low=file.asarray()
    low.tofile(root/'decoded-low.raw')
    baseline,baseline_s=stock_video.restore(root/'decoded-low.raw',w//2,h//2,Bicubic(),torch.device(device),tile,halo,scale=2)
    base_metrics=quality(reference,baseline,meta)
    rows=[]; report=None
    for index,checkpoint in enumerate(checkpoints):
        destination=root/f'candidate-{index}.dng'
        receipt=stock_still.decode(gpr,14,sdk,destination,reconstruction=dict(root=cnn_root,checkpoint=checkpoint,device=device,tile=tile,halo=halo,scale=2))
        if receipt['raw_sha256']!=decoded['raw_sha256']: raise ValueError('candidate did not receive identical decoded input')
        with tifffile.TiffFile(destination) as file: pixels=file.asarray()
        if pixels.shape!=(h,w) or hashlib.sha256(pixels.astype('<u2').tobytes()).hexdigest()!=receipt['restored_raw_sha256']:
            raise ValueError('editable high-resolution pixel identity differs')
        expected,geometry=stock_cnn_metadata.scaled(low_meta,2)
        if stock_still.canonical(raw_media.extract(destination,14))!=stock_still.canonical(expected):
            raise ValueError('editable high-resolution calibration differs')
        metrics=quality(reference,pixels,meta)
        row=dict(checkpoint_name=Path(checkpoint).name,model=receipt['model'],cnn_s=receipt['cnn_s'],
                 restored_raw_sha256=receipt['restored_raw_sha256'],editable_dng_sha256=receipt['output_sha256'],
                 geometry=geometry,metrics=metrics,active_psnr_delta_vs_bicubic_db=psnr_delta(metrics['active']['psnr_db'],base_metrics['active']['psnr_db']))
        rows.append(row)
        report=dict(schema='gpr.stock_sr_benchmark.v1',complete=False,quality_approval=False,
                    host=platform.platform(),torch=torch.__version__,device=device,width=w,height=h,
                    input_width=w//2,input_height=h//2,cfa='RGGB',sample_bits=14,output_scale=2,
                    source_sha256=stock_still.digest(source),reference_raw_sha256=stock_still.digest(independent),
                    independent_lossless_decode_verified=True,simulated_low_raw_sha256=stock_still.digest(low_path),
                    low_gpr_sha256=encoded['output_sha256'],low_gpr_bytes=encoded['output_bytes'],
                    low_decoded_raw_sha256=decoded['raw_sha256'],sdk_sha256=stock_still.digest(sdk),
                    simulated_metadata_sha256=hashlib.sha256(stock_still.canonical(low_meta)).hexdigest(),
                    simulation_metadata_omissions=omitted,stock_quality=3,tile_plane_pixels=tile,halo_plane_pixels=halo,
                    bicubic_metrics=base_metrics,bicubic_s=baseline_s,
                    bicubic_raw_sha256=hashlib.sha256(baseline.astype('<u2').tobytes()).hexdigest(),rows=rows,
                    source_files={name:stock_still.digest(Path(__file__).parent/name) for name in
                                  ('benchmark_stock_sr.py','benchmark_stills.py','stock_video.py','stock_still.py','stock_cnn_models.py','stock_cnn_metadata.py','raw_media.py')},
                    limitations='Same-site rounded 2x2 area average, then actual stock Q3: not the previous fused decimator, sensor footage or temporal evidence. Checkpoint training/split provenance requires its separate receipt; inference support is not quality approval. Timing is diagnostic tiled inference, not capture throughput.')
        (root/'benchmark.json').write_text(json.dumps(report,indent=2)+'\n')
        print(json.dumps(dict(checkpoint=row['checkpoint_name'],bicubic_psnr_db=base_metrics['active']['psnr_db'],
                              model_psnr_db=metrics['active']['psnr_db'],delta_db=row['active_psnr_delta_vs_bicubic_db'])),flush=True)
    if report is None: raise ValueError('at least one candidate checkpoint is required')
    report['complete']=True
    (root/'benchmark.json').write_text(json.dumps(report,indent=2)+'\n')
    return report


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,required=True);parser.add_argument('--sdk',type=Path,required=True)
    parser.add_argument('--checkpoint',type=Path,action='append',required=True)
    parser.add_argument('--cnn-root',type=Path);parser.add_argument('--output-dir',type=Path,required=True)
    parser.add_argument('--device',choices=('cpu','mps','cuda'),default='cpu')
    parser.add_argument('--tile',type=int,default=256);parser.add_argument('--halo',type=int,default=64)
    args=parser.parse_args()
    evaluate(args.source,args.sdk,args.checkpoint,args.output_dir,args.device,args.tile,args.halo,args.cnn_root)


if __name__=='__main__': main()
