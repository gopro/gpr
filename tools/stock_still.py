#!/usr/bin/env python3
"""Stock still compression with explicit presets and typed source calibration.

Production dependencies are Python's standard library and gpr_tools. The
encoder remains stock VC-5. Optional restore requires external Torch/model
dependencies and explicit weights; it makes no automatic quality approval.
"""
import argparse
import copy
import errno
import hashlib
import json
import os
from pathlib import Path
import subprocess
import shutil
import sys
import tempfile
import time
import raw_media
from stock_video import decode_metadata

FAST_FLAGS=('GPR_STREAM_UNPACK','GPR_BAND_STREAM','GPR_COMPACT_BANDS','GPR_DIRECT_BANDS')


def digest(path):
    h=hashlib.sha256()
    with Path(path).open('rb') as f:
        for block in iter(lambda:f.read(1024*1024),b''): h.update(block)
    return h.hexdigest()


def canonical(meta):
    selected={k:meta[k] for k in ('schema','width','height','sample_bits','cfa')}
    for group in ('image','exif','gps'):
        selected[group]=[dict(code=t['code'],type=t['type'],count=t['count'],bytes=raw_media.unpack_record(t).hex())
                         for t in sorted(meta[group],key=lambda t:t['code'])]
    return json.dumps(selected,sort_keys=True,separators=(',',':')).encode()


def run(sdk, args, environment):
    start=time.perf_counter()
    result=subprocess.run([str(Path(sdk).resolve()),*map(str,args)],capture_output=True,text=True,env=environment)
    if result.returncode:
        raise RuntimeError(f'gpr_tools failed ({result.returncode}): {result.stderr[-8000:]}')
    return time.perf_counter()-start


def publish(files):
    # Prefer an atomic, exclusive link. exFAT and cross-filesystem destinations
    # need an exclusive copy instead. Roll back only names created by us.
    created=[]
    try:
        for source,destination in files:
            try:
                os.link(source,destination)
            except OSError as error:
                if error.errno not in (errno.EXDEV,errno.EPERM,errno.EACCES,errno.ENOTSUP,errno.EOPNOTSUPP): raise
                with Path(destination).open('xb') as output:
                    created.append(destination)
                    with Path(source).open('rb') as staged: shutil.copyfileobj(staged,output,1024*1024)
            else: created.append(destination)
    except BaseException:
        for path in reversed(created): path.unlink()
        raise


def outputs(output, receipt, editable):
    output=Path(os.path.abspath(output))
    receipt=Path(os.path.abspath(receipt)) if receipt else output.with_suffix(output.suffix+'.json')
    editable=Path(os.path.abspath(editable)) if editable else None
    paths=[output,receipt]+([editable] if editable else [])
    if len(set(paths))!=len(paths): raise ValueError('output names must differ')
    for path in paths:
        if path.exists() or path.is_symlink(): raise FileExistsError(path)
    for path in paths: path.parent.mkdir(parents=True,exist_ok=True)
    return output,receipt,editable


def carrier(meta):
    # The legacy SDK infers 12/14/16 count domains from full-scale WhiteLevel.
    # Use that only in a private decoding carrier; output calibration is exact.
    result=copy.deepcopy(meta)
    import struct
    result['image']=[t for t in result['image'] if t['code']!=50717]
    result['image'].append(raw_media.record(50717,4,1,struct.pack('<I',(1<<meta['sample_bits'])-1)))
    return result


