#!/usr/bin/env python3
"""Optional checkpoint-bound MPSGraph/custom Metal residual execution.

BN folding and raw fp16 blob layout follow the original PR #95 exporter.
The current host retains baseline, residual scale, tiling and count rounding.
"""
import argparse
from collections import OrderedDict
import ctypes
import hashlib
import json
from pathlib import Path
import platform
import tempfile
import threading


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def export(checkpoint, output_dir):
    import numpy as np
    import torch
    import stock_video
    model,info=stock_video.load_cnn(None,checkpoint,torch.device('cpu'))
    checkpoint_data=torch.load(checkpoint,map_location='cpu',weights_only=True)
    root=Path(output_dir).absolute()
    if root.exists() or root.is_symlink():raise FileExistsError(root)
    root.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.stock-metal-',dir=root.parent) as temp:
        staged=Path(temp)/'export';staged.mkdir();weights=staged/'weights';weights.mkdir()
        def save(name,value):
            array=value.detach().cpu().numpy() if hasattr(value,'detach') else np.asarray(value)
            array=np.ascontiguousarray(array,dtype='<f2')
            if not np.isfinite(array).all():raise ValueError('weights exceed finite fp16 range')
            array.tofile(weights/(name+'.bin'))
        def pair(name,layer):
            save(name+'_weight',layer.weight)
            if layer.bias is not None:save(name+'_bias',layer.bias)
        def fold(name,bn,conv):
            w=conv.weight.detach().cpu().numpy()[:,:,0,0]
            scale=bn.weight.detach().cpu().numpy()/np.sqrt(bn.running_var.detach().cpu().numpy()+bn.eps)
            shift=bn.bias.detach().cpu().numpy()-bn.running_mean.detach().cpu().numpy()*scale
            save(name+'_weight',w*scale[None,:])
            save(name+'_bias',conv.bias.detach().cpu().numpy()+(w*shift[None,:]).sum(axis=1))
        def block(name,layer):
            fold(name+'_conv1',layer.bn1,layer.conv1);pair(name+'_dw',layer.dw)
            pair(name+'_proj1',layer.proj1);fold(name+'_mlp1',layer.bn2,layer.mlp1);pair(name+'_mlp2',layer.mlp2)
        net=model.backbone;pair('intro',net.intro)
        for index in range(3):
            block(f'enc{index}',net.encoders[index][0]);pair(f'down{index}',net.downs[index])
            pair(f'up{index}',net.ups[index][0]);block(f'dec{index}',net.decoders[index][0])
        block('middle',net.middle[0])
        pair('subpixel' if info['output_scale']==2 else 'outro',net.subpixel[0] if info['output_scale']==2 else net.outro)
        (weights/'MANIFEST.txt').write_text('bn_folded=true\nwidth=16\ndepth=3\ndw_kernel=3\nactivation=silu\n')
        files={path.name:dict(bytes=path.stat().st_size,sha256=digest(path)) for path in sorted(weights.iterdir())}
        contract=dict(schema='gpr.stock_metal.v1',checkpoint_sha256=info['checkpoint_sha256'],
            architecture_sha256=info['architecture_sha256'],output_scale=info['output_scale'],
            residual_scale=float(checkpoint_data['residual_scale']),runtime_raw_norm=16383,input_cfa='RGGB',
            precision='float16',max_plane=512,width=16,depth=3,dw_kernel=3,files=files,quality_approval=False)
        (staged/'contract.json').write_text(json.dumps(contract,indent=2)+'\n')
        root.mkdir()
        try:
            for path in staged.iterdir():path.rename(root/path.name)
        except BaseException:
            import shutil
            shutil.rmtree(root);raise
    return contract


