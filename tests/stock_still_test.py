#!/usr/bin/env python3
"""Still preset workflows against independent stock bytes and TIFF reading."""
import argparse
import errno
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import numpy as np
import tifffile
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import raw_media as media
import stock_still as still
from raw_media_test import metadata


class Stills(unittest.TestCase):
    def test_sdk_parameter_roundtrip_large_gain_maps(self):
        # Real Mission 1 maps are 65x65: 16,988 bytes per channel, beyond
        # the old parameter reader's fixed 16 KB backing buffer.
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp)
            raw=(np.arange(128*96,dtype=np.uint16).reshape(96,128)*7)&16383
            raw.tofile(root/'input.raw')
            for side in (9,65,97):
                with self.subTest(map_side=side):
                    records=[]
                    for site in range(4):
                        body=struct.pack('>8I2I4dI',site//2,site%2,96,128,0,1,2,2,
                                         side,side,1/128,1/128,0.,0.,1)
                        values=[1+site+((row+col)%3)/4 for row in range(side) for col in range(side)]
                        body+=struct.pack('>'+str(len(values))+'f',*values)
                        records.append(struct.pack('>4I',9,0x01040000,3,len(body))+body)
                    opcodes=struct.pack('>I',4)+b''.join(records)
                    meta=metadata();meta['image']=[r for r in meta['image'] if r['code']!=51009]
                    meta['image'].append(media.record(51009,7,len(opcodes),opcodes))
                    source=root/f'source-{side}.dng';media.write(source,meta,raw.astype('<u2').tobytes())
                    dumped=subprocess.run([str(args.sdk),'-i',str(source),'-d','1'],capture_output=True,text=True)
                    self.assertEqual(dumped.returncode,0,dumped.stderr)
                    params=json.loads(dumped.stdout);self.assertEqual(params['tuning_info']['gain_map']['size'],88+4*side*side)
                    parameter_file=root/f'params-{side}.json';parameter_file.write_text(dumped.stdout)
                    restored=root/f'restored-{side}.dng'
                    result=subprocess.run([str(args.sdk),'-i',str(root/'input.raw'),'-a',str(parameter_file),
                                           '--no-preview','-o',str(restored)],capture_output=True,text=True)
                    self.assertEqual(result.returncode,0,result.stderr)
                    with tifffile.TiffFile(restored) as f:self.assertTrue(np.array_equal(f.asarray(),raw))
                    tags={t['code']:t for t in media.extract(restored,14)['image']}
                    self.assertEqual(media.unpack_record(tags[51009]),opcodes)

    def test_copy_fallback_and_partial_repeats(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); source=root/'staged'; source.write_bytes(b'encoded bytes')
            with mock.patch.object(still.os,'link',side_effect=OSError(errno.EOPNOTSUPP,'links unavailable')):
                still.publish([(source,root/'copied')])
                self.assertEqual((root/'copied').read_bytes(),source.read_bytes())
                protected=root/'protected'; protected.write_bytes(b'original')
                with self.assertRaises(FileExistsError): still.publish([(source,root/'first'),(source,protected)])
                self.assertFalse((root/'first').exists()); self.assertEqual(protected.read_bytes(),b'original')
                with mock.patch.object(still.shutil,'copyfileobj',side_effect=OSError('copy failed')):
                    with self.assertRaises(OSError): still.publish([(source,root/'partial')])
                    self.assertFalse((root/'partial').exists())
            raw=(np.arange(128*96,dtype=np.uint16).reshape(96,128)*7)&16383
            raw.tofile(root/'input.raw')
            for repeat in ((1,2),(2,1)):
                meta=metadata(); meta['image']=[t for t in meta['image'] if t['code'] not in (50713,50714)]
                meta['image'] += [media.record(50713,3,2,struct.pack('<2H',*repeat)),
                                  media.record(50714,5,2,struct.pack('<4I',33,2,17,1))]
                dng=root/f'{repeat[0]}x{repeat[1]}.dng'
                still.encode(root/'input.raw',meta,3,args.sdk,root/f'{repeat[0]}x{repeat[1]}.gpr',
                             editable=dng,receipt=root/'receipts'/f'{repeat[0]}x{repeat[1]}.json')
                self.assertEqual(still.canonical(media.extract(dng,14)),still.canonical(meta))

    def test_even_geometry_and_transform_boundaries(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp)
            for w,h in ((48,48),(50,50),(130,96),(128,98)):
                with self.subTest(width=w,height=h):
                    meta=metadata(width=w,height=h)
                    y,x=np.indices((h,w),dtype=np.uint32)
                    raw=((x*123+y*97+x*y*29)&16383).astype('<u2')
                    source=root/f'{w}x{h}.raw'; raw.tofile(source)
                    editable=root/f'{w}x{h}.dng'; output=root/f'{w}x{h}.gpr'
                    still.encode(source,meta,3,args.sdk,output,editable=editable)
                    recovered=root/f'{w}x{h}-decoded.dng'; still.decode(output,14,args.sdk,recovered)
                    with tifffile.TiffFile(editable) as a,tifffile.TiffFile(recovered) as b:
                        self.assertEqual(a.asarray().shape,(h,w))
                        self.assertTrue(np.array_equal(a.asarray(),b.asarray()))

    def test_all_formats_with_exact_nonstandard_calibration(self):
        positions={'RGGB':(0,1,2,3),'GBRG':(2,0,3,1),'GRBG':(1,0,3,2),'BGGR':(3,1,2,0)}
        y,x=np.indices((96,128),dtype=np.uint32)
        canonical=((x*123+y*97+x*y*29+(x^y)*37)&16383).astype('<u2')
        fixture=(Path(__file__).parent/'fixtures/stock_preview/128x96-q3.vc5').read_bytes()
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp)
            for bits in (12,14,16):
              for phase,sites in positions.items():
                with self.subTest(bits=bits,phase=phase):
                    raw=np.empty_like(canonical)
                    for c,site in enumerate(sites): raw[site//2::2,site%2::2]=(canonical[c//2::2,c%2::2]>>2)<<(bits-12)
                    meta=metadata(bits=bits,cfa=phase)
                    meta['image']=[t for t in meta['image'] if t['code'] not in (50714,50717)]
                    meta['image'] += [media.record(50717,4,1,struct.pack('<I',(1<<bits)-101)),
                                      media.record(50714,5,4,struct.pack('<8I',33,2,17,1,18,1,19,1))]
                    prefix=f'{phase}{bits}'
                    path=root/f'{prefix}.raw'; raw.tofile(path)
                    output=root/f'{prefix}.gpr'; editable=root/f'{prefix}.dng'
                    report=still.encode(path,meta,3,args.sdk,output,editable=editable)
                    self.assertEqual(report['payload_sha256'],hashlib.sha256(fixture).hexdigest())
                    self.assertEqual(report['output_sha256'],still.digest(output))
                    self.assertEqual(report['metadata_sha256'],hashlib.sha256(still.canonical(meta)).hexdigest())
                    recovered,payload=media.extract(output,bits,with_payload=True)
                    self.assertEqual(payload,fixture)
                    self.assertEqual(still.canonical(recovered),still.canonical(meta))
                    decoded=root/f'{prefix}-decoded.dng'; result=still.decode(output,bits,args.sdk,decoded)
                    self.assertEqual(result['metadata_sha256'],report['metadata_sha256'])
                    self.assertEqual(still.canonical(media.extract(decoded,bits)),still.canonical(meta))
                    with tifffile.TiffFile(editable) as a,tifffile.TiffFile(decoded) as b:
                        self.assertTrue(np.array_equal(a.asarray(),b.asarray()))
                        self.assertEqual(a.pages[0].tags[50714].value,(33,2,17,1,18,1,19,1))
                    self.assertEqual(json.loads(output.with_suffix('.gpr.json').read_text()),report)
                    with self.assertRaises(FileExistsError): still.encode(path,meta,3,args.sdk,output)

    def test_dng_input_and_exclusive_publication(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); raw=(np.arange(128*96,dtype=np.uint16).reshape(96,128)*7)&16383
            meta=metadata(); meta['image']=[t for t in meta['image'] if t['code']!=50717]
            meta['image'].append(media.record(50717,4,1,struct.pack('<I',15000)))
            source=root/'source.dng'; media.write(source,meta,raw.tobytes())
            result=still.encode(source,meta,0,args.sdk,root/'encoded.gpr',source_type='dng')
            self.assertEqual(result['raw_sha256'],hashlib.sha256(raw.tobytes()).hexdigest())
            # A competing existing output is never replaced; earlier links roll back.
            staged=root/'staged'; staged.write_bytes(b'new')
            protected=root/'protected'; protected.write_bytes(b'original')
            with self.assertRaises(FileExistsError): still.publish([(staged,root/'first'),(staged,protected)])
            self.assertFalse((root/'first').exists()); self.assertEqual(protected.read_bytes(),b'original')
            short=root/'short.raw'; short.write_bytes(bytes(10))
            with self.assertRaises(ValueError): still.encode(short,meta,3,args.sdk,root/'bad.gpr')
            self.assertFalse((root/'bad.gpr').exists())
            overflow=root/'overflow.raw'; np.full((96,128),65535,dtype='<u2').tofile(overflow)
            with self.assertRaises(ValueError): still.encode(overflow,meta,3,args.sdk,root/'overflow.gpr')
            with self.assertRaises(ValueError): still.encode(short,meta,6,args.sdk,root/'invalid.gpr')


if __name__=='__main__':
    p=argparse.ArgumentParser(); p.add_argument('--sdk',type=Path,required=True); args=p.parse_args()
    unittest.main(argv=[sys.argv[0]])