def encode(source, metadata, quality, sdk, output, receipt=None, editable=None, source_type='raw'):
    start=time.perf_counter()
    if sys.byteorder!='little': raise ValueError('still frontend requires a little-endian host')
    if type(quality) is not int or not 0<=quality<=5: raise ValueError('stock preset must be 0..5')
    raw_media.validate(metadata)
    w,h,bits=metadata['width'],metadata['height'],metadata['sample_bits']
    if w<48 or h<48: raise ValueError('still frontend requires Bayer dimensions at least 48')
    if source_type not in ('raw','dng'): raise ValueError('requires raw or dng source')
    source=Path(source).resolve(); sdk=Path(sdk).resolve()
    output,receipt,editable=outputs(output,receipt,editable)
    environment=os.environ.copy()
    environment.update({flag:'1' for flag in FAST_FLAGS})
    with tempfile.TemporaryDirectory(prefix='.stock-still-',dir=output.parent) as temp:
        root=Path(temp); raw=source; input_decode_s=0.
        if source_type=='dng':
            actual=raw_media.extract(source,bits)
            if tuple(actual[k] for k in ('width','height','cfa'))!=(w,h,metadata['cfa']):
                raise ValueError('source DNG Bayer geometry/phase differs from calibration')
            raw=root/'source.raw'
            input_decode_s=run(sdk,['-i',source,'-o',raw],environment)
        data=raw.read_bytes()
        if len(data)!=w*h*2 or not raw_media.samples_in_domain(data,bits):
            raise ValueError('requires tightly packed little-endian words in the declared count domain')
        raw_sha=hashlib.sha256(data).hexdigest(); del data
        encoded=root/'encoded.gpr'
        native_s=run(sdk,['-i',raw,'-o',encoded,'-w',w,'-h',h,'-x',metadata['cfa'].lower()+str(bits),
                          '-q',quality,'--no-preview'],environment)
        generated,payload=raw_media.extract(encoded,bits,with_payload=True)
        if (generated['width'],generated['height'],generated['cfa'])!=(w,h,metadata['cfa']):
            raise ValueError('encoder returned a different Bayer image')
        packaged=root/'final.gpr'; raw_media.write(packaged,metadata,payload,'gpr')
        files=[(packaged,output)]; decode_s=None
        if editable:
            decoded=root/'decoded.raw'
            decode_s=run(sdk,['-i',encoded,'-o',decoded],environment)
            dng=root/'editable.dng'; raw_media.write(dng,metadata,decoded.read_bytes())
            files.append((dng,editable))
        result=dict(schema='gpr.stock_still.v1',action='encode',quality=quality,cnn=False,
                    width=w,height=h,sample_bits=bits,cfa=metadata['cfa'],source_type=source_type,
                    source_sha256=raw_sha if source_type=='raw' else actual['source_sha256'],raw_sha256=raw_sha,
                    metadata_sha256=hashlib.sha256(canonical(metadata)).hexdigest(),
                    payload_sha256=hashlib.sha256(payload).hexdigest(),payload_bytes=len(payload),
                    output_sha256=digest(packaged),output_bytes=packaged.stat().st_size,
                    sdk_sha256=digest(sdk),encoder_environment={flag:environment[flag] for flag in FAST_FLAGS},
                    input_decode_process_s=input_decode_s,encode_process_s=native_s,
                    decode_process_s=decode_s,elapsed_before_publication_s=time.perf_counter()-start)
        if editable: result['editable_sha256']=digest(root/'editable.dng')
        log=root/'receipt.json'; log.write_text(json.dumps(result,indent=2)+'\n'); files.append((log,receipt))
        publish(files)
        return result


