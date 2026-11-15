#!/usr/bin/env python3
"""Exercise the complete simulated capture/reconstruction/export workflow."""
import argparse
import json
import platform
import struct
import subprocess
from pathlib import Path
import sys
import tempfile
import unittest

import numpy as np
import tifffile
import torch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import raw_media
import stock_cnn_models
import stock_cnn_metadata
import stock_still
import stock_upres
import stock_video
from raw_media_test import metadata


class Workflow(unittest.TestCase):
    def test_simulated_calibration_contract(self):
        meta=metadata()
        meta['image'] += [raw_media.record(50829,4,4,struct.pack('<4I',4,8,92,120)),
                          raw_media.record(50719,5,2,struct.pack('<4I',3,2,5,2)),
                          raw_media.record(50720,4,2,struct.pack('<2I',100,80)),
                          raw_media.record(700,1,4,b'XMP!')]
        half,omitted=stock_cnn_metadata.simulated_half(meta)
        restored,_=stock_cnn_metadata.scaled(half,2)
        original={tag['code']:raw_media.values(tag) for tag in meta['image'] if tag['code'] in (50829,50719)}
        final={tag['code']:raw_media.values(tag) for tag in restored['image'] if tag['code'] in original}
        self.assertEqual(original,final);self.assertEqual(omitted[0]['code'],700)
        meta['image'].append(raw_media.record(50715,10,1,struct.pack('<2i',1,1)))
        with self.assertRaises(ValueError):stock_cnn_metadata.simulated_half(meta)

    def test_all_modes_and_failure_accounting(self):
        torch.set_num_threads(1)
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);sources=[]
            backbone=stock_cnn_models.build('F_ane')
            for parameter in backbone.parameters():parameter.data.zero_()
            checkpoint=root/'bicubic.pt'
            torch.save(dict(variant='F_ane',raw_norm=16383.,residual_scale=.01,width=16,depth=3,
                            backbone_state=backbone.state_dict()),checkpoint)
            for index in range(2):
                source=root/f'{index}.dng';sources.append(source)
                pixels=((np.arange(128*96,dtype=np.uint32)*17+index*911)%16384).astype('<u2').reshape(96,128)
                raw_media.write(source,metadata(),pixels.tobytes())
            hashes=[stock_still.digest(p) for p in sources]
            common=dict(reference_kind='lossless_sensor_dng',checkpoint=checkpoint,sdk=args.sdk,
                        capture_quality=3,output_quality=5,bridge=args.bridge,tile=16,halo=8,dng_export=True)
            for mode in ('regression','timelapse','both'):
                output=root/mode
                report=stock_upres.execute(mode,sources if mode!='regression' else [],
                                          sources[:1] if mode!='timelapse' else [],output_dir=output,**common)
                self.assertTrue(report['complete']);self.assertFalse(report['quality_approval'])
                for row in report['frames']:
                    frame=output/Path(row['full_gpr']).parent
                    with tifffile.TiffFile(frame/'model-output.dng') as file:model=file.asarray()
                    self.assertEqual(model.shape,(96,128))
                    self.assertEqual(stock_still.canonical(raw_media.extract(frame/'model-output.dng',14)),
                                     stock_still.canonical(raw_media.extract(frame/'full.gpr',14)))
                    if row['group']=='regression':
                        self.assertEqual(row['bicubic'],row['model_output'])
                        self.assertFalse(row['quality_approval'])
                if mode!='regression':
                    with (output/'timelapse.gvid').open('rb') as file:
                        header=stock_video.read_header(file);frames=list(stock_video.records(file,header))
                    self.assertEqual(len(frames),2)
                    for index,frame in enumerate(frames):
                        _,payload=raw_media.extract(output/f'{index:06d}-timelapse/full.gpr',14,with_payload=True)
                        self.assertEqual(frame['payload'],payload)
                    self.assertTrue((output/'timelapse.mov').is_file())
                with self.assertRaises(FileExistsError):
                    stock_upres.execute(mode,sources if mode!='regression' else [],sources[:1] if mode!='timelapse' else [],output_dir=output,**common)
            self.assertEqual(hashes,[stock_still.digest(p) for p in sources])
            replay=stock_upres.execute('regression',[],[root/'regression/000000-regression/full.gpr'],
                output_dir=root/'replay',**dict(common,reference_kind='camera_gpr_replay'))
            self.assertTrue(replay['complete']);self.assertEqual(replay['reference_kind'],'camera_gpr_replay')
            captured=root/'regression/000000-regression/full.gpr'
            captured_meta,captured_payload=raw_media.extract(captured,14,with_payload=True)
            body=struct.pack('>8I',0,0,96,128,0,1,1,1)+struct.pack('>2I4dI4f',2,2,1.,1.,0.,0.,1,1.,1.1,1.2,1.3)
            opcodes=struct.pack('>I4I',1,9,0x01040000,3,len(body))+body
            captured_meta['image']=[t for t in captured_meta['image'] if t['code']!=51009]
            captured_meta['image'].append(raw_media.record(51009,7,len(opcodes),opcodes))
            captured=root/'captured-calibrated.gpr';raw_media.write(captured,captured_meta,captured_payload,'gpr')
            direct=stock_upres.execute('timelapse',[captured],[],output_dir=root/'direct',
                input_mode='captured_gpr',**dict(common,reference_kind='camera_gpr_replay'))
            self.assertFalse(direct['capture_simulated']);self.assertIsNone(direct['capture_quality'])
            direct_frame=root/'direct/000000-timelapse'
            self.assertEqual(raw_media.extract(captured,14,with_payload=True)[1],
                             raw_media.extract(direct_frame/'capture.gpr',14,with_payload=True)[1])
            with tifffile.TiffFile(direct_frame/'model-output.dng') as file:self.assertEqual(file.asarray().shape,(192,256))
            self.assertFalse(direct['frames'][0]['capture_simulated'])
            self.assertIsNone(direct['frames'][0]['capture_quality'])
            expected_meta,_=stock_cnn_metadata.scaled(captured_meta,2)
            self.assertEqual(stock_still.canonical(raw_media.extract(direct_frame/'full.gpr',14)),stock_still.canonical(expected_meta))
            command=[sys.executable,str(Path(stock_upres.__file__)),'--mode','timelapse','--input-mode','captured_gpr',
                '--input',str(captured),'--reference-kind','camera_gpr_replay','--checkpoint',str(checkpoint),
                '--sdk',str(args.sdk),'--output-quality','3','--bridge',str(args.bridge),'--output-dir',str(root/'cli-direct')]
            cli=json.loads(subprocess.run(command,check=True,capture_output=True,text=True).stdout)
            self.assertTrue(cli['complete']);self.assertIsNone(cli['capture_quality'])
            with self.assertRaises(ValueError):
                stock_upres.execute('timelapse',[captured],[],output_dir=root/'refused-simulation',
                    **dict(common,reference_kind='camera_gpr_replay'))
            self.assertFalse((root/'refused-simulation').exists())
            if args.renderer:
                render_source=root/'render.dng';render_meta=metadata(512,384)
                render_meta['image'].append(raw_media.record(50707,1,4,b'\1\3\0\0'))
                y,x=np.indices((384,512))
                raw_media.write(render_source,render_meta,(1000+x*9+y*6).astype('<u2').tobytes())
                report=stock_upres.execute('timelapse',[render_source],[],output_dir=root/'prores',
                    prores=dict(decoder=args.decoder,renderer=args.renderer,encoder=args.encoder,probe=args.probe),**common)
                self.assertTrue(report['complete']);self.assertTrue((root/'prores/timelapse-prores.mov').is_file())
            bad=dict(common,reference_kind='camera_gpr_replay')
            with self.assertRaises(ValueError):
                stock_upres.execute('regression',[],sources[:1],output_dir=root/'failure',**bad)
            failure=json.loads((root/'failure/run.json').read_text())
            self.assertFalse(failure['complete']);self.assertIn('camera replay',failure['error'])
            with self.assertRaises(ValueError):
                stock_upres.execute('both',[],sources,output_dir=root/'invalid',**common)
            self.assertFalse((root/'invalid').exists())
            one=stock_cnn_models.build('F_ane_no_sr');bad_checkpoint=root/'one.pt'
            torch.save(dict(variant='F_ane_no_sr',raw_norm=16383.,residual_scale=.01,
                            width=16,depth=3,backbone_state=one.state_dict()),bad_checkpoint)
            with self.assertRaises(ValueError):
                stock_upres.execute('regression',[],sources[:1],output_dir=root/'wrong-scale',**dict(common,checkpoint=bad_checkpoint))
            self.assertFalse((root/'wrong-scale').exists())


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--sdk',required=True,type=Path)
    parser.add_argument('--bridge',required=True,type=Path)
    parser.add_argument('--receipt',type=Path)
    for name in ('decoder','renderer','encoder','probe'):parser.add_argument('--'+name,type=Path)
    args,remaining=parser.parse_known_args();result=unittest.main(argv=[sys.argv[0],*remaining],exit=False).result
    if args.receipt:
        tools=Path(stock_upres.__file__).parent
        record=dict(schema='gpr.stock_upres.tests.v1',passed=result.wasSuccessful(),tests=result.testsRun,
                    platform=platform.platform(),machine=platform.machine(),torch=torch.__version__,
                    sdk_sha256=stock_still.digest(args.sdk),bridge_sha256=stock_still.digest(args.bridge),
                    tool_sha256={name:stock_still.digest(tools/name) for name in ('stock_upres.py','stock_sequence.py','stock_cnn_metadata.py')},
                    tested_modes=['regression','timelapse','both'],tested_inputs=['simulated','captured_gpr'],prores_tested=args.renderer is not None,
                    quality_approval=False)
        args.receipt.parent.mkdir(parents=True,exist_ok=True)
        args.receipt.write_text(json.dumps(record,indent=2)+'\n')
    sys.exit(0 if result.wasSuccessful() else 1)
