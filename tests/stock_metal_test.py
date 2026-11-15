#!/usr/bin/env python3
"""MPSGraph/hybrid residual agreement, lifecycle, artifact and media contracts."""
import argparse
import ctypes
import json
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
import unittest
import numpy as np
import tifffile
import torch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import stock_metal
import stock_cnn_models
import stock_still
import stock_video
import stock_upres
import stock_sequence
import raw_media
from raw_media_test import metadata

ROWS=[]


class Metal(unittest.TestCase):
    def test_counts_artifacts_and_workflows(self):
        torch.set_num_threads(1);torch.manual_seed(671)
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);raw=root/'input.raw'
            ((np.arange(128*96,dtype=np.uint32)*73)%16384).astype('<u2').tofile(raw)
            source=root/'source.gpr';stock_still.encode(raw,metadata(),3,args.sdk,source)
            for scale,variant in ((1,'F_ane_no_sr'),(2,'F_ane')):
                checkpoint=root/f'{scale}.pt';net=stock_cnn_models.build(variant)
                # A large test residual scale exposes wrong layers/channel order
                # and verifies the backend does not hardcode historical 0.01.
                torch.save(dict(variant=variant,raw_norm=16383.,residual_scale=.5,width=16,depth=3,
                                backbone_state=net.state_dict()),checkpoint)
                weights=root/f'{scale}-weights';stock_metal.export(checkpoint,weights)
                with self.assertRaises(FileExistsError):stock_metal.export(checkpoint,weights)
                portable,_=stock_video.load_cnn(None,checkpoint,torch.device('cpu'))
                expected,_=stock_video.restore(raw,128,96,portable,torch.device('cpu'),16,8,scale)
                outputs=[]
                for backend in ('mpsgraph','hybrid'):
                    external_kernels=root/f'{scale}-{backend}.metallib'
                    kernel_bytes=args.kernels.read_bytes();external_kernels.write_bytes(kernel_bytes)
                    config=dict(weights=weights,library=args.library,kernels=external_kernels,backend=backend)
                    native,info=stock_video.load_cnn(None,checkpoint,torch.device('cpu'),metal=config)
                    actual,_=stock_video.restore(raw,128,96,native,torch.device('cpu'),16,8,scale)
                    error=np.abs(actual.astype(np.int32)-expected.astype(np.int32));worst=int(error.max())
                    self.assertLessEqual(worst,32)
                    for h,w in ((8,8),(16,24),(24,32),(8,8)):
                        with torch.inference_mode():pred=native(torch.full((1,4,h,w),.4))
                        self.assertEqual(tuple(pred.shape),(1,4,h*scale,w*scale))
                        self.assertLessEqual(len(native.contexts),2)
                    with self.assertRaises(ValueError):native(torch.zeros(1,4,520,8))
                    with self.assertRaises(ValueError):native(torch.zeros(1,4,9,8))
                    with self.assertRaises(RuntimeError):native(torch.full((1,4,8,8),float('nan')))
                    # A new shape after external artifact mutation must still
                    # use the private verified snapshot held by this instance.
                    probe=torch.full((1,4,32,40),.4)
                    with torch.inference_mode():snapshot_expected=native(probe)
                    with torch.inference_mode():
                        native(torch.full((1,4,40,48),.4));native(torch.full((1,4,48,56),.4))
                    bias=weights/'weights/intro_bias.bin';original_bias=bias.read_bytes();bias.write_bytes(bytes(len(original_bias)))
                    external_kernels.write_bytes(b'invalid replacement')
                    try:
                        with torch.inference_mode():changed_shape=native(probe)
                        self.assertTrue(torch.equal(changed_shape,snapshot_expected))
                    finally:
                        bias.write_bytes(original_bias);external_kernels.write_bytes(kernel_bytes)
                    native.close();native.close()
                    self.assertFalse(Path(native.snapshot.name).exists())
                    with self.assertRaises(RuntimeError):native(torch.full((1,4,8,8),.4))
                    outputs.append(actual)
                    settings=dict(checkpoint=checkpoint,device='cpu',tile=16,halo=8,scale=scale,metal=config)
                    receipt=stock_still.decode(source,14,args.sdk,root/f'{scale}-{backend}.dng',reconstruction=settings,
                        gpr_output=root/f'{scale}-{backend}.gpr',gpr_quality=3)
                    self.assertFalse(receipt['quality_approval']);self.assertEqual(receipt['model']['runtime_backend'],info['runtime_backend'])
                    ROWS.append(dict(scale=scale,backend=backend,residual_scale=.5,
                        max_abs_count_difference=worst,model=info,quality_approval=False))
                    if scale==2:
                        report=stock_upres.execute('timelapse',[source],[],'camera_gpr_replay',checkpoint,args.sdk,
                            root/f'upres-{backend}',None,3,bridge=args.bridge,input_mode='captured_gpr',tile=16,halo=8,metal=config)
                        self.assertTrue(report['complete']);self.assertEqual(report['model']['runtime_backend'],info['runtime_backend'])
                        if backend=='hybrid':
                            large=root/'large.raw'
                            ((np.arange(512*384,dtype=np.uint32)*17)%12000+1000).astype('<u2').tofile(large)
                            large_meta=metadata(512,384)
                            large_meta['image'].append(raw_media.record(50707,1,4,b'\1\3\0\0'))
                            large_gpr=root/'large.gpr';stock_still.encode(large,large_meta,3,args.sdk,large_gpr)
                            clip=root/'large.gvid';stock_sequence.pack([large_gpr],clip,3)
                            command=[sys.executable,str(Path(stock_video.__file__)),'restore',str(clip),
                                '--out-dir',str(root/'video-restore'),'--decoder',str(args.decoder),'--checkpoint',str(checkpoint),
                                '--scale','2','--tile','16','--halo','8','--editable','dng',
                                '--metal-weights',str(weights),'--metal-library',str(args.library),
                                '--metal-kernels',str(args.kernels),'--metal-backend','hybrid']
                            subprocess.run(command,check=True,capture_output=True,text=True)
                            with tifffile.TiffFile(root/'video-restore/000000.dng') as file:
                                self.assertEqual(file.asarray().shape,(768,1024))
                            if args.renderer:
                                import stock_prores
                                rendered=stock_prores.convert(clip,root/'native-prores.mov',args.decoder,args.renderer,
                                    args.encoder,args.probe,checkpoint=checkpoint,scale=2,tile=16,halo=8,metal=config)
                                self.assertEqual(rendered['cnn']['runtime_backend'],'metal_hybrid')
                                self.assertEqual(rendered['frame_count'],1)
                # Hybrid keeps SiLU/projection intermediates in float registers;
                # MPSGraph can round those intermediates to half. Report peer
                # differences rather than inventing cross-backend bit identity.
                peer=int(np.max(np.abs(outputs[0].astype(np.int32)-outputs[1].astype(np.int32))))
                for row in ROWS[-2:]:row['peer_max_abs_count_difference']=peer
                path=weights/'contract.json';saved=path.read_text();contract=json.loads(saved)
                config=dict(weights=weights,library=args.library,kernels=args.kernels,backend='mpsgraph')
                for key,value in (('checkpoint_sha256','0'*64),('output_scale',3-scale),('residual_scale',.01)):
                    path.write_text(json.dumps(dict(contract,**{key:value})))
                    with self.assertRaises(ValueError):stock_video.load_cnn(None,checkpoint,torch.device('cpu'),metal=config)
                path.write_text(saved)
                blob=weights/'weights/intro_bias.bin';saved_blob=blob.read_bytes();blob.write_bytes(saved_blob[:-2])
                with self.assertRaises(ValueError):stock_video.load_cnn(None,checkpoint,torch.device('cpu'),metal=config)
                bridge=ctypes.CDLL(str(args.library.resolve()))
                bridge.gpr_metal_create.argtypes=[ctypes.c_char_p,ctypes.c_char_p,*([ctypes.c_uint32]*4)]
                bridge.gpr_metal_create.restype=ctypes.c_void_p
                self.assertFalse(bridge.gpr_metal_create(str((weights/'weights').resolve()).encode(),
                    str(args.kernels.resolve()).encode(),8,8,scale,0))
                blob.write_bytes(saved_blob)
                with self.assertRaises(ValueError):stock_video.load_cnn(None,checkpoint,torch.device('mps'),metal=config)
                with self.assertRaises(ValueError):stock_video.load_cnn(None,checkpoint,torch.device('cpu'),coreml_package=weights,metal=config)
            # Zero-residual 2x model must retain exact current bicubic/count math.
            for parameter in net.parameters():parameter.data.zero_()
            checkpoint=root/'zero.pt';torch.save(dict(variant='F_ane',raw_norm=16383.,residual_scale=.5,
                width=16,depth=3,backbone_state=net.state_dict()),checkpoint)
            weights=root/'zero';stock_metal.export(checkpoint,weights)
            portable,_=stock_video.load_cnn(None,checkpoint,torch.device('cpu'))
            expected,_=stock_video.restore(raw,128,96,portable,torch.device('cpu'),16,8,2)
            for backend in ('mpsgraph','hybrid'):
                config=dict(weights=weights,library=args.library,kernels=args.kernels,backend=backend)
                native,_=stock_video.load_cnn(None,checkpoint,torch.device('cpu'),metal=config)
                actual,_=stock_video.restore(raw,128,96,native,torch.device('cpu'),16,8,2)
                self.assertTrue(np.array_equal(expected,actual));native.close()


if __name__=='__main__':
    p=argparse.ArgumentParser()
    for name in ('sdk','bridge','library','kernels','decoder'):p.add_argument('--'+name,required=True,type=Path)
    for name in ('renderer','encoder','probe'):p.add_argument('--'+name,type=Path)
    p.add_argument('--receipt',type=Path);args,remaining=p.parse_known_args()
    result=unittest.main(argv=[sys.argv[0],*remaining],exit=False).result
    if args.receipt:
        record=dict(schema='gpr.stock_metal.tests.v1',passed=result.wasSuccessful(),tests=result.testsRun,
            platform=platform.platform(),torch=torch.__version__,rows=ROWS,quality_approval=False,
            integration=dict(still=True,upres=True,gvid_restore=True,prores=args.renderer is not None),
            source_files={name:stock_metal.digest(Path(stock_metal.__file__).with_name(name)) for name in
                ('stock_metal.py','stock_video.py','stock_still.py','stock_upres.py')})
        args.receipt.parent.mkdir(parents=True,exist_ok=True);args.receipt.write_text(json.dumps(record,indent=2)+'\n')
    sys.exit(0 if result.wasSuccessful() else 1)