def decode(source, bits, sdk, output, receipt=None, reconstruction=None, gpr_output=None, gpr_quality=None):
    if sys.byteorder!='little': raise ValueError('still frontend requires a little-endian host')
    if gpr_output is not None:
        if reconstruction is None: raise ValueError('additional GPR output requires reconstruction')
        if type(gpr_quality) is not int or not 0<=gpr_quality<=5:
            raise ValueError('additional GPR output requires an explicit stock preset 0..5')
    elif gpr_quality is not None: raise ValueError('GPR preset requires an additional GPR output')
    start=time.perf_counter(); source=Path(source).resolve(); sdk=Path(sdk).resolve()
    meta,payload=raw_media.extract(source,bits,with_payload=True)
    if meta['width']<48 or meta['height']<48: raise ValueError('still frontend requires Bayer dimensions at least 48')
    output,receipt,gpr_output=outputs(output,receipt,gpr_output)
    model=model_info=None; output_meta=meta; output_scale=1; geometry=None;native=None;native_result=None
    if reconstruction is not None:
        if (meta['cfa'],bits)!=('RGGB',14): raise ValueError('current CNN checkpoint adapter requires RGGB14')
        tile,halo=reconstruction['tile'],reconstruction['halo']
        if type(tile) is not int or type(halo) is not int or tile<8 or tile%8 or halo<0 or halo%8:
            raise ValueError('CNN tile/halo must be multiples of eight; tile must be positive')
        from stock_cnn_metadata import scaled
        output_scale=reconstruction.get('scale',1)
        output_meta,geometry=scaled(meta,output_scale)
        native=reconstruction.get('native')
        if native is not None:
            if reconstruction.get('root') or reconstruction.get('coreml_package') or reconstruction.get('metal'):
                raise ValueError('select standalone native or a Python reconstruction backend')
        else:
            import torch
            from stock_video import load_cnn, restore
            device=torch.device(reconstruction['device'])
            model,model_info=load_cnn(reconstruction.get('root'),Path(reconstruction['checkpoint']),device,
                                     reconstruction.get('coreml_package'),reconstruction.get('coreml_units','all'),reconstruction.get('metal'))
            if model_info.get('output_scale',1)!=output_scale: raise ValueError('checkpoint output scale differs from requested reconstruction scale')
    with tempfile.TemporaryDirectory(prefix='.stock-still-',dir=output.parent) as temp:
        root=Path(temp); private=root/'carrier.gpr'
        raw_media.write(private,carrier(meta),payload,'gpr')
        raw=root/'decoded.raw'; decode_s=run(sdk,['-i',private,'-o',raw],os.environ.copy())
        data=raw.read_bytes(); raw_sha=hashlib.sha256(data).hexdigest()
        if model is not None:
            restored,cnn_s=restore(raw,meta['width'],meta['height'],model,device,tile,halo,scale=output_scale)
            data=restored.astype('<u2').tobytes()
        elif native is not None:
            from stock_native import restore as native_restore
            data,native_result=native_restore(raw,meta['width'],meta['height'],output_scale,reconstruction['checkpoint'],
                                            native,root/'native.raw',tile,halo)
            model_info=native_result['model'];cnn_s=native_result['restore_s']
            if not raw_media.samples_in_domain(data,bits):raise ValueError('native output exceeds declared count domain')
        restored_sha=hashlib.sha256(data).hexdigest() if reconstruction is not None else None
        dng=root/'editable.dng'; raw_media.write(dng,output_meta,data)
        if gpr_output is not None:
            reconstructed=root/'restored.raw'; reconstructed.write_bytes(data)
        del data
        result=dict(schema='gpr.stock_still.v1',action='restore' if reconstruction is not None else 'decode',cnn=reconstruction is not None,
                    width=output_meta['width'],height=output_meta['height'],sample_bits=bits,cfa=meta['cfa'],
                    source_sha256=meta['source_sha256'],payload_sha256=hashlib.sha256(payload).hexdigest(),
                    metadata_sha256=hashlib.sha256(canonical(output_meta)).hexdigest(),
                    raw_sha256=raw_sha,output_sha256=digest(dng),sdk_sha256=digest(sdk),
                    decode_process_s=decode_s,elapsed_before_publication_s=time.perf_counter()-start)
        if reconstruction is not None:
            result.update(model=model_info,cnn_s=cnn_s,restored_raw_sha256=restored_sha,
                          source_width=meta['width'],source_height=meta['height'],
                          source_metadata_sha256=hashlib.sha256(canonical(meta)).hexdigest(),geometry=geometry,
                          tile_plane_pixels=tile,halo_plane_pixels=halo,runtime_raw_norm=16383,output_scale=output_scale,
                          quality_approval=False)
        if native_result is not None:result['native_reconstruction']=native_result
        files=[(dng,output)]
        if gpr_output is not None:
            packaged=root/'restored.gpr'
            encoded=encode(reconstructed,output_meta,gpr_quality,sdk,packaged)
            packaged_meta,packaged_payload=raw_media.extract(packaged,bits,with_payload=True)
            if canonical(packaged_meta)!=canonical(output_meta):
                raise ValueError('reconstructed GPR calibration differs')
            encoded_carrier=root/'reencoded-carrier.gpr'
            raw_media.write(encoded_carrier,carrier(output_meta),packaged_payload,'gpr')
            decoded=root/'reencoded.raw'
            verify_s=run(sdk,['-i',encoded_carrier,'-o',decoded],os.environ.copy())
            if decoded.stat().st_size!=output_meta['width']*output_meta['height']*2:
                raise ValueError('reconstructed GPR decoded size differs')
            decoded_sha=digest(decoded)
            result['reencoded_gpr']=dict(encode=encoded,decoded_raw_sha256=decoded_sha,
                decoded_matches_restored=decoded_sha==result['restored_raw_sha256'],
                verification_decode_process_s=verify_s,reencoded=True,lossy=True,quality_approval=False)
            files.append((packaged,gpr_output))
        result['elapsed_before_publication_s']=time.perf_counter()-start
        log=root/'receipt.json'; log.write_text(json.dumps(result,indent=2)+'\n')
        publish(files+[(log,receipt)])
        return result


