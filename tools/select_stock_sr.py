#!/usr/bin/env python3
"""Select explicit 2x stock-Q3 candidates using complete validation images.

Uses a completed train_stock_sr dataset's validation split only. Final-test
sources are hashed for split auditing but never decoded or evaluated here.
A selected checkpoint is a validation result, not final quality approval.
Optional analysis dependencies match benchmark_stock_sr.py.
"""
import argparse
import json
import math
from pathlib import Path
import platform
import time

import torch

import benchmark_stock_sr
import stock_still
import stock_video
import train_stock_sr

REGIONS=('active','shadow_bottom_decile','highlight_top_decile','detail_top_decile')


def select(dataset,sdk,checkpoints,output_dir,device='cpu',tile=256,halo=64):
    dataset=Path(dataset).resolve();sdk=Path(sdk).resolve()
    if not checkpoints:raise ValueError('at least one explicit candidate is required')
    if (type(tile) is not int or type(halo) is not int or tile<8 or tile%8 or halo<0 or halo%8):
        raise ValueError('tile/halo must be multiples of eight; tile must be positive')
    manifest=dataset/'dataset.json';info=json.loads(manifest.read_text())
    if (info.get('schema')!='gpr.stock_sr_dataset.v1' or info.get('complete') is not True or
        tuple(info.get(k) for k in ('cfa','sample_bits','stock_quality','output_scale','dtype'))!=('RGGB',14,3,2,'<u2')):
        raise ValueError('requires a completed stock-Q3 RGGB14 scale-two dataset')
    dataset_hash=stock_still.digest(manifest)
    for name,key in (('inputs.npy','input_sha256'),('targets.npy','target_sha256')):
        if stock_still.digest(dataset/name)!=info[key]:raise ValueError('dataset array hash differs')
    sources=train_stock_sr.sources(dict(schema='gpr.stock_sr_sources.v1',sources=info['sources']))
    for row in info['sources']:
        if row['split']=='test' and (row['patch_start'] is not None or row['patch_count']!=0):
            raise ValueError('final-test pixels leaked into patch arrays')
    validation=[row for row in sources if row['split']=='validation']
    candidates=[];seen=set()
    for path in checkpoints:
        path=Path(path).resolve();model,contract=stock_video.load_cnn(None,path,torch.device('cpu'))
        del model
        if contract['output_scale']!=2:raise ValueError('full-frame SR selection requires scale-two candidates')
        digest=stock_still.digest(path)
        if digest in seen:raise ValueError('duplicate candidate checkpoint')
        seen.add(digest);candidates.append(dict(path=str(path),sha256=digest))
    sdk_hash=stock_still.digest(sdk)
    root=Path(output_dir).resolve();root.mkdir(parents=True,exist_ok=False)
    protocol=dict(schema='gpr.stock_sr_full_selection_protocol.v1',dataset_sha256=dataset_hash,
                  dataset_input_sha256=info['input_sha256'],dataset_target_sha256=info['target_sha256'],
                  sdk_sha256=sdk_hash,validation_sources=validation,candidates=candidates,
                  device=device,tile_plane_pixels=tile,halo_plane_pixels=halo,regions=list(REGIONS),
                  selection='lowest sample-weighted active MSE strictly below bicubic; no regional RMSE increase on any complete validation image; first explicit candidate wins an exact tie',
                  final_reference_pixels_used=False,quality_approval=False,
                  source_files={name:stock_still.digest(Path(__file__).parent/name) for name in
                                ('select_stock_sr.py','benchmark_stock_sr.py','benchmark_stills.py','train_stock_sr.py',
                                 'stock_still.py','stock_video.py','stock_cnn_models.py','stock_cnn_metadata.py','raw_media.py')})
    protocol_path=root/'protocol.json';protocol_path.write_text(json.dumps(protocol,indent=2,allow_nan=False)+'\n')
    report=dict(schema='gpr.stock_sr_full_selection.v1',complete=False,quality_approval=False,
                protocol_sha256=stock_still.digest(protocol_path),host=platform.platform(),torch=torch.__version__,
                final_reference_pixels_used=False,validation_sources=[],candidates=[],selected=None)
    totals=[0. for _ in candidates];baseline_total=0.;sample_total=0
    regressions=[[] for _ in candidates];started=time.perf_counter()
    for index,source in enumerate(validation):
        if stock_still.digest(Path(source['path']))!=source['sha256']:raise ValueError('validation source changed')
        for candidate in candidates:
            if stock_still.digest(Path(candidate['path']))!=candidate['sha256']:raise ValueError('candidate checkpoint changed')
        result=benchmark_stock_sr.evaluate(source['path'],sdk,[Path(c['path']) for c in candidates],
                                            root/f'validation-{index}',device,tile,halo)
        if (result['source_sha256']!=source['sha256'] or result['sdk_sha256']!=protocol['sdk_sha256'] or
            len(result['rows'])!=len(candidates)):
            raise ValueError('validation benchmark binding differs')
        base=result['bicubic_metrics'];top,left,bottom,right=base['active_area']
        samples=(bottom-top)*(right-left)
        if samples<=0:raise ValueError('empty validation active area')
        row=dict(source=source,benchmark_sha256=stock_still.digest(root/f'validation-{index}/benchmark.json'),
                 active_samples=samples,bicubic_metrics=base,candidates=[])
        for pos,(candidate,measured) in enumerate(zip(candidates,result['rows'])):
            if measured['model']['checkpoint_sha256']!=candidate['sha256']:
                raise ValueError('measured checkpoint differs')
            metrics=measured['metrics'];failed=[]
            for region in REGIONS:
                a,b=metrics[region]['rmse_counts'],base[region]['rmse_counts']
                if not math.isfinite(a) or not math.isfinite(b) or a<0 or b<0:
                    raise ValueError('invalid regional error')
                if a>b:failed.append(region)
            totals[pos]+=metrics['active']['rmse_counts']**2*samples
            regressions[pos].extend(dict(source_index=index,region=region) for region in failed)
            row['candidates'].append(dict(checkpoint_sha256=candidate['sha256'],metrics=metrics,
                                          regressions=failed,passed_regional_guard=not failed))
        baseline_total+=base['active']['rmse_counts']**2*samples;sample_total+=samples
        report['validation_sources'].append(row)
        (root/'selection.json').write_text(json.dumps(report,indent=2,allow_nan=False)+'\n')
    baseline=baseline_total/sample_total;best=baseline
    for candidate,total,failed in zip(candidates,totals,regressions):
        mse=total/sample_total;eligible=not failed and mse<baseline
        row=dict(checkpoint=candidate,active_mse_counts=mse,regressions=failed,eligible=eligible,
                 active_psnr_gain_db=train_stock_sr.delta(baseline,mse))
        report['candidates'].append(row)
        if eligible and mse<best:report['selected']=row;best=mse
    for candidate in candidates:
        if stock_still.digest(Path(candidate['path']))!=candidate['sha256']:raise ValueError('candidate checkpoint changed')
    if stock_still.digest(manifest)!=dataset_hash:raise ValueError('dataset manifest changed')
    if stock_still.digest(sdk)!=sdk_hash:raise ValueError('SDK executable changed')
    report.update(complete=True,active_samples=sample_total,bicubic_active_mse_counts=baseline,
                  elapsed_s=time.perf_counter()-started,
                  limitation='Validation selection only. Historical or newly unseen final references, broader-camera, rendered and temporal quality require separate frozen evaluation. Supplied scene-group labels do not establish perceptual disjointness.')
    (root/'selection.json').write_text(json.dumps(report,indent=2,allow_nan=False)+'\n')
    print(json.dumps(dict(selected=report['selected'],candidates=len(candidates),validation_sources=len(validation),
                          quality_approval=False,elapsed_s=report['elapsed_s']),allow_nan=False),flush=True)
    return report


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dataset',type=Path,required=True);parser.add_argument('--sdk',type=Path,required=True)
    parser.add_argument('--checkpoint',type=Path,action='append',required=True)
    parser.add_argument('--output-dir',type=Path,required=True)
    parser.add_argument('--device',choices=('cpu','mps','cuda'),default='cpu')
    parser.add_argument('--tile',type=int,default=256);parser.add_argument('--halo',type=int,default=64)
    args=parser.parse_args();select(args.dataset,args.sdk,args.checkpoint,args.output_dir,args.device,args.tile,args.halo)


if __name__=='__main__':main()
