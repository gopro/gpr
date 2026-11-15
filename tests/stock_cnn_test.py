#!/usr/bin/env python3
"""Optional Torch contract/integration tests; no trained weights are needed."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import numpy as np
import tifffile
import torch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
import raw_media
import stock_still
import stock_video
from raw_media_test import metadata


class Contract(unittest.TestCase):
    def test_reconstructed_gpr_output_and_joint_publication(self):
        import hashlib
        import struct
        import stock_cnn_models
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);meta=metadata()
            meta['image']=[t for t in meta['image'] if t['code'] not in (50714,50717)]
            meta['image'] += [raw_media.record(50717,4,1,struct.pack('<I',15000)),
                              raw_media.record(50714,5,4,struct.pack('<8I',33,2,35,2,37,2,39,2))]
            raw=root/'input.raw'
            (np.arange(128*96,dtype=np.uint16)*7%15000).astype('<u2').tofile(raw)
            source=root/'source.gpr';stock_still.encode(raw,meta,3,args.sdk,source)
            for scale,variant,quality in ((1,'F_ane_no_sr',0),(2,'F_ane',5)):
                backbone=stock_cnn_models.build(variant)
                for parameter in backbone.parameters():parameter.data.zero_()
                checkpoint=root/f'{scale}.pt'
                torch.save(dict(variant=variant,raw_norm=16383.,residual_scale=.01,
                                width=16,depth=3,backbone_state=backbone.state_dict()),checkpoint)
                settings=dict(checkpoint=checkpoint,device='cpu',tile=16,halo=8,scale=scale)
                dng=root/f'{scale}.dng';gpr=root/f'{scale}.gpr'
                if scale==1:
                    subprocess.run([sys.executable,str(Path(stock_still.__file__)),'restore',str(source),
                        '--sample-bits','14','--sdk',str(args.sdk),'--checkpoint',str(checkpoint),
                        '--tile','16','--halo','8','--output',str(dng),'--gpr-output',str(gpr),
                        '--gpr-quality',str(quality)],check=True,capture_output=True,text=True)
                    result=json.loads(dng.with_suffix('.dng.json').read_text())
                else:
                    result=stock_still.decode(source,14,args.sdk,dng,reconstruction=settings,gpr_output=gpr,gpr_quality=quality)
                encoded=result['reencoded_gpr'];self.assertTrue(encoded['lossy']);self.assertFalse(encoded['quality_approval'])
                self.assertEqual(encoded['encode']['quality'],quality)
                self.assertEqual(encoded['encode']['output_sha256'],stock_still.digest(gpr))
                self.assertEqual(encoded['encode']['raw_sha256'],result['restored_raw_sha256'])
                with tifffile.TiffFile(dng) as f:
                    self.assertEqual(hashlib.sha256(f.asarray().astype('<u2').tobytes()).hexdigest(),result['restored_raw_sha256'])
                self.assertEqual(stock_still.canonical(raw_media.extract(gpr,14)),stock_still.canonical(raw_media.extract(dng,14)))
                decoded=stock_still.decode(gpr,14,args.sdk,root/f'{scale}-decoded.dng')
                self.assertEqual(decoded['raw_sha256'],encoded['decoded_raw_sha256'])
                with self.assertRaises(FileExistsError):
                    stock_still.decode(source,14,args.sdk,root/'protected.dng',reconstruction=settings,gpr_output=gpr,gpr_quality=quality)
                self.assertFalse((root/'protected.dng').exists())
                with self.assertRaisesRegex(ValueError,'names must differ'):
                    stock_still.decode(source,14,args.sdk,root/'alias',reconstruction=settings,gpr_output=root/'alias',gpr_quality=quality)
            for extra in (dict(gpr_output=root/'bad.gpr'),dict(gpr_quality=3),dict(gpr_output=root/'bad.gpr',gpr_quality=True)):
                with self.assertRaises(ValueError):stock_still.decode(source,14,args.sdk,root/'bad.dng',reconstruction=settings,**extra)
            with self.assertRaisesRegex(ValueError,'requires reconstruction'):
                stock_still.decode(source,14,args.sdk,root/'bad.dng',gpr_output=root/'bad.gpr',gpr_quality=3)
            # A destination appearing after inference must roll back the DNG
            # while preserving the competing writer's GPR and omitting receipt.
            dng=root/'raced.dng';gpr=root/'raced.gpr';link=stock_still.os.link
            def race(src,dest):
                if Path(dest)==gpr:gpr.write_bytes(b'protected competing output')
                return link(src,dest)
            with patch.object(stock_still.os,'link',side_effect=race):
                with self.assertRaises(FileExistsError):
                    stock_still.decode(source,14,args.sdk,dng,reconstruction=settings,gpr_output=gpr,gpr_quality=3)
            self.assertEqual(gpr.read_bytes(),b'protected competing output')
            self.assertFalse(dng.exists());self.assertFalse(dng.with_suffix('.dng.json').exists())
            self.assertFalse(list(root.glob('.stock-still-*')))

    def test_full_frame_selection_uses_validation_only_and_can_reject_all(self):
        import copy
        import select_stock_sr
        import stock_cnn_models
        import train_stock_sr
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);rows=[]
            values=np.array([500,1001,2003,4007],dtype='<u2')
            for index,split in enumerate(('train','validation','test')):
                pixels=np.empty((256,256),dtype='<u2')
                for site in range(4):pixels[site//2::2,site%2::2]=values[site]+index
                path=root/(split+'.dng');raw_media.write(path,metadata(256,256),pixels.tobytes())
                rows.append(dict(path=str(path),group=split,split=split))
            manifest=root/'sources.json';manifest.write_text(json.dumps(dict(schema='gpr.stock_sr_sources.v1',sources=rows)))
            dataset=root/'dataset';train_stock_sr.build(manifest,args.sdk,dataset,patch=64,train_patches=1,validation_patches=1)
            with tifffile.TiffFile(dataset/'source-001/decoded.dng') as file:decoded=file.asarray()
            low=np.array([decoded[site//2,site%2] for site in range(4)],dtype=np.float64)
            correction=values.astype(np.float64)+1-low
            self.assertTrue(np.any(correction!=0))
            checkpoints=[]
            for name,offset in [('bad',correction+100),('identity',np.zeros(4)),('good',correction)]:
                model=stock_cnn_models.build('F_ane')
                for parameter in model.parameters():parameter.data.zero_()
                model.subpixel[0].bias.data.copy_(torch.tensor(np.repeat(offset/(.01*16383),4),dtype=torch.float32))
                checkpoint=root/(name+'.pt')
                torch.save(dict(variant='F_ane',raw_norm=16383.,residual_scale=.01,width=16,depth=3,
                                backbone_state=model.state_dict()),checkpoint);checkpoints.append(checkpoint)
            evaluate=select_stock_sr.benchmark_stock_sr.evaluate
            with patch.object(select_stock_sr.benchmark_stock_sr,'evaluate',wraps=evaluate) as measured:
                report=select_stock_sr.select(dataset,args.sdk,checkpoints,root/'selected',tile=64,halo=16)
                self.assertEqual(len(measured.call_args_list),1)
                self.assertEqual(Path(measured.call_args.args[0]).resolve(),Path(rows[1]['path']).resolve())
            self.assertTrue(report['complete']);self.assertFalse(report['quality_approval'])
            self.assertFalse(report['final_reference_pixels_used'])
            self.assertEqual(report['selected']['checkpoint']['sha256'],stock_still.digest(checkpoints[2]))
            self.assertEqual(report['selected']['active_mse_counts'],0.)
            self.assertFalse(report['candidates'][0]['eligible'])
            self.assertTrue(report['candidates'][0]['regressions'])
            self.assertFalse(report['candidates'][1]['eligible'])
            rejected=select_stock_sr.select(dataset,args.sdk,checkpoints[:2],root/'rejected',tile=64,halo=16)
            self.assertIsNone(rejected['selected'])
            # Reject duplicate candidates and leaked final pixels before creating output.
            with self.assertRaisesRegex(ValueError,'duplicate candidate'):
                select_stock_sr.select(dataset,args.sdk,[checkpoints[0]]*2,root/'duplicate')
            self.assertFalse((root/'duplicate').exists())
            info=json.loads((dataset/'dataset.json').read_text());bad=copy.deepcopy(info)
            bad['sources'][2]['patch_start']=0;bad['sources'][2]['patch_count']=1
            (dataset/'dataset.json').write_text(json.dumps(bad))
            with self.assertRaisesRegex(ValueError,'final-test pixels leaked'):
                select_stock_sr.select(dataset,args.sdk,checkpoints,root/'leaked')
            self.assertFalse((root/'leaked').exists())

    def test_training_split_guards_and_checkpoint_roundtrip(self):
        import copy
        import train_stock_sr as recipe
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);rows=[]
            for index,split in enumerate(('train','validation','test')):
                path=root/f'{split}.source';path.write_bytes(bytes([index]))
                rows.append(dict(path=str(path),group=split,split=split))
            manifest=dict(schema='gpr.stock_sr_sources.v1',sources=rows)
            audited=recipe.sources(manifest)
            for key,value in (('group','train'),('path',rows[0]['path'])):
                bad=copy.deepcopy(manifest);bad['sources'][1][key]=value
                with self.assertRaises(ValueError):recipe.sources(bad)
            for index,row in enumerate(audited):
                row.update(patch_start=(0,2,None)[index],patch_count=(2,1,0)[index])
            pixels=(np.arange(3*4*64*64,dtype=np.uint32).reshape(3,4,64,64)%4000+400).astype('<u2')
            tensor=torch.from_numpy(pixels.astype(np.float32)/16383)
            base=torch.nn.functional.interpolate(tensor,scale_factor=2,mode='bicubic',align_corners=False).clamp(0,1).numpy()
            base=np.clip(base*16383+.5,0,16383).astype('<u2')
            data=root/'data';data.mkdir();np.save(data/'inputs.npy',pixels);np.save(data/'targets.npy',base+9)
            info=dict(schema='gpr.stock_sr_dataset.v1',complete=True,cfa='RGGB',sample_bits=14,stock_quality=3,
                      output_scale=2,dtype='<u2',sources=audited,input_shape=list(pixels.shape),target_shape=list(base.shape),
                      input_sha256=stock_still.digest(data/'inputs.npy'),target_sha256=stock_still.digest(data/'targets.npy'))
            log=data/'dataset.json';log.write_text(json.dumps(info))
            receipt=recipe.train(data,root/'fit',steps=1,batch=2,validate_every=1)
            self.assertTrue(receipt['complete']);self.assertTrue(receipt['fresh_initialization'])
            self.assertFalse(receipt['pretrained_weights']);self.assertFalse(receipt['quality_approval'])
            self.assertFalse(receipt['final_test_used_for_training_or_selection'])
            model,contract=stock_video.load_cnn(None,root/'fit/best.pt',torch.device('cpu'))
            self.assertEqual(contract['output_scale'],2)
            self.assertEqual(contract['checkpoint_sha256'],receipt['best_checkpoint_sha256'])
            with torch.inference_mode(): predicted=model(tensor[2:])[:,:,32:96,32:96].numpy()
            counts=np.clip(predicted*16383+.5,0,16383).astype('<u2')
            error=counts.astype(np.float64)-(base[2:,:,32:96,32:96]+9)
            self.assertEqual(float(np.square(error).mean()),receipt['best_validation_mse_counts'])
            bad=copy.deepcopy(info);bad['sources'][2]['patch_count']=1
            log.write_text(json.dumps(bad))
            with self.assertRaisesRegex(ValueError,'final-test pixels leaked'):
                recipe.train(data,root/'leak',steps=1)
            self.assertFalse((root/'leak').exists())
            # A perfectly represented reference has infinite PSNR: report null,
            # keep the zero-residual checkpoint and avoid invalid JSON floats.
            np.save(data/'targets.npy',base);info['target_sha256']=stock_still.digest(data/'targets.npy')
            log.write_text(json.dumps(info))
            perfect=recipe.train(data,root/'perfect',steps=1,batch=2,validate_every=1)
            self.assertIsNone(perfect['baseline_validation_psnr_db']);self.assertEqual(perfect['best_step'],0)
            self.assertEqual(perfect['best_validation_mse_counts'],0)
            # The 1x recipe must produce a runtime-loadable cleanup checkpoint
            # and preserve an exact identity when no improvement is possible.
            np.save(data/'targets.npy',pixels)
            info.update(output_scale=1,target_shape=list(pixels.shape),target_sha256=stock_still.digest(data/'targets.npy'))
            log.write_text(json.dumps(info))
            cleanup=recipe.train(data,root/'cleanup',steps=1,batch=2,validate_every=1,shadow_weight=3,guard_regions=True)
            model,contract=stock_video.load_cnn(None,root/'cleanup/best.pt',torch.device('cpu'))
            self.assertEqual(contract['output_scale'],1);self.assertEqual(cleanup['best_step'],0)
            self.assertEqual(cleanup['core_output_plane_pixels'],32)
            with torch.inference_mode():restored=model(tensor).numpy()
            np.testing.assert_array_equal(np.clip(restored*16383+.5,0,16383).astype('<u2'),pixels)
            regions=cleanup['baseline_validation_regions_mse']
            self.assertTrue(recipe.region_guard(regions,regions))
            worse=copy.deepcopy(regions);worse['group:validation']['shadow']=1.
            self.assertFalse(recipe.region_guard(worse,regions))
            np.save(data/'inputs.npy',pixels+1)
            with self.assertRaisesRegex(ValueError,'array hash differs'):
                recipe.train(data,root/'tampered',steps=1)

    def test_native_recording_to_portable_two_times_editable_sequence(self):
        if args.capture is None or args.decoder is None: self.skipTest('native capture/decoder arguments not supplied')
        import stock_cnn_models
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); source=root/'input.raw'; calibration=root/'metadata.json'
            y,x=np.indices((48,48),dtype=np.uint32)
            (((y*123+x*97+x*y*29)&16383).astype('<u2')).tofile(source)
            calibration.write_text(json.dumps(metadata(width=48,height=48)))
            clip=root/'input.gvid'
            subprocess.run([str(args.capture), '--output',str(clip),'--width','48','--height','48',
                            '--quality','3','--frames','2','--workspace-mib','16','--leased',
                            '--input',str(source),'--source-metadata',str(calibration)],check=True,capture_output=True)
            backbone=stock_cnn_models.build('F_ane')
            for parameter in backbone.parameters(): parameter.data.zero_()
            checkpoint=root/'identity-2x.pt'
            torch.save(dict(variant='F_ane',raw_norm=16383.,residual_scale=.01,
                            backbone_state=backbone.state_dict()),checkpoint)
            output=root/'sequence'
            subprocess.run([sys.executable,str(Path(stock_video.__file__)),'restore',str(clip),
                            '--decoder',str(args.decoder),'--checkpoint',str(checkpoint),'--scale','2',
                            '--tile','16','--halo','8','--editable','dng','--out-dir',str(output)],check=True,capture_output=True)
            receipt=json.loads((output/'receipt.json').read_text())
            self.assertTrue(receipt['complete']); self.assertTrue(receipt['end_marker_verified'])
            self.assertEqual(receipt['cnn']['architecture_source'],'bundled')
            self.assertEqual(receipt['cnn']['output_scale'],2)
            self.assertEqual(len(receipt['frames']),2)
            for row in receipt['frames']:
                self.assertEqual((row['output_width'],row['output_height']),(96,96))
                self.assertEqual(row['geometry']['scale'],2)
                stem=output/f'{row["index"]:06d}'
                low=np.fromfile(stem.with_suffix('.raw'),dtype='<u2').reshape(48,48)
                planes=np.stack([low[0::2,0::2],low[0::2,1::2],low[1::2,0::2],low[1::2,1::2]])
                tensor=torch.from_numpy(planes.astype(np.float32)/16383)[None]
                high=torch.nn.functional.interpolate(tensor,scale_factor=2,mode='bicubic',align_corners=False).clamp(0,1)[0].numpy()
                with tifffile.TiffFile(stem.with_suffix('.dng')) as file: pixels=file.asarray()
                for site in range(4):
                    expected=np.clip(high[site]*16383+.5,0,16383).astype('<u2')
                    np.testing.assert_array_equal(pixels[site//2::2,site%2::2],expected)
                raw_media.validate(raw_media.extract(stem.with_suffix('.dng'),14))

    def test_context_halos_match_whole_image(self):
        # A known spatial filter tests seams, four distinct planes and the
        # non-multiple-of-eight image edges against one whole-image inference.
        with tempfile.TemporaryDirectory() as temp:
            h,w=98,130
            y,x=np.indices((h,w),dtype=np.uint32)
            image=((y*131+x*67+x*y*31)&16383).astype('<u2')
            raw=Path(temp)/'source.raw'; image.tofile(raw)
            model=torch.nn.Conv2d(4,4,3,padding=1,groups=4,bias=False)
            with torch.no_grad():
                model.weight[:]=torch.tensor([[0,.125,0],[.125,.5,.125],[0,.125,0]])
            tiled,_=stock_video.restore(raw,w,h,model,torch.device('cpu'),16,8)
            planes=np.stack([image[0::2,0::2],image[0::2,1::2],image[1::2,0::2],image[1::2,1::2]])
            tensor=torch.from_numpy(planes.astype(np.float32)/16383)[None]
            tensor=torch.nn.functional.pad(tensor,(0,7,0,7),mode='replicate')
            with torch.inference_mode(): whole=model(tensor)[0,:,:h//2,:w//2].numpy()
            expected=np.clip(whole*16383+.5,0,16383).astype('<u2')
            for site in range(4):
                np.testing.assert_array_equal(tiled[site//2::2,site%2::2],expected[site])

    def test_nonfinite_and_wrong_shape_outputs_rejected(self):
        class Bad(torch.nn.Module):
            def __init__(self,kind): super().__init__(); self.kind=kind
            def forward(self,x):
                if self.kind=='nan': return x*float('nan')
                if self.kind=='rgb': return x[:,:3]
                return x.repeat_interleave(2,-1)
        with tempfile.TemporaryDirectory() as temp:
            raw=Path(temp)/'input.raw'; np.zeros((48,48),dtype='<u2').tofile(raw)
            for kind in ('nan','rgb','2x'):
                with self.subTest(kind=kind),self.assertRaisesRegex(ValueError,'finite 4-plane'):
                    stock_video.restore(raw,48,48,Bad(kind),torch.device('cpu'),16,8)

    def test_two_times_tiling_matches_whole_image_bicubic(self):
        class Upscale(torch.nn.Module):
            def forward(self,x): return torch.nn.functional.interpolate(x,scale_factor=2,mode='bicubic',align_corners=False).clamp(0,1)
        with tempfile.TemporaryDirectory() as temp:
            h,w=98,130; raw=Path(temp)/'input.raw'
            y,x=np.indices((h,w),dtype=np.uint32)
            image=((y*123+x*97+x*y*29)&16383).astype('<u2'); image.tofile(raw)
            restored,_=stock_video.restore(raw,w,h,Upscale(),torch.device('cpu'),16,8,scale=2)
            planes=np.stack([image[0::2,0::2],image[0::2,1::2],image[1::2,0::2],image[1::2,1::2]])
            tensor=torch.from_numpy(planes.astype(np.float32)/16383)[None]
            tensor=torch.nn.functional.pad(tensor,(0,7,0,7),mode='replicate')
            with torch.inference_mode(): expected=Upscale()(tensor)[0,:,:h,:w].numpy()
            expected=np.clip(expected*16383+.5,0,16383).astype('<u2')
            self.assertEqual(restored.shape,(h*2,w*2))
            for site in range(4): np.testing.assert_array_equal(restored[site//2::2,site%2::2],expected[site])

    def test_bundled_checkpoint_loading_and_scale_contract(self):
        import stock_cnn_models
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp)
            for variant,scale in (('F_ane_no_sr',1),('F_ane',2)):
                checkpoint=root/(variant+'.pt')
                backbone=stock_cnn_models.build(variant)
                # A zero residual makes the checkpoint contract independently
                # predictable, including the 2x clipped bicubic skip path.
                for parameter in backbone.parameters(): parameter.data.zero_()
                torch.save(dict(variant=variant,raw_norm=16383.,residual_scale=.01,
                                width=16,depth=3,backbone_state=backbone.state_dict()),checkpoint)
                model,info=stock_video.load_cnn(None,checkpoint,torch.device('cpu'))
                self.assertEqual(info['architecture_source'],'bundled'); self.assertEqual(info['output_scale'],scale)
                tensor=torch.linspace(0,1,4*16*24).reshape(1,4,16,24)
                expected=tensor if scale==1 else torch.nn.functional.interpolate(tensor,scale_factor=2,mode='bicubic',align_corners=False).clamp(0,1)
                with torch.inference_mode(): torch.testing.assert_close(model(tensor),expected,rtol=0,atol=0)
            bad=root/'bad.pt'
            torch.save(dict(variant='F_ane',raw_norm=65535.,backbone_state={}),bad)
            with self.assertRaisesRegex(ValueError,'normalized'): stock_video.load_cnn(None,bad,torch.device('cpu'))

    def test_still_restoration_preserves_calibration_and_decoded_counts(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); meta=metadata()
            # Nonstandard white and fractional black exercise the private
            # full-domain decoding carrier; published calibration stays exact.
            import struct
            meta['image']=[t for t in meta['image'] if t['code'] not in (50714,50717)]
            meta['image'] += [raw_media.record(50717,4,1,struct.pack('<I',15000)),
                              raw_media.record(50714,5,4,struct.pack('<8I',33,2,35,2,37,2,39,2))]
            raw=root/'source.raw'
            (np.arange(128*96,dtype=np.uint16)%15000).astype('<u2').tofile(raw)
            gpr=root/'source.gpr'; stock_still.encode(raw,meta,3,args.sdk,gpr)
            plain=root/'plain.dng'; baseline=stock_still.decode(gpr,14,args.sdk,plain)
            settings=dict(root=root,checkpoint=root/'unused.pt',device='cpu',tile=16,halo=8)
            info=dict(checkpoint_sha256='test-identity',quality_status='unvalidated_for_stock_video')
            with patch.object(stock_video,'load_cnn',return_value=(torch.nn.Identity(),info)):
                output=root/'restored.dng'
                receipt=stock_still.decode(gpr,14,args.sdk,output,reconstruction=settings)
                self.assertTrue(receipt['cnn']); self.assertEqual(receipt['action'],'restore')
                self.assertFalse(receipt['quality_approval'])
                self.assertEqual(receipt['raw_sha256'],baseline['raw_sha256'])
                self.assertEqual(receipt['restored_raw_sha256'],baseline['raw_sha256'])
                with tifffile.TiffFile(plain) as a,tifffile.TiffFile(output) as b:
                    np.testing.assert_array_equal(a.asarray(),b.asarray())
                self.assertEqual(stock_still.canonical(raw_media.extract(output,14)),stock_still.canonical(meta))
                before=output.read_bytes()
                with self.assertRaises(FileExistsError): stock_still.decode(gpr,14,args.sdk,output,reconstruction=settings)
                self.assertEqual(output.read_bytes(),before)
                with self.assertRaisesRegex(ValueError,'tile/halo'):
                    stock_still.decode(gpr,14,args.sdk,root/'bad.dng',reconstruction=dict(settings,tile=0))
                self.assertFalse((root/'bad.dng').exists())
            class Two(torch.nn.Module):
                def forward(self,x): return x.repeat_interleave(2,-1).repeat_interleave(2,-2)
            with patch.object(stock_video,'load_cnn',return_value=(Two(),dict(info,output_scale=2))):
                with self.assertRaisesRegex(ValueError,'output scale differs'):
                    stock_still.decode(gpr,14,args.sdk,root/'wrong-scale.dng',reconstruction=settings)
                high=root/'2x.dng'
                result=stock_still.decode(gpr,14,args.sdk,high,reconstruction=dict(settings,scale=2))
                self.assertEqual((result['width'],result['height']),(256,192))
                self.assertEqual(result['output_scale'],2)
                high_meta=raw_media.extract(high,14)
                self.assertEqual((high_meta['width'],high_meta['height']),(256,192))
                source_tags={t['code']:raw_media.unpack_record(t) for t in meta['image']}
                high_tags={t['code']:raw_media.unpack_record(t) for t in high_meta['image']}
                self.assertEqual(source_tags,high_tags)
                with tifffile.TiffFile(plain) as a,tifffile.TiffFile(high) as b:
                    low=a.asarray(); pixels=b.asarray()
                    for site in range(4):
                        expected=np.repeat(np.repeat(low[site//2::2,site%2::2],2,axis=0),2,axis=1)
                        np.testing.assert_array_equal(pixels[site//2::2,site%2::2],expected)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk',type=Path,required=True)
    parser.add_argument('--capture',type=Path); parser.add_argument('--decoder',type=Path)
    args=parser.parse_args(); args.sdk=args.sdk.resolve()
    if args.capture: args.capture=args.capture.resolve()
    if args.decoder: args.decoder=args.decoder.resolve()
    torch.set_num_threads(1)
    unittest.main(argv=[sys.argv[0]])