def main():
    p=argparse.ArgumentParser(description=__doc__); sub=p.add_subparsers(dest='action',required=True)
    enc=sub.add_parser('encode'); enc.add_argument('input',type=Path)
    enc.add_argument('--metadata',type=Path); enc.add_argument('--sample-bits',type=int,choices=(12,14,16))
    enc.add_argument('--source-type',choices=('raw','dng'),default='raw')
    enc.add_argument('--quality',type=int,choices=range(6),required=True); enc.add_argument('--editable',type=Path)
    dec=sub.add_parser('decode'); dec.add_argument('input',type=Path)
    dec.add_argument('--sample-bits',type=int,choices=(12,14,16),required=True)
    rec=sub.add_parser('restore',help='optional experimental 1x/2x CNN to editable DNG and optional re-encoded GPR')
    rec.add_argument('input',type=Path); rec.add_argument('--sample-bits',type=int,choices=(14,),required=True)
    rec.add_argument('--cnn-root',type=Path,help='explicit external architecture override; default: bundled BIBO')
    rec.add_argument('--checkpoint',type=Path,required=True)
    rec.add_argument('--scale',type=int,choices=(1,2),default=1)
    rec.add_argument('--device',choices=('cpu','mps','cuda'),default='cpu')
    from stock_coreml import add_arguments
    add_arguments(rec)
    import stock_metal
    stock_metal.add_arguments(rec)
    import stock_native
    stock_native.add_arguments(rec)
    rec.add_argument('--tile',type=int,default=256); rec.add_argument('--halo',type=int,default=64)
    rec.add_argument('--gpr-output',type=Path,help='also save reconstructed counts as a newly compressed GPR')
    rec.add_argument('--gpr-quality',type=int,choices=range(6),help='required with --gpr-output; lossy stock preset')
    for cmd in (enc,dec,rec):
        cmd.add_argument('--sdk',type=Path,required=True); cmd.add_argument('--output',type=Path,required=True)
        cmd.add_argument('--receipt',type=Path)
    args=p.parse_args()
    if args.action=='encode':
        if args.metadata: meta=decode_metadata(args.metadata.read_bytes())
        elif args.source_type=='dng' and args.sample_bits: meta=raw_media.extract(args.input,args.sample_bits)
        else: p.error('RAW requires --metadata; DNG requires --metadata or --sample-bits')
        if args.sample_bits and meta['sample_bits']!=args.sample_bits: p.error('metadata count domain differs')
        result=encode(args.input,meta,args.quality,args.sdk,args.output,args.receipt,args.editable,args.source_type)
    else:
        settings=dict(root=args.cnn_root,checkpoint=args.checkpoint,device=args.device,tile=args.tile,halo=args.halo,scale=args.scale,
                      coreml_package=args.coreml_package,coreml_units=args.coreml_units,
                      metal=stock_metal.configuration(args),native=stock_native.configuration(args)) if args.action=='restore' else None
        result=decode(args.input,args.sample_bits,args.sdk,args.output,args.receipt,settings,
                      getattr(args,'gpr_output',None),getattr(args,'gpr_quality',None))
    print(json.dumps(result,indent=2))


if __name__=='__main__': main()
