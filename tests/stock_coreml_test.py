#!/usr/bin/env python3
"""Native Core ML vs Torch Bayer counts, artifact integrity and still integration."""
import argparse
import json
from pathlib import Path
import platform
import struct
import subprocess
import sys
import tempfile
import unittest

import numpy as np
import tifffile
import torch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import stock_coreml
import stock_cnn_models
import stock_still
import stock_video
import stock_sequence
import raw_media
from raw_media_test import metadata

ROWS=[]


class Native(unittest.TestCase):
    def test_native_counts_and_artifact_contract(self):
        torch.set_num_threads(1);torch.manual_seed(671)
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp)
            raw=root/'source.raw'
            ((np.arange(128*96,dtype=np.uint32)*73)%16384).astype('<u2').tofile(raw)
            gpr=root/'source.gpr';stock_still.encode(raw,metadata(),3,args.sdk,gpr)
            for scale,variant in ((1,'F_ane_no_sr'),(2,'F_ane')):
                checkpoint=root/f'{scale}.pt';backbone=stock_cnn_models.build(variant).eval()
                torch.save(dict(variant=variant,raw_norm=16383.,residual_scale=.01,width=16,depth=3,
                                backbone_state=backbone.state_dict()),checkpoint)
                portable,info=stock_video.load_cnn(None,checkpoint,torch.device('cpu'))
                for precision in ('float32','float16'):
                    package=root/f'{scale}-{precision}'
                    contract=stock_coreml.export(checkpoint,package,precision,max_plane=64)
                    with self.assertRaises(FileExistsError):stock_coreml.export(checkpoint,package,precision,max_plane=64)
                    for units in ('cpu','gpu','ane','all'):
                        native,native_info=stock_video.load_cnn(None,checkpoint,torch.device('cpu'),package,units)
                        worst=0
                        for w,h in ((128,96),(112,80)):
                            cropped=root/f'{w}.raw'
                            np.fromfile(raw,dtype='<u2').reshape(96,128)[:h,:w].tofile(cropped)
                            expected,_=stock_video.restore(cropped,w,h,portable,torch.device('cpu'),16,8,scale)
                            actual,_=stock_video.restore(cropped,w,h,native,torch.device('cpu'),16,8,scale)
                            difference=np.abs(actual.astype(np.int32)-expected.astype(np.int32))
                            worst=max(worst,int(difference.max()))
                        self.assertLessEqual(worst,2 if precision=='float16' else 1)
                        ROWS.append(dict(scale=scale,precision=precision,units=units,max_abs_count_difference=worst,
                                         model=native_info,quality_approval=False))
                    settings=dict(checkpoint=checkpoint,device='cpu',tile=16,halo=8,scale=scale,
                                  coreml_package=package,coreml_units='cpu')
                    output=root/f'{scale}-{precision}.dng'
                    receipt=stock_still.decode(gpr,14,args.sdk,output,reconstruction=settings,
                                              gpr_output=root/f'{scale}-{precision}.gpr',gpr_quality=3)
                    self.assertEqual(receipt['model']['runtime_backend'],'coreml')
                    self.assertFalse(receipt['quality_approval'])
                    with tifffile.TiffFile(output) as file:self.assertEqual(file.asarray().shape,(96*scale,128*scale))
                    with self.assertRaises(ValueError):stock_video.load_cnn(None,checkpoint,torch.device('mps'),package)
                    native,_=stock_video.load_cnn(None,checkpoint,torch.device('cpu'),package)
                    with self.assertRaises(ValueError):native(torch.zeros(1,4,72,72))
                    with self.assertRaises(ValueError):native(torch.zeros(1,4,15,16))
                    path=package/'contract.json';saved=path.read_text();changed=json.loads(saved)
                    for field,value in (('checkpoint_sha256','0'*64),('architecture_sha256','0'*64),
                                        ('residual_scale',.9),('output_scale',3-scale),('package_sha256','0'*64)):
                        path.write_text(json.dumps(dict(changed,**{field:value})))
                        with self.assertRaises(ValueError):stock_video.load_cnn(None,checkpoint,torch.device('cpu'),package)
                    path.write_text(saved)
                    self.assertEqual(stock_coreml.tree_digest(package/'model.mlpackage'),contract['package_sha256'])
                    link=package/'model.mlpackage/unexpected-link';link.symlink_to(checkpoint)
                    try:
                        with self.assertRaises(ValueError):stock_video.load_cnn(None,checkpoint,torch.device('cpu'),package)
                    finally:link.unlink()
                    if scale==2 and precision=='float32':
                        large=root/'large.raw'
                        ((np.arange(512*384,dtype=np.uint32)*17)%12000+1000).astype('<u2').tofile(large)
                        large_meta=metadata(512,384)
                        large_meta['image'].append(raw_media.record(50707,1,4,b'\1\3\0\0'))
                        large_gpr=root/'large.gpr';stock_still.encode(large,large_meta,3,args.sdk,large_gpr)
                        clip=root/'large.gvid';stock_sequence.pack([large_gpr],clip,3)
                        command=[sys.executable,str(Path(stock_video.__file__)),'restore',str(clip),
                            '--out-dir',str(root/'video-restore'),'--decoder',str(args.decoder),
                            '--checkpoint',str(checkpoint),'--scale','2','--tile','16','--halo','8',
                            '--editable','dng','--coreml-package',str(package),'--coreml-units','cpu']
                        subprocess.run(command,check=True,capture_output=True,text=True)
                        restored=root/'video-restore/000000.dng'
                        with tifffile.TiffFile(restored) as file:self.assertEqual(file.asarray().shape,(768,1024))
                        if args.renderer:
                            import stock_prores
                            result=stock_prores.convert(clip,root/'native-prores.mov',args.decoder,args.renderer,
                                args.encoder,args.probe,checkpoint=checkpoint,scale=2,tile=16,halo=8,
                                coreml_package=package,coreml_units='cpu')
                            self.assertEqual(result['cnn']['runtime_backend'],'coreml')
                            self.assertEqual(result['frame_count'],1)


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--sdk',required=True,type=Path)
    parser.add_argument('--decoder',required=True,type=Path)
    for name in ('renderer','encoder','probe'):parser.add_argument('--'+name,type=Path)
    parser.add_argument('--receipt',type=Path)
    args,remaining=parser.parse_known_args()
    result=unittest.main(argv=[sys.argv[0],*remaining],exit=False).result
    if args.receipt:
        record=dict(schema='gpr.stock_coreml.tests.v1',passed=result.wasSuccessful(),tests=result.testsRun,
                    platform=platform.platform(),torch=torch.__version__,rows=ROWS,quality_approval=False,
                    sdk_sha256=stock_still.digest(args.sdk),
                    integration=dict(still=True,gvid_restore=True,prores=args.renderer is not None),
                    source_files={name:stock_still.digest(Path(stock_coreml.__file__).with_name(name))
                                  for name in ('stock_coreml.py','stock_video.py','stock_still.py','stock_cnn_models.py')})
        args.receipt.parent.mkdir(parents=True,exist_ok=True);args.receipt.write_text(json.dumps(record,indent=2)+'\n')
    sys.exit(0 if result.wasSuccessful() else 1)
