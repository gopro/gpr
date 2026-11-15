"""Standard-library adapter for standalone native Bayer reconstruction."""
import hashlib
import json
from pathlib import Path
import subprocess
import os
import tempfile


def inspect(checkpoint,configuration,scale):
    path=Path(configuration['artifact'])/'contract.json';data=path.read_bytes();contract=json.loads(data)
    backend=configuration.get('backend','mpsgraph')
    schema='gpr.stock_coreml.v1' if backend=='coreml' else 'gpr.stock_metal.v1'
    expected=hashlib.sha256(Path(checkpoint).read_bytes()).hexdigest();binding=hashlib.sha256(data).hexdigest()
    if (contract.get('schema')!=schema or contract.get('checkpoint_sha256')!=expected
            or contract.get('output_scale')!=scale or contract.get('runtime_raw_norm')!=16383
            or contract.get('input_cfa')!='RGGB' or contract.get('quality_approval') is not False
            or configuration.get('contract_sha256',binding)!=binding):
        raise ValueError('standalone artifact differs from checkpoint/scale or prior binding')
    return dict(runtime_backend=backend,host_runtime='native',checkpoint_sha256=expected,
        architecture_sha256=contract['architecture_sha256'],artifact_contract_sha256=binding,
        output_scale=scale,input_cfa='RGGB',runtime_raw_norm=16383,quality_status='unvalidated_for_stock_video')


def restore(source,width,height,scale,checkpoint,configuration,output,tile,halo):
    expected=inspect(checkpoint,configuration,scale)
    executable=Path(configuration['executable']).resolve()
    artifact=Path(configuration['artifact']).resolve()
    backend=configuration.get('backend','mpsgraph')
    command=[executable,'--input',Path(source).resolve(),'--output',Path(output).resolve(),
        '--artifact',artifact,'--checkpoint',Path(checkpoint).resolve(),'--backend',backend,
        '--width',width,'--height',height,'--scale',scale,'--tile',tile,'--halo',halo,
        '--threads',configuration.get('threads',4)]
    if backend=='coreml':command+=['--units',configuration.get('units','all')]
    else:
        if not configuration.get('kernels'):raise ValueError('native Metal reconstruction requires a metallib')
        command+=['--kernels',Path(configuration['kernels']).resolve()]
    process=subprocess.run(list(map(str,command)),capture_output=True,text=True)
    if process.returncode:raise RuntimeError('native reconstruction failed: '+process.stderr[-8000:])
    result=json.loads(process.stdout)
    raw=Path(output).read_bytes()
    if (result.get('schema')!='gpr.native_reconstruct.v1' or result.get('complete') is not True
            or result.get('quality_approval') is not False or result.get('torch_required') is not False
            or result.get('model',{}).get('artifact_contract_sha256')!=expected['artifact_contract_sha256']
            or result.get('input_sha256')!=hashlib.sha256(Path(source).read_bytes()).hexdigest()
            or (result.get('width'),result.get('height'),result.get('output_scale'))!=(width*scale,height*scale,scale)
            or len(raw)!=width*height*scale*scale*2
            or hashlib.sha256(raw).hexdigest()!=result.get('output_sha256')):
        raise ValueError('native output differs from its count/geometry receipt')
    return raw,result


