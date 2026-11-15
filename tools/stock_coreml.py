#!/usr/bin/env python3
"""Optional native Core ML residual inference for the bundled stock BIBO models.

Export is explicit and local. Model acceptance does not confer quality approval.
Torch retains the existing baseline, tiling and integer conversion contract.
"""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import tempfile


def tree_digest(root):
    root=Path(root)
    if root.is_symlink() or not root.is_dir():raise ValueError('requires a regular Core ML package directory')
    digest=hashlib.sha256();count=0
    for path in sorted(root.rglob('*')):
        if path.is_symlink():raise ValueError('Core ML artifacts must not contain symlinks')
        if path.is_file():
            relative=path.relative_to(root).as_posix().encode()
            digest.update(len(relative).to_bytes(4,'little'));digest.update(relative)
            digest.update(path.stat().st_size.to_bytes(8,'little'))
            with path.open('rb') as file:
                for block in iter(lambda:file.read(1024*1024),b''):digest.update(block)
            count+=1
    if not count:raise ValueError('empty Core ML package')
    return digest.hexdigest()


def export(checkpoint, output_dir, precision='float32', max_plane=1024):
    if platform.system()!='Darwin':raise ValueError('native Core ML export validation requires macOS')
    if precision not in ('float32','float16'):raise ValueError('unsupported Core ML precision')
    if type(max_plane) is not int or not 32<=max_plane<=2048 or max_plane%8:
        raise ValueError('maximum plane extent must be a multiple of eight within 32..2048')
    import numpy as np
    import torch
    import coremltools as ct
    import stock_video
    model,info=stock_video.load_cnn(None,checkpoint,torch.device('cpu'))
    checkpoint_data=torch.load(checkpoint,map_location='cpu',weights_only=True)
    root=Path(output_dir).absolute()
    if root.exists() or root.is_symlink():raise FileExistsError(root)
    root.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.stock-coreml-',dir=root.parent) as temp:
        staged=Path(temp)/'export';staged.mkdir()
        traced=torch.jit.trace(model.backbone,torch.zeros(1,4,32,32)).eval()
        converted=ct.convert(traced,convert_to='mlprogram',minimum_deployment_target=ct.target.macOS13,
            compute_precision=ct.precision.FLOAT32 if precision=='float32' else ct.precision.FLOAT16,
            inputs=[ct.TensorType(name='bayer_planes',dtype=np.float32,shape=(1,4,
                ct.RangeDim(lower_bound=8,upper_bound=max_plane,default=32),
                ct.RangeDim(lower_bound=8,upper_bound=max_plane,default=32)))],
            outputs=[ct.TensorType(name='residual',dtype=np.float32)],compute_units=ct.ComputeUnit.CPU_ONLY)
        package=staged/'model.mlpackage';converted.save(str(package))
        contract=dict(schema='gpr.stock_coreml.v1',checkpoint_sha256=info['checkpoint_sha256'],
            architecture_sha256=info['architecture_sha256'],output_scale=info['output_scale'],
            runtime_raw_norm=16383,input_cfa='RGGB',residual_scale=float(checkpoint_data['residual_scale']),
            precision=precision,max_plane=max_plane,input_name='bayer_planes',output_name='residual',
            package_sha256=tree_digest(package),torch=torch.__version__,coremltools=ct.__version__,
            quality_approval=False)
        (staged/'contract.json').write_text(json.dumps(contract,indent=2)+'\n')
        # Check the serialized artifact through the same loader used at runtime.
        native=load_runtime(staged,info,float(checkpoint_data['residual_scale']),'cpu')
        with torch.inference_mode():
            probe=torch.linspace(0,1,4*32*32).reshape(1,4,32,32)
            if not torch.isfinite(native(probe)).all():raise ValueError('nonfinite Core ML export probe')
        # mkdir is exclusive even if another process creates the destination.
        root.mkdir()
        try:
            for child in staged.iterdir():child.rename(root/child.name)
        except BaseException:
            import shutil
            shutil.rmtree(root);raise
    return contract


def load_runtime(directory, info, residual_scale, units):
    if platform.system()!='Darwin':raise ValueError('native Core ML inference requires macOS')
    if units not in ('cpu','gpu','ane','all'):raise ValueError('unsupported Core ML compute units')
    import numpy as np
    import torch
    import coremltools as ct
    directory=Path(directory);contract_path=directory/'contract.json'
    contract=json.loads(contract_path.read_text());package=directory/'model.mlpackage'
    if (contract.get('schema')!='gpr.stock_coreml.v1' or contract.get('quality_approval') is not False
            or contract.get('runtime_raw_norm')!=16383 or contract.get('input_cfa')!='RGGB'
            or contract.get('input_name')!='bayer_planes' or contract.get('output_name')!='residual'
            or contract.get('precision') not in ('float32','float16')
            or contract.get('residual_scale')!=residual_scale
            or any(contract.get(key)!=info[key] for key in ('checkpoint_sha256','architecture_sha256','output_scale'))
            or type(contract.get('max_plane')) is not int or not 32<=contract['max_plane']<=2048
            or contract['max_plane']%8 or tree_digest(package)!=contract.get('package_sha256')):
        raise ValueError('Core ML artifact does not match checkpoint, architecture or package contract')
    compute={'cpu':ct.ComputeUnit.CPU_ONLY,'gpu':ct.ComputeUnit.CPU_AND_GPU,
             'ane':ct.ComputeUnit.CPU_AND_NE,'all':ct.ComputeUnit.ALL}
    native=ct.models.MLModel(str(package),compute_units=compute[units])
    scale=info['output_scale']

    class NativeResidual(torch.nn.Module):
        def forward(self,x):
            if (x.device.type!='cpu' or x.dtype!=torch.float32 or x.ndim!=4 or tuple(x.shape[:2])!=(1,4)
                    or any(v<8 or v>contract['max_plane'] or v%8 for v in x.shape[-2:])):
                raise ValueError('Core ML requires CPU float32 1x4 planes within the exported spatial contract')
            residual=native.predict({'bayer_planes':x.detach().contiguous().numpy()})['residual']
            if residual.shape!=(1,4,x.shape[-2]*scale,x.shape[-1]*scale) or not np.isfinite(residual).all():
                raise ValueError('Core ML returned invalid residual planes')
            base=x if scale==1 else torch.nn.functional.interpolate(x,scale_factor=scale,mode='bicubic',align_corners=False).clamp(0,1)
            return (base+residual_scale*torch.from_numpy(np.asarray(residual,dtype=np.float32))).clamp(0,1)

    info.update(runtime_backend='coreml',coreml_compute_units=units,coreml_precision=contract['precision'],
                coreml_package_sha256=contract['package_sha256'],
                coreml_contract_sha256=hashlib.sha256(contract_path.read_bytes()).hexdigest(),
                coremltools=ct.__version__,coreml_max_plane=contract['max_plane'],
                coreml_device_assignment='runtime_selected_within_requested_units')
    return NativeResidual().eval()


def add_arguments(parser):
    parser.add_argument('--coreml-package',type=Path,help='explicit exported stock Core ML directory; checkpoint remains required')
    parser.add_argument('--coreml-units',choices=('cpu','gpu','ane','all'),default='all',
                        help='allowed Core ML units; selection does not prove individual operations use ANE/GPU')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('checkpoint',type=Path);parser.add_argument('--output-dir',required=True,type=Path)
    parser.add_argument('--precision',choices=('float32','float16'),default='float32')
    parser.add_argument('--max-plane',type=int,default=1024)
    print(json.dumps(export(**vars(parser.parse_args())),indent=2))
