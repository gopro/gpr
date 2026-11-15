#!/usr/bin/env python3
"""Measure captured-GPR upres and verify every result against independent codecs.

This is an offline functional/performance check, not a CNN quality approval.
The reference encoder must support GPR_BENCH_QUALITY as in the stock baseline.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import resource
import subprocess
import tempfile
import time

import numpy as np
import tifffile
import torch
import raw_media
import stock_cnn_metadata
import stock_still as still
import stock_upres
import stock_video


def run(command, environment=None):
    result=subprocess.run(list(map(str,command)),env=environment,capture_output=True,text=True)
    if result.returncode:raise RuntimeError(result.stderr[-8000:])


def benchmark(inputs, checkpoint, sdk, bridge, decoder, reference_encoder, output_dir,
              threads=4, quality=3, tile=256, halo=64):
    if not 1<=threads<=64:raise ValueError('threads must be 1..64')
    torch.set_num_threads(threads)
    started=time.perf_counter()
    report=stock_upres.execute('timelapse',inputs,[],'camera_gpr_replay',checkpoint,sdk,output_dir,
        capture_quality=quality,output_quality=quality,bridge=bridge,input_mode='captured_gpr',
        tile=tile,halo=halo,dng_export=True)
    workflow_s=time.perf_counter()-started;root=Path(output_dir)
    rows=[];environment={k:v for k,v in os.environ.items() if not k.startswith('GPR_')}
    environment['GPR_BENCH_QUALITY']=str(quality)
    with (root/'timelapse.gvid').open('rb') as file:
        header=stock_video.read_header(file)
        for index,frame in enumerate(stock_video.records(file,header)):
            source=Path(inputs[index]);original_meta,original_payload=raw_media.extract(source,14,with_payload=True)
            expected_meta,_=stock_cnn_metadata.scaled(original_meta,2)
            frame_dir=root/Path(report['frames'][index]['full_gpr']).parent
            meta,payload=raw_media.extract(frame_dir/'full.gpr',14,with_payload=True)
            if payload!=frame['payload']:raise ValueError('GVID payload changed')
            if raw_media.extract(frame_dir/'capture.gpr',14,with_payload=True)[1]!=original_payload:
                raise ValueError('original capture payload changed')
            if still.canonical(meta)!=still.canonical(expected_meta) or still.canonical(frame['metadata']['raw_metadata'])!=still.canonical(meta):
                raise ValueError('scaled calibration differs')
            with tempfile.TemporaryDirectory(prefix='.verify-',dir=root) as temp:
                work=Path(temp);raw=work/'model.raw'
                with tifffile.TiffFile(frame_dir/'model-output.dng') as dng:dng.asarray().astype('<u2').tofile(raw)
                expected=work/'reference.vc5'
                run([Path(reference_encoder).resolve(),'-i',raw,'-o',expected,'-w',meta['width'],'-h',meta['height'],
                     '-p',meta['width']*2,'-x','rggb14'],environment)
                if expected.read_bytes()!=payload:raise ValueError('independent stock encoder payload differs')
                encoded=work/'frame.vc5';encoded.write_bytes(payload);decoded=work/'decoded.raw'
                run([Path(decoder).resolve(),'--decode',encoded,'--output',decoded,'--width',meta['width'],
                     '--height',meta['height'],'--cfa','RGGB','--sample-bits',14])
                with tifffile.TiffFile(frame_dir/'decoded-full.dng') as dng:
                    decoded_sha=hashlib.sha256(dng.asarray().astype('<u2').tobytes()).hexdigest()
                if still.digest(decoded)!=decoded_sha:raise ValueError('independent decoded pixels differ')
                rows.append(dict(source_name=source.name,source_sha256=still.digest(source),width=meta['width'],height=meta['height'],
                    full_gpr_sha256=still.digest(frame_dir/'full.gpr'),payload_sha256=hashlib.sha256(payload).hexdigest(),
                    decoded_raw_sha256=decoded_sha,metadata_sha256=hashlib.sha256(still.canonical(meta)).hexdigest(),
                    cnn_s=report['frames'][index]['cnn_s'],independent_payload_exact=True,independent_pixels_exact=True,
                    source_payload_preserved=True,scaled_calibration_exact=True,reencode_loss=report['frames'][index]['reencode_loss']))
        if len(rows)!=len(inputs) or not header['_end_marker_verified']:raise ValueError('incomplete verified sequence')
    result=dict(schema='gpr.stock_upres.benchmark.v1',complete=True,quality_approval=False,
        host=platform.platform(),machine=platform.machine(),torch=torch.__version__,threads=threads,
        input_mode='captured_gpr',input_count=len(inputs),workflow_s=workflow_s,
        workflow_s_per_frame=workflow_s/len(inputs),workflow_fps=len(inputs)/workflow_s,
        verification_and_workflow_s=time.perf_counter()-started,
        process_peak_rss_bytes=resource.getrusage(resource.RUSAGE_SELF).ru_maxrss*(1 if platform.system()=='Darwin' else 1024),
        model=report['model'],tile=tile,halo=halo,quality=quality,frames=rows,
        gvid_sha256=still.digest(root/'timelapse.gvid'),mov_sha256=still.digest(root/'timelapse.mov'),
        binaries={name:still.digest(path) for name,path in dict(sdk=sdk,bridge=bridge,decoder=decoder,reference_encoder=reference_encoder).items()},
        source_files={name:still.digest(Path(__file__).with_name(name)) for name in
            ('benchmark_stock_upres.py','stock_upres.py','stock_sequence.py','stock_video.py','stock_still.py','stock_cnn_metadata.py')},
        limitations='Camera still replay, offline 2x reconstruction, one measured pass; no independent high-resolution sensor truth, temporal quality or model promotion. Workflow includes imports/model load, all local exports and verification performed by the product workflow. Additional independent codec verification is timed separately. Process RSS includes later verification.')
    (root/'benchmark.json').write_text(json.dumps(result,indent=2,allow_nan=False)+'\n')
    return result


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--input',dest='inputs',action='append',type=Path,required=True)
    for name in ('checkpoint','sdk','bridge','decoder','reference-encoder','output-dir'):p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--threads',type=int,default=4);p.add_argument('--quality',type=int,choices=range(6),default=3)
    p.add_argument('--tile',type=int,default=256);p.add_argument('--halo',type=int,default=64)
    print(json.dumps(benchmark(**vars(p.parse_args())),indent=2))
