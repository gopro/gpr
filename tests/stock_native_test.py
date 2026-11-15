#!/usr/bin/env python3
"""Standalone runtime, host kernel identity and stdlib-only still workflow."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import numpy as np
import tifffile
import torch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import stock_native
import stock_metal
import stock_coreml
import stock_cnn_models
import stock_still
import stock_video
import stock_sequence
import raw_media
from raw_media_test import metadata

ROWS=[]


class Standalone(unittest.TestCase):
    def test_native_matrix_and_standard_library_workflow(self):
        torch.set_num_threads(1);torch.manual_seed(671)
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp)
            for scale,variant in ((1,'F_ane_no_sr'),(2,'F_ane')):
                net=stock_cnn_models.build(variant)
                checkpoint=root/f'{scale}.pt'
                torch.save(dict(variant=variant,raw_norm=16383.,residual_scale=.5,width=16,depth=3,
                                backbone_state=net.state_dict()),checkpoint)
                metal=root/f'{scale}-metal';coreml=root/f'{scale}-coreml'
                stock_metal.export(checkpoint,metal);stock_coreml.export(checkpoint,coreml,max_plane=64)
                for backend in ('mpsgraph','hybrid','coreml'):
                    artifact=coreml if backend=='coreml' else metal
                    if backend=='coreml':reference,info=stock_video.load_cnn(None,checkpoint,torch.device('cpu'),coreml,'cpu')
                    else:reference,info=stock_video.load_cnn(None,checkpoint,torch.device('cpu'),metal=dict(
                        weights=metal,library=args.library,kernels=args.kernels,backend=backend))
                    for w,h in ((128,96),(130,98),(50,50)):
                        raw=root/f'input-{w}.raw'
                        ((np.arange(w*h,dtype=np.uint32)*73)%16384).astype('<u2').tofile(raw)
                        expected,_=stock_video.restore(raw,w,h,reference,torch.device('cpu'),16,8,scale)
                        config=dict(executable=args.native,artifact=artifact,backend=backend,kernels=args.kernels,units='cpu')
                        output=root/f'{scale}-{backend}-{w}.raw'
                        data,receipt=stock_native.restore(raw,w,h,scale,checkpoint,config,output,16,8)
                        self.assertFalse(receipt['torch_required']);self.assertFalse(receipt['quality_approval'])
                        actual=np.frombuffer(data,dtype='<u2').reshape(h*scale,w*scale)
                        worst=int(np.max(np.abs(actual.astype(np.int32)-expected.astype(np.int32))))
                        self.assertLessEqual(worst,1)
                        command=[str(args.native.resolve()),'--input',str(raw),'--output',str(output)+'.scalar',
                            '--artifact',str(artifact),'--checkpoint',str(checkpoint),'--backend',backend,
                            '--kernels',str(args.kernels.resolve()),'--units','cpu','--width',str(w),'--height',str(h),
                            '--scale',str(scale),'--tile','16','--halo','8','--host-kernel','scalar','--threads','1']
                        process=subprocess.run(command,check=True,capture_output=True,text=True)
                        scalar=json.loads(process.stdout)
                        self.assertEqual(Path(str(output)+'.scalar').read_bytes(),data)
                        self.assertEqual(scalar['output_sha256'],receipt['output_sha256'])
                        if backend=='coreml' and w==128:
                            for units in ('gpu','ane','all'):
                                selected=list(command);selected[selected.index('--units')+1]=units
                                unit_output=Path(str(output)+'.'+units)
                                selected[selected.index('--output')+1]=str(unit_output)
                                result=json.loads(subprocess.run(selected,check=True,capture_output=True,text=True).stdout)
                                self.assertEqual(result['model']['compute_units'],units)
                                unit_pixels=np.fromfile(unit_output,dtype='<u2').reshape(expected.shape)
                                self.assertLessEqual(int(np.max(np.abs(unit_pixels.astype(np.int32)-actual.astype(np.int32)))),1)
                        protected=output.read_bytes()
                        with self.assertRaises(RuntimeError):stock_native.restore(raw,w,h,scale,checkpoint,config,output,16,8)
                        self.assertEqual(output.read_bytes(),protected)
                        ROWS.append(dict(scale=scale,backend=backend,width=w,height=h,
                            max_abs_count_difference=worst,scalar_vector_thread_counts_exact=True,
                            native_executable_sha256=receipt['executable_sha256'],native_library_sha256=receipt['native_library_sha256']))
                    if hasattr(reference,'close'):reference.close()
                # The actual still command must run without site packages.
                raw=root/'still.raw';np.arange(128*96,dtype='<u2').tofile(raw)
                source=root/f'source-{scale}.gpr';stock_still.encode(raw,metadata(),3,args.sdk,source)
                output=root/f'stdlib-{scale}.dng';gpr=root/f'stdlib-{scale}.gpr'
                command=[sys.executable,'-S',str(Path(stock_still.__file__)),'restore',str(source),
                    '--sample-bits','14','--checkpoint',str(checkpoint),'--scale',str(scale),'--sdk',str(args.sdk.resolve()),
                    '--output',str(output),'--gpr-output',str(gpr),'--gpr-quality','3','--tile','16','--halo','8',
                    '--native-executable',str(args.native.resolve()),'--native-artifact',str(metal),
                    '--native-backend','hybrid','--native-kernels',str(args.kernels.resolve())]
                receipt=json.loads(subprocess.run(command,check=True,capture_output=True,text=True).stdout)
                self.assertFalse(receipt['native_reconstruction']['torch_required'])
                with tifffile.TiffFile(output) as file:self.assertEqual(file.asarray().shape,(96*scale,128*scale))
                self.assertEqual(receipt['reencoded_gpr']['encode']['output_sha256'],stock_still.digest(gpr))
                clip=root/f'clip-{scale}.gvid';stock_sequence.pack([source],clip,3)
                native_args=['--native-executable',str(args.native.resolve()),'--native-artifact',str(metal),
                             '--native-backend','hybrid','--native-kernels',str(args.kernels.resolve())]
                command=[sys.executable,'-S',str(Path(stock_video.__file__)),'restore',str(clip),
                    '--out-dir',str(root/f'video-{scale}'),'--decoder',str(args.decoder.resolve()),
                    '--checkpoint',str(checkpoint),'--scale',str(scale),'--tile','16','--halo','8','--editable','dng',*native_args]
                video=json.loads(subprocess.run(command,check=True,capture_output=True,text=True).stdout)
                self.assertTrue(video['complete']);self.assertEqual(video['cnn']['host_runtime'],'native')
                if scale==2:
                    import stock_upres
                    command=[sys.executable,'-S',str(Path(stock_upres.__file__)),'--mode','timelapse',
                        '--input-mode','captured_gpr','--input',str(source),'--reference-kind','camera_gpr_replay',
                        '--checkpoint',str(checkpoint),'--sdk',str(args.sdk.resolve()),'--output-quality','3',
                        '--bridge',str(args.bridge.resolve()),'--tile','16','--halo','8','--dng-export',
                        '--output-dir',str(root/'native-timelapse'),*native_args]
                    upres=json.loads(subprocess.run(command,check=True,capture_output=True,text=True).stdout)
                    self.assertTrue(upres['complete']);self.assertEqual(upres['runtime'],'standalone_native')
                    self.assertFalse(upres['numeric_regression_evaluated'])
                    if args.renderer:
                        import stock_prores
                        large=root/'large.raw'
                        ((np.arange(512*384,dtype=np.uint32)*17)%12000+1000).astype('<u2').tofile(large)
                        calibration=metadata(512,384)
                        calibration['image'].append(raw_media.record(50707,1,4,b'\1\3\0\0'))
                        large_gpr=root/'large.gpr';stock_still.encode(large,calibration,3,args.sdk,large_gpr)
                        large_clip=root/'large.gvid';stock_sequence.pack([large_gpr],large_clip,3)
                        command=[sys.executable,'-S',str(Path(stock_prores.__file__)),str(large_clip),
                            '--output',str(root/'native-prores.mov'),'--decoder',str(args.decoder.resolve()),
                            '--renderer',str(args.renderer.resolve()),'--encoder',str(args.encoder.resolve()),
                            '--ffprobe',str(args.probe.resolve()),'--checkpoint',str(checkpoint),
                            '--scale','2','--tile','16','--halo','8',*native_args]
                        rendered=json.loads(subprocess.run(command,check=True,capture_output=True,text=True).stdout)
                        self.assertTrue(rendered['complete']);self.assertEqual(rendered['cnn']['host_runtime'],'native')
                        self.assertEqual(rendered['cnn_settings'],dict(tile=16,halo=8,device='cpu',scale=2))
                # Artifact corruption is rejected before a native output appears.
                bias=metal/'weights/intro_bias.bin';saved=bias.read_bytes();bias.write_bytes(saved[:-2])
                with self.assertRaises(RuntimeError):stock_native.restore(raw,128,96,scale,checkpoint,
                    dict(executable=args.native,artifact=metal,backend='hybrid',kernels=args.kernels),root/'bad.raw',16,8)
                self.assertFalse((root/'bad.raw').exists());bias.write_bytes(saved)


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    for name in ('native','sdk','library','kernels','decoder','bridge'):parser.add_argument('--'+name,type=Path,required=True)
    for name in ('renderer','encoder','probe'):parser.add_argument('--'+name,type=Path)
    parser.add_argument('--receipt',type=Path);args,remaining=parser.parse_known_args()
    result=unittest.main(argv=[sys.argv[0],*remaining],exit=False).result
    if args.receipt:
        record=dict(schema='gpr.native_reconstruct.tests.v1',passed=result.wasSuccessful(),tests=result.testsRun,
            rows=ROWS,quality_approval=False,stdlib_only_still_tested=True,
            stdlib_only_video_tested=True,stdlib_only_timelapse_tested=True,
            native_coreml_compute_units_tested=['cpu','gpu','ane','all'],
            stdlib_only_prores_tested=args.renderer is not None,
            source_files={name:stock_still.digest(Path(stock_native.__file__).with_name(name)) for name in
                ('stock_native.py','stock_still.py','stock_video.py','stock_upres.py','stock_prores.py')})
        args.receipt.parent.mkdir(parents=True,exist_ok=True);args.receipt.write_text(json.dumps(record,indent=2)+'\n')
    sys.exit(0 if result.wasSuccessful() else 1)