def load_runtime(configuration, info, residual_scale):
    if platform.system()!='Darwin':raise ValueError('native MPSGraph/Metal inference requires macOS')
    import numpy as np
    import torch
    root=Path(configuration['weights']);library=Path(configuration['library']).resolve()
    kernels=Path(configuration['kernels']).resolve();backend=configuration.get('backend','mpsgraph')
    if backend not in ('mpsgraph','hybrid'):raise ValueError('unsupported Metal backend')
    contract_path=root/'contract.json';contract_bytes=contract_path.read_bytes()
    contract=json.loads(contract_bytes);weights=root/'weights'
    if (contract.get('schema')!='gpr.stock_metal.v1' or contract.get('quality_approval') is not False
            or any(contract.get(k)!=v for k,v in dict(runtime_raw_norm=16383,input_cfa='RGGB',precision='float16',
                max_plane=512,width=16,depth=3,dw_kernel=3,residual_scale=residual_scale).items())
            or any(contract.get(k)!=info[k] for k in ('checkpoint_sha256','architecture_sha256','output_scale'))):
        raise ValueError('Metal artifact does not match checkpoint or architecture')
    files=contract.get('files',{})
    if (weights.is_symlink() or not weights.is_dir() or not files
            or set(files)!={p.name for p in weights.iterdir()}):raise ValueError('invalid Metal weight directory')
    verified_bytes={}
    for name,expected in files.items():
        path=weights/name
        if Path(name).name!=name or path.is_symlink() or not path.is_file():
            raise ValueError('Metal weight bytes differ from export')
        data=path.read_bytes()
        if len(data)!=expected.get('bytes') or hashlib.sha256(data).hexdigest()!=expected.get('sha256'):
            raise ValueError('Metal weight bytes differ from export')
        verified_bytes[name]=data
    if not library.is_file() or not kernels.is_file():raise ValueError('requires explicit native library and metallib')
    kernel_bytes=kernels.read_bytes()
    native=ctypes.CDLL(str(library));pointer=ctypes.POINTER(ctypes.c_float)
    native.gpr_metal_create.argtypes=[ctypes.c_char_p,ctypes.c_char_p,*([ctypes.c_uint32]*4)]
    native.gpr_metal_create.restype=ctypes.c_void_p
    native.gpr_metal_predict.argtypes=[ctypes.c_void_p,pointer,ctypes.c_size_t,pointer,ctypes.c_size_t]
    native.gpr_metal_predict.restype=ctypes.c_int
    native.gpr_metal_destroy.argtypes=[ctypes.c_void_p];native.gpr_metal_destroy.restype=None
    scale=info['output_scale']

    class NativeResidual(torch.nn.Module):
        def __init__(self):
            super().__init__();self.contexts=OrderedDict();self.lock=threading.Lock();self.closed=False
            # Future spatial cache misses must read the same verified weights,
            # even if the caller changes the external export during a clip.
            self.snapshot=tempfile.TemporaryDirectory(prefix='gpr-metal-loaded-')
            self.weights_path=Path(self.snapshot.name)/'weights';self.weights_path.mkdir()
            self.kernels_path=Path(self.snapshot.name)/'kernels.metallib';self.kernels_path.write_bytes(kernel_bytes)
            for name,data in verified_bytes.items():(self.weights_path/name).write_bytes(data)
        def close(self):
            with self.lock:
                if self.closed:return
                while self.contexts:native.gpr_metal_destroy(self.contexts.popitem(last=False)[1])
                self.snapshot.cleanup();self.closed=True
        def __del__(self):self.close()
        def forward(self,x):
            if (x.device.type!='cpu' or x.dtype!=torch.float32 or x.ndim!=4 or tuple(x.shape[:2])!=(1,4)
                    or any(v<8 or v>512 or v%8 for v in x.shape[-2:])):
                raise ValueError('Metal requires CPU float32 1x4 planes, multiples of eight within 512')
            h,w=x.shape[-2:];array=x.detach().contiguous().numpy()
            residual=np.empty((1,4,h*scale,w*scale),dtype=np.float32)
            with self.lock:
                if self.closed:raise RuntimeError('Metal model is closed')
                key=(h,w)
                context=self.contexts.pop(key,None)
                if context is None:
                    if len(self.contexts)>=2:native.gpr_metal_destroy(self.contexts.popitem(last=False)[1])
                    context=native.gpr_metal_create(str(self.weights_path).encode(),str(self.kernels_path).encode(),h,w,scale,backend=='hybrid')
                    if not context:raise RuntimeError('native Metal engine creation failed')
                self.contexts[key]=context
                status=native.gpr_metal_predict(context,array.ctypes.data_as(pointer),array.size,
                                               residual.ctypes.data_as(pointer),residual.size)
                if status or not np.isfinite(residual).all():raise RuntimeError('native Metal residual inference failed')
            base=x if scale==1 else torch.nn.functional.interpolate(x,scale_factor=scale,mode='bicubic',align_corners=False).clamp(0,1)
            return (base+residual_scale*torch.from_numpy(residual)).clamp(0,1)
    info.update(runtime_backend='mpsgraph' if backend=='mpsgraph' else 'metal_hybrid',metal_precision='float16',
        metal_contract_sha256=hashlib.sha256(contract_bytes).hexdigest(),metal_library_sha256=digest(library),
        metallib_sha256=hashlib.sha256(kernel_bytes).hexdigest(),
        metal_context_cache_limit=2,metal_max_plane=512)
    return NativeResidual().eval()


ARGUMENTS=('metal_weights','metal_library','metal_kernels','metal_backend')


def add_arguments(parser):
    for name in ARGUMENTS[:3]:parser.add_argument('--'+name.replace('_','-'),type=Path)
    parser.add_argument('--metal-backend',choices=('mpsgraph','hybrid'),default='mpsgraph')


def configuration(args):
    values=vars(args) if not isinstance(args,dict) else args
    selected=[values.get(name) for name in ARGUMENTS[:3]]
    if not any(selected) and values.get('metal_backend','mpsgraph')=='mpsgraph':return None
    if not all(selected):raise ValueError('Metal requires weights, native library and metallib paths')
    return dict(zip(('weights','library','kernels'),selected),backend=values['metal_backend'])


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('checkpoint',type=Path)
    parser.add_argument('--output-dir',required=True,type=Path)
    print(json.dumps(export(**vars(parser.parse_args())),indent=2))