def timelapse(inputs,checkpoint,sdk,output_dir,output_quality,bridge,native,tile=256,halo=64,
              fps_num=24,fps_den=1,dng_export=False,prores=None):
    """Captured GPR -> native 2x -> stock GPR/GVID/MOV, without ML Python deps."""
    import raw_media as media
    import stock_still as still
    import stock_cnn_metadata as geometry
    import stock_sequence
    import stock_mov
    if not inputs or bridge is None:raise ValueError('native timelapse requires captures and MOV bridge')
    if type(output_quality) is not int or not 0<=output_quality<=5:raise ValueError('explicit stock preset required')
    if not 0<fps_num<=240000 or not 0<fps_den<=10000 or fps_num>240*fps_den:raise ValueError('invalid nominal frame rate')
    model=inspect(checkpoint,native,2);native=dict(native,contract_sha256=model['artifact_contract_sha256'])
    for source in inputs:
        meta,_=media.extract(Path(source),14,with_payload=True);full,_=geometry.scaled(meta,2)
        if (meta['cfa']!='RGGB' or min(meta['width'],meta['height'])<48 or full['width']>8192
                or full['height']>6144 or full['width']%8 or full['height']%8):
            raise ValueError('native timelapse requires RGGB14 captures within stock GVID output geometry')
        stock_sequence.metadata_bytes(dict(raw_metadata=full,source_gpr_sha256='0'*64))
    if prores is not None and (set(prores)!={'decoder','renderer','encoder','probe'} or
        any(not Path(p).is_file() for p in prores.values())):raise ValueError('requires four ProRes executable paths')
    root=Path(output_dir).resolve();root.mkdir(parents=True,exist_ok=False)
    report=dict(schema='gpr.stock_upres.v1',mode='timelapse',input_mode='captured_gpr',complete=False,
        capture_simulated=False,capture_quality=None,output_quality=output_quality,quality_approval=False,
        runtime='standalone_native',reference_kind='camera_gpr_replay',model=model,frames=[],numeric_regression_evaluated=False)
    def save():
        path=root/'.run.json.tmp';path.write_text(json.dumps(report,indent=2)+'\n');os.replace(path,root/'run.json')
    save();sequence=[]
    try:
        for index,source in enumerate(inputs):
            source=Path(source).resolve();source_hash=still.digest(source)
            frame=root/f'{index:06d}-timelapse';frame.mkdir()
            meta,payload=media.extract(source,14,with_payload=True);media.write(frame/'capture.gpr',meta,payload,'gpr')
            full=frame/'full.gpr'
            with tempfile.TemporaryDirectory(prefix='.native-',dir=frame) as temp:
                model_dng=frame/'model-output.dng' if dng_export else Path(temp)/'model.dng'
                restored=still.decode(source,14,sdk,model_dng,reconstruction=dict(checkpoint=checkpoint,
                    scale=2,device='cpu',tile=tile,halo=halo,native=native),gpr_output=full,gpr_quality=output_quality)
                encoded=restored['reencoded_gpr']['encode']
                if restored['source_sha256']!=source_hash or still.digest(full)!=encoded['output_sha256']:
                    raise ValueError('native source/output identity differs')
                with full.with_suffix('.gpr.json').open('x') as file:json.dump(encoded,file,indent=2)
                (frame/'reconstruction.json').write_text(json.dumps(restored,indent=2)+'\n')
                if dng_export:
                    decoded=still.decode(full,14,sdk,frame/'decoded-full.dng')
                    if decoded['raw_sha256']!=restored['reencoded_gpr']['decoded_raw_sha256']:
                        raise ValueError('final editable DNG differs from verified GPR decode')
                if still.digest(source)!=source_hash:raise ValueError('source changed during native workflow')
                report['model']=restored['model']
                report['frames'].append(dict(group='timelapse',source=str(source),source_sha256=source_hash,
                    capture_simulated=False,capture_quality=None,full_gpr=str(full.relative_to(root)),
                    full_gpr_sha256=encoded['output_sha256'],model_raw_sha256=restored['restored_raw_sha256'],
                    geometry=restored['geometry'],omitted_metadata=restored['geometry'].get('omitted',[]),
                    final_decoded_raw_sha256=restored['reencoded_gpr']['decoded_raw_sha256'],
                    cnn_s=restored['cnn_s'],native_reconstruction=restored['native_reconstruction'],
                    reencode_loss=dict(evaluated=False,reason='numeric quality metrics require a separate regression evaluation')))
                sequence.append(full);save()
        report['gvid']=stock_sequence.pack(sequence,root/'timelapse.gvid',output_quality,fps_num=fps_num,fps_den=fps_den)
        report['mov']=stock_mov.convert('wrap',root/'timelapse.gvid',bridge,root/'timelapse.mov')
        if prores is not None:
            import stock_prores
            report['prores']=stock_prores.convert(root/'timelapse.gvid',root/'timelapse-prores.mov',**prores)
        report['complete']=True;save()
    except BaseException as error:
        report['error']=str(error);save();raise
    return report


def add_arguments(parser):
    parser.add_argument('--native-executable',type=Path,help='standalone gpr_reconstruct; requires no Torch at runtime')
    parser.add_argument('--native-artifact',type=Path,help='explicit exported Core ML or Metal artifact directory')
    parser.add_argument('--native-backend',choices=('coreml','mpsgraph','hybrid'),default='mpsgraph')
    parser.add_argument('--native-kernels',type=Path,help='Metal metallib; required for native mpsgraph/hybrid')
    parser.add_argument('--native-units',choices=('cpu','gpu','ane','all'),default='all')
    parser.add_argument('--native-threads',type=int,default=4)


ARGUMENTS=('native_executable','native_artifact','native_backend','native_kernels','native_units','native_threads')


def configuration(args):
    values=vars(args) if not isinstance(args,dict) else args
    if not any(values.get(k) for k in ('native_executable','native_artifact','native_kernels')):return None
    if not values.get('native_executable') or not values.get('native_artifact'):raise ValueError('native reconstruction requires executable and artifact')
    return dict(executable=values['native_executable'],artifact=values['native_artifact'],backend=values.get('native_backend','mpsgraph'),
                kernels=values.get('native_kernels'),units=values.get('native_units','all'),threads=values.get('native_threads',4))
