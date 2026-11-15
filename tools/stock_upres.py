#!/usr/bin/env python3
"""Explicit stock UPRESABLE timelapse/regression workflow, with no model approval.

Simulates half-resolution capture from declared reference DNG/GPR inputs, runs
a caller-selected 2x Bayer CNN, then re-encodes full-resolution stock GPRs.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import tempfile

import raw_media as media
import stock_cnn_metadata as geometry
import stock_sequence
import stock_still as still
import stock_video as video


def page_with_bayer(pages):
    for page in pages:
        if 33422 in page.tags:return page
        if page.pages:
            found=page_with_bayer(page.pages)
            if found is not None:return found


def metrics(reference, actual, meta):
    import numpy as np
    ref=reference.astype(np.int32);error=actual.astype(np.int32)-ref
    tags={t['code']:t for t in meta['image']}
    t,l,b,r=map(int,media.values(tags[50829])) if 50829 in tags else (0,0,*reference.shape)
    ref=ref[t:b,l:r];error=error[t:b,l:r]
    if min(error.shape)<3:raise ValueError('quality evaluation requires an active area at least 3x3')
    def measure(values):
        mse=float(np.mean(np.square(values,dtype=np.float64)))
        return dict(rmse_counts=mse**.5,psnr_db=None if mse==0 else float(20*np.log10(16383)-10*np.log10(mse)))
    low,high=np.percentile(ref,(10,90))
    gradient=np.abs(ref[2:,:-2]-ref[:-2,:-2])+np.abs(ref[:-2,2:]-ref[:-2,:-2])
    return dict(active=measure(error),shadow=measure(error[ref<=low]),highlight=measure(error[ref>=high]),
                detail=measure(error[:-2,:-2][gradient>=np.percentile(gradient,90)]),
                sites=[measure(error[(site//2-t)%2::2,(site%2-l)%2::2]) for site in range(4)])


def execute(mode, inputs, references, reference_kind, checkpoint, sdk, output_dir,
            capture_quality, output_quality, bridge=None, device='cpu', tile=256, halo=64,
            cnn_root=None, fps_num=24, fps_den=1, dng_export=False, prores=None,
            input_mode='simulated',coreml_package=None,coreml_units='all',metal=None,native=None):
    if native is not None:
        if (mode!='timelapse' or input_mode!='captured_gpr' or references or reference_kind!='camera_gpr_replay'
                or coreml_package is not None or metal is not None or cnn_root is not None or device!='cpu'):
            raise ValueError('standalone native upres requires captured-GPR timelapse with no Python backend')
        import stock_native
        return stock_native.timelapse(inputs,checkpoint,sdk,output_dir,output_quality,bridge,native,
            tile,halo,fps_num,fps_den,dng_export,prores)
    import numpy as np
    import tifffile
    import torch
    if mode not in ('timelapse','regression','both'):raise ValueError('invalid workflow mode')
    if input_mode not in ('simulated','captured_gpr'):raise ValueError('invalid input mode')
    if input_mode=='captured_gpr' and (mode=='regression' or reference_kind!='camera_gpr_replay'):
        raise ValueError('captured GPR mode requires timelapse and camera_gpr_replay references')
    if reference_kind not in ('lossless_sensor_dng','camera_gpr_replay'):raise ValueError('declare reference kind')
    if mode in ('timelapse','both') and (not inputs or bridge is None):raise ValueError('timelapse requires ordered inputs and MOV bridge')
    if mode in ('regression','both') and not references:raise ValueError('regression requires references')
    if mode=='regression' and inputs or mode=='timelapse' and references:raise ValueError('unused source list for selected mode')
    if prores is not None:
        if mode=='regression':raise ValueError('ProRes export requires timelapse')
        if set(prores)!={'decoder','renderer','encoder','probe'}:raise ValueError('ProRes requires decoder, renderer, encoder and probe')
        if any(not Path(path).is_file() for path in prores.values()):raise ValueError('missing ProRes executable')
    if not Path(sdk).is_file() or bridge is not None and not Path(bridge).is_file():raise ValueError('missing SDK or MOV bridge')
    if (any(type(v) is not int for v in (fps_num,fps_den)) or not 0<fps_num<=240000
            or not 0<fps_den<=10000 or fps_num>240*fps_den):raise ValueError('invalid nominal rate')
    needs_simulation=input_mode=='simulated' or mode in ('regression','both')
    if (type(output_quality) is not int or not 0<=output_quality<=5 or
            needs_simulation and (type(capture_quality) is not int or not 0<=capture_quality<=5)):
        raise ValueError('explicit stock presets 0..5 required for every new encode')
    if tile<8 or tile%8 or halo<0 or halo%8:raise ValueError('invalid inference tile/halo')
    device=torch.device(device);model,model_info=video.load_cnn(cnn_root,checkpoint,device,coreml_package,coreml_units,metal)
    if model_info['output_scale']!=2:raise ValueError('workflow requires a 2x checkpoint')
    class Bicubic(torch.nn.Module):
        def forward(self,x):return torch.nn.functional.interpolate(x,scale_factor=2,mode='bicubic',align_corners=False).clamp(0,1)
    tasks=[('timelapse',Path(p).resolve()) for p in inputs]+[('regression',Path(p).resolve()) for p in references]
    for group,path in tasks:
        meta=media.extract(path,14)
        if meta['cfa']!='RGGB' or min(meta['width'],meta['height'])<96:raise ValueError('workflow requires RGGB14 sources at least 96x96')
        direct=group=='timelapse' and input_mode=='captured_gpr'
        low_meta=meta if direct else geometry.simulated_half(meta)[0]
        full_meta,_=geometry.scaled(low_meta,2)
        if direct:media.extract(path,14,with_payload=True)
        if group=='timelapse' and (full_meta['width']>8192 or full_meta['height']>6144 or full_meta['width']%8 or full_meta['height']%8):
            raise ValueError('timelapse exceeds GVID geometry; no implicit crop')
        if group=='timelapse':stock_sequence.metadata_bytes(dict(raw_metadata=full_meta,source_gpr_sha256='0'*64))
    root=Path(output_dir).resolve();root.mkdir(parents=True,exist_ok=False)
    report=dict(schema='gpr.stock_upres.v1',complete=False,mode=mode,model=model_info,
                reference_kind=reference_kind,input_mode=input_mode,capture_simulated=input_mode=='simulated',quality_approval=False,
                capture_quality=capture_quality if input_mode=='simulated' or mode=='both' else None,
                output_quality=output_quality,frames=[])
    def save():
        staged=root/'.run.json.tmp';staged.write_text(json.dumps(report,indent=2,allow_nan=False)+'\n');os.replace(staged,root/'run.json')
    save();full_sequence=[]
    try:
        for index,(group,source) in enumerate(tasks):
            frame_dir=root/f'{index:06d}-{group}';frame_dir.mkdir()
            source_sha=still.digest(source);meta=media.extract(source,14);w,h=meta['width'],meta['height']
            direct=group=='timelapse' and input_mode=='captured_gpr'
            low_meta,omitted=(meta,[]) if direct else geometry.simulated_half(meta)
            full_meta,accounting=geometry.scaled(low_meta,2)
            with tempfile.TemporaryDirectory(prefix='.frame-',dir=frame_dir) as temp:
                work=Path(temp);raw=work/'source.raw'
                still.run(sdk,['-i',source,'-o',raw],os.environ.copy())
                reference=np.fromfile(raw,dtype='<u2').reshape(h,w)
                if np.any(reference>16383):raise ValueError('reference exceeds 14-bit domain')
                with tifffile.TiffFile(source) as file:
                    page=page_with_bayer(file.pages)
                    if page is None:raise ValueError('missing Bayer source page')
                    if reference_kind=='lossless_sensor_dng':
                        if int(page.compression) not in (1,7,8,32946) or not np.array_equal(page.asarray(),reference):
                            raise ValueError('lossless reference and independent source decode disagree')
                    elif int(page.compression)!=9:raise ValueError('camera replay requires VC-5 GPR input')
                low_gpr=frame_dir/'capture.gpr'
                if direct:
                    _,payload=media.extract(source,14,with_payload=True)
                    media.write(low_gpr,low_meta,payload,'gpr')
                else:
                    planes=np.stack([reference[y::2,x::2] for y,x in ((0,0),(0,1),(1,0),(1,1))]).astype(np.uint32)
                    small=((planes[:,::2,::2]+planes[:,::2,1::2]+planes[:,1::2,::2]+planes[:,1::2,1::2]+2)//4).astype('<u2')
                    low=np.empty((h//2,w//2),dtype='<u2')
                    for site in range(4):low[site//2::2,site%2::2]=small[site]
                    low.tofile(work/'low.raw');del planes,small,low
                    still.encode(work/'low.raw',low_meta,capture_quality,sdk,low_gpr)
                still.decode(low_gpr,14,sdk,work/'low.dng')
                with tifffile.TiffFile(work/'low.dng') as file:file.asarray().astype('<u2').tofile(work/'decoded.raw')
                restored,cnn_s=video.restore(work/'decoded.raw',low_meta['width'],low_meta['height'],model,device,tile,halo,scale=2)
                restored.tofile(work/'restored.raw');full_gpr=frame_dir/'full.gpr'
                encoded=still.encode(work/'restored.raw',full_meta,output_quality,sdk,full_gpr)
                editable=frame_dir/'decoded-full.dng' if dng_export else work/'full.dng'
                still.decode(full_gpr,14,sdk,editable)
                with tifffile.TiffFile(editable) as file:decoded=file.asarray()
                row=dict(group=group,source=str(source),source_sha256=source_sha,full_gpr=str(full_gpr.relative_to(root)),
                         capture_simulated=not direct,capture_quality=None if direct else capture_quality,
                         capture_payload_sha256=hashlib.sha256(media.extract(low_gpr,14,with_payload=True)[1]).hexdigest(),
                         full_gpr_sha256=encoded['output_sha256'],cnn_s=cnn_s,omitted_metadata=omitted,geometry=accounting,
                         model_raw_sha256=hashlib.sha256(restored.astype('<u2').tobytes()).hexdigest(),
                         final_decoded_raw_sha256=hashlib.sha256(decoded.astype('<u2').tobytes()).hexdigest(),
                         reencode_loss=metrics(restored,decoded,full_meta))
                if dng_export:media.write(frame_dir/'model-output.dng',full_meta,restored.astype('<u2').tobytes())
                if group=='regression':
                    baseline,_=video.restore(work/'decoded.raw',w//2,h//2,Bicubic(),device,tile,halo,scale=2)
                    row.update(bicubic=metrics(reference,baseline,meta),model_output=metrics(reference,restored,meta),
                               final_output=metrics(reference,decoded,meta),quality_approval=False)
                if still.digest(source)!=source_sha:raise ValueError('source changed during workflow')
                if group=='timelapse':full_sequence.append(full_gpr)
                report['frames'].append(row);save()
        if full_sequence:
            import stock_mov
            report['gvid']=stock_sequence.pack(full_sequence,root/'timelapse.gvid',output_quality,fps_num=fps_num,fps_den=fps_den)
            report['mov']=stock_mov.convert('wrap',root/'timelapse.gvid',bridge,root/'timelapse.mov')
            if prores is not None:
                import stock_prores
                report['prores']=stock_prores.convert(root/'timelapse.gvid',root/'timelapse-prores.mov',**prores)
        report['complete']=True;save()
    except BaseException as error:
        report['error']=str(error);save();raise
    return report


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--mode',choices=('timelapse','regression','both'),required=True)
    from stock_coreml import add_arguments
    add_arguments(p)
    import stock_metal
    stock_metal.add_arguments(p)
    import stock_native
    stock_native.add_arguments(p)
    p.add_argument('--input',dest='inputs',action='append',type=Path,default=[])
    p.add_argument('--input-mode',choices=('simulated','captured_gpr'),default='simulated',
                   help='captured_gpr: feed existing timelapse GPR captures directly to the 2x model; no simulated capture or capture-preset claim')
    p.add_argument('--reference',dest='references',action='append',type=Path,default=[])
    p.add_argument('--reference-kind',choices=('lossless_sensor_dng','camera_gpr_replay'),required=True)
    for name in ('checkpoint','sdk','output-dir'):p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--capture-quality',type=int,choices=range(6),help='required for simulated capture; unused for existing captured GPRs')
    p.add_argument('--output-quality',type=int,choices=range(6),required=True)
    p.add_argument('--bridge',type=Path);p.add_argument('--device',default='cpu');p.add_argument('--cnn-root',type=Path)
    p.add_argument('--tile',type=int,default=256);p.add_argument('--halo',type=int,default=64)
    p.add_argument('--fps-num',type=int,default=24);p.add_argument('--fps-den',type=int,default=1)
    p.add_argument('--dng-export',action='store_true')
    p.add_argument('--prores',action='store_true',help='also render calibrated deep ProRes using the four explicit executable paths')
    for name in ('decoder','renderer','encoder','probe'):p.add_argument('--prores-'+name,type=Path)
    args=vars(p.parse_args());enabled=args.pop('prores')
    args['metal']=stock_metal.configuration(args)
    for name in stock_metal.ARGUMENTS:args.pop(name)
    args['native']=stock_native.configuration(args)
    for name in stock_native.ARGUMENTS:args.pop(name)
    binaries={name:args.pop('prores_'+name) for name in ('decoder','renderer','encoder','probe')}
    if enabled and not all(binaries.values()) or not enabled and any(binaries.values()):p.error('ProRes requires --prores and all four executable paths')
    print(json.dumps(execute(**args,prores=binaries if enabled else None),indent=2))


if __name__=='__main__':main()
