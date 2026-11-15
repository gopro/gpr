#!/usr/bin/env python3
"""Measure stock still presets against a declared reference, without a CNN.

Development dependencies: numpy/tifffile (imagecodecs for independent source
DNG decompression). Production encoding uses the stdlib stock_still workflow.
"""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import statistics
import subprocess
import time
import numpy as np
import tifffile
import raw_media
import stock_still


def quality(reference,decoded,meta):
    expected=np.asarray(reference,dtype=np.int32); actual=np.asarray(decoded,dtype=np.int32)
    error=actual-expected; peak=(1<<meta['sample_bits'])-1
    tags={t['code']:t for t in meta['image']}
    area=tuple(map(int,raw_media.values(tags[50829]))) if 50829 in tags else (0,0,meta['height'],meta['width'])
    t,l,b,r=area; active=error[t:b,l:r]; ref=expected[t:b,l:r]
    def measure(values):
        mse=float(np.mean(np.square(values,dtype=np.float64)))
        return dict(rmse_counts=mse**.5,psnr_db=None if mse==0 else float(20*np.log10(peak)-10*np.log10(mse)))
    result=dict(full_frame=measure(error),active=measure(active),active_area=area,
                active_abs_error_percentiles=dict(zip(('p50','p90','p99','p99_9'),map(float,np.percentile(np.abs(active),(50,90,99,99.9))))),
                active_max_abs_error=int(np.max(np.abs(active))),active_bias_counts=float(np.mean(active)))
    # Reference-only masks are fixed across quality presets. Two-pixel steps
    # compare the same Bayer site, avoiding a false color-checkerboard edge.
    gradient=np.abs(ref[2:,:-2]-ref[:-2,:-2])+np.abs(ref[:-2,2:]-ref[:-2,:-2])
    threshold=float(np.percentile(gradient,90)); mask=gradient>=threshold
    result['detail_gradient_threshold_counts']=threshold
    result['detail_top_decile']=measure(active[:-2,:-2][mask])
    low,high=np.percentile(ref,(10,90))
    result['shadow_bottom_decile']=measure(active[ref<=low])
    result['highlight_top_decile']=measure(active[ref>=high])
    result['sites']=[]
    for site,color in enumerate(meta['cfa']):
        yy=(site//2-t)%2; xx=(site%2-l)%2
        result['sites'].append(dict(site=site,color=color,**measure(active[yy::2,xx::2])))
    return result


def bayer_page(pages):
    for page in pages:
        if 33422 in page.tags: return page
        if page.pages:
            found=bayer_page(page.pages)
            if found is not None: return found


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source',type=Path,required=True)
    p.add_argument('--sample-bits',type=int,choices=(12,14,16),required=True)
    p.add_argument('--reference-kind',choices=('lossless_sensor_dng','camera_gpr_replay'),required=True)
    p.add_argument('--raw',type=Path,help='optional predecoded source; checked against a fresh SDK decode')
    p.add_argument('--sdk',type=Path,required=True); p.add_argument('--output-dir',type=Path,required=True)
    p.add_argument('--repeats',type=int,default=3); args=p.parse_args()
    if not 1<=args.repeats<=20: p.error('repeats must be 1..20')
    root=args.output_dir.resolve(); root.mkdir(parents=True,exist_ok=False)
    sdk=args.sdk.resolve(); source=args.source.resolve()
    meta=raw_media.extract(source,args.sample_bits); w,h=meta['width'],meta['height']
    raw=root/'reference.raw'
    begin=time.perf_counter()
    subprocess.run([str(sdk),'-i',str(source),'-o',str(raw)],capture_output=True,check=True)
    input_decode_s=time.perf_counter()-begin
    if args.raw and stock_still.digest(args.raw)!=stock_still.digest(raw): raise ValueError('supplied RAW differs from fresh SDK decode')
    if raw.stat().st_size!=w*h*2: raise ValueError('reference dimensions differ')
    reference=np.memmap(raw,mode='r',dtype='<u2',shape=(h,w))
    independent=False
    with tifffile.TiffFile(source) as file:
        page=bayer_page(file.pages)
        if args.reference_kind=='lossless_sensor_dng':
            if int(page.compression) not in (1,7,8,32946): raise ValueError('reference is not a supported lossless DNG')
            # Failure to independently decode is a failed gate, not a skipped
            # quality reference check. Install imagecodecs if needed.
            independent=np.array_equal(page.asarray(),reference)
            if not independent: raise ValueError('independent source DNG decode differs from SDK reference')
        elif int(page.compression)!=9: raise ValueError('camera replay reference requires VC-5 GPR')
    rows=[]
    for q in range(6):
        attempts=[]; metrics=None; payload_sha=None
        for repeat in range(args.repeats):
            gpr=root/f'q{q}-r{repeat}.gpr'; dng=root/f'q{q}-r{repeat}.dng'
            encoded=stock_still.encode(raw,meta,q,sdk,gpr)
            decoded=stock_still.decode(gpr,args.sample_bits,sdk,dng)
            if decoded['metadata_sha256']!=encoded['metadata_sha256']: raise ValueError('calibration changed')
            if payload_sha and encoded['payload_sha256']!=payload_sha: raise ValueError('encode is nondeterministic')
            payload_sha=encoded['payload_sha256']
            if repeat==0:
                with tifffile.TiffFile(dng) as file: pixels=file.asarray()
                metrics=quality(reference,pixels,meta); del pixels
            attempts.append(dict(encode_process_s=encoded['encode_process_s'],
                                 encode_workflow_s=encoded['elapsed_before_publication_s'],
                                 decode_process_s=decoded['decode_process_s'],
                                 decode_workflow_s=decoded['elapsed_before_publication_s'],
                                 gpr_sha256=encoded['output_sha256'],decoded_raw_sha256=decoded['raw_sha256']))
        row=dict(quality=q,payload_bytes=encoded['payload_bytes'],gpr_bytes=encoded['output_bytes'],
                 bits_per_pixel=encoded['output_bytes']*8/(w*h),raw_word_compression_ratio=w*h*2/encoded['output_bytes'],
                 encode_process_median_s=statistics.median(a['encode_process_s'] for a in attempts),
                 encode_workflow_median_s=statistics.median(a['encode_workflow_s'] for a in attempts),
                 payload_sha256=payload_sha,metrics=metrics,attempts=attempts)
        rows.append(row)
        print(json.dumps(dict(quality=q,gpr_bytes=row['gpr_bytes'],encode_process_ms=1000*row['encode_process_median_s'],
                              active_psnr_db=metrics['active']['psnr_db'])),flush=True)
    repo=Path(__file__).resolve().parents[1]
    hashes={path.relative_to(repo).as_posix():stock_still.digest(path)
            for folder in ('source','tests','tools','cmake') for path in sorted((repo/folder).rglob('*'))
            if path.is_file() and path.suffix in ('.c','.cpp','.h','.hpp','.S','.py','.cmake','.txt') and '__pycache__' not in path.parts}
    hashes['CMakeLists.txt']=stock_still.digest(repo/'CMakeLists.txt')
    (root/'source-files.json').write_text(json.dumps(hashes,indent=2)+'\n')
    result=dict(schema='gpr.stock_still_benchmark.v1',platform=platform.platform(),reference_kind=args.reference_kind,
                independent_source_decode_verified=bool(independent),width=w,height=h,sample_bits=args.sample_bits,cfa=meta['cfa'],
                source_sha256=stock_still.digest(source),raw_sha256=stock_still.digest(raw),sdk_sha256=stock_still.digest(sdk),
                source_files_sha256=hashlib.sha256(json.dumps(hashes,sort_keys=True).encode()).hexdigest(),
                metadata_sha256=hashlib.sha256(stock_still.canonical(meta)).hexdigest(),repeats=args.repeats,cnn=False,
                reference_decode_process_s=input_decode_s,rows=rows,
                timing_scope='fresh SDK processes with input/file I/O, warm file cache, no JPEG preview; workflow also validates/hashes and preserves typed calibration; no final fsync')
    (root/'benchmark.json').write_text(json.dumps(result,indent=2)+'\n')


if __name__=='__main__': main()
