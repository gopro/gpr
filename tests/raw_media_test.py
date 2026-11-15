#!/usr/bin/env python3
"""Calibrated stock payload wrapping, independent TIFF reading and SDK decode."""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import raw_media as media
import stock_video
import numpy as np
import tifffile


def run(command):
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f'{command}: exit {result.returncode}\n{result.stderr}')
    return result


def metadata(width=128, height=96, bits=14, cfa='RGGB'):
    def tag(code, dtype, fmt, data):
        data = tuple(data)
        return media.record(code, dtype, len(data)//2 if dtype in (5, 10) else len(data), struct.pack('<'+fmt*len(data), *data))
    return dict(schema=media.SCHEMA, width=width, height=height, sample_bits=bits, cfa=cfa,
                image=[media.record(271, 2, 5, b'Test\0'), media.record(272, 2, 7, b'Camera\0'),
                       media.record(50708, 2, 12, b'Test Camera\0'), media.record(50706, 1, 4, b'\1\4\0\0'),
                       tag(33421, 3, 'H', (2, 2)), media.record(33422, 1, 4, bytes('RGB'.index(x) for x in cfa)),
                       tag(50713, 3, 'H', (2, 2)), tag(50714, 5, 'I', (16, 1, 17, 1, 18, 1, 19, 1)),
                       tag(50717, 4, 'I', ((1 << bits)-1,)), tag(50728, 5, 'I', (1, 2, 1, 1, 2, 3)),
                       tag(50721, 10, 'i', (1, 1, 0, 1, 0, 1, 0, 1, 1, 1, 0, 1, 0, 1, 0, 1, 1, 1)),
                       tag(50778, 3, 'H', (21,)), media.record(51009, 7, 4, bytes(4))],
                exif=[tag(33434, 5, 'I', (1, 120)), tag(34855, 3, 'H', (800,)), tag(37384, 3, 'H', (14,))],
                gps=[media.record(0, 1, 4, b'\2\3\0\0')])


class Media(unittest.TestCase):
    def test_reconstruction_geometry_and_gain_map_phases(self):
        import copy
        import stock_cnn_metadata as geometry
        meta=metadata()
        meta['image'] += [media.record(50829,4,4,struct.pack('<4I',2,4,94,124)),
                          media.record(50719,5,2,struct.pack('<4I',3,2,5,2)),
                          media.record(50720,4,2,struct.pack('<2I',100,80)),
                          media.record(50830,4,4,struct.pack('<4I',0,0,2,128)),
                          media.record(700,1,4,b'XMP!')]
        meta['exif'] += [media.record(40962,4,1,struct.pack('<I',128)),
                         media.record(40963,4,1,struct.pack('<I',96))]
        h,w=92,120
        def make_list(pitch):
            records=[]
            for sy,sx in ([(0,0)] if pitch==1 else [(0,0),(0,1),(1,0),(1,1)]):
                area=struct.pack('>8I',sy,sx,h,w,0,1,pitch,pitch)
                grid=struct.pack('>2I4dI4f',2,2,1.,1.,0.,0.,1,1.,2.,3.,4.)
                body=area+grid
                records.append(struct.pack('>4I',9,0x01040000,3,len(body))+body)
            return struct.pack('>I',len(records))+b''.join(records)
        original=copy.deepcopy(meta)
        for pitch in (1,2):
            case=copy.deepcopy(meta)
            data=make_list(pitch)
            case['image']=[t for t in case['image'] if t['code']!=51009]+[media.record(51009,7,len(data),data)]
            high,accounting=geometry.scaled(case,2)
            self.assertEqual((high['width'],high['height']),(256,192))
            tags={t['code']:t for t in high['image']}
            self.assertEqual(media.values(tags[50829]),(4,8,188,248))
            self.assertEqual(media.values(tags[50719]),(3.,5.))
            self.assertEqual(media.values(tags[50720]),(200,160))
            self.assertEqual(media.values(tags[50830]),(0,0,4,256))
            self.assertNotIn(700,tags)
            self.assertEqual(accounting['omitted'][0]['code'],700)
            exif={t['code']:t for t in high['exif']}
            self.assertEqual(media.values(exif[40962]),(256,)); self.assertEqual(media.values(exif[40963]),(192,))
            data=media.unpack_record(tags[51009]); count,=struct.unpack_from('>I',data); self.assertEqual(count,4)
            pos=4; phases=set()
            for _ in range(count):
                op,version,flags,size=struct.unpack_from('>4I',data,pos); pos+=16
                self.assertEqual((op,version,flags),(9,0x01040000,3))
                body=data[pos:pos+size]; pos+=size
                t,l,b,r,plane,planes,rp,cp=struct.unpack_from('>8I',body)
                self.assertEqual((plane,planes,rp,cp),(0,1,2,2))
                self.assertEqual((b,r),(h*2,w*2)); self.assertIn(t,(0,1)); self.assertIn(l,(0,1))
                phases.add((t,l))
                rows,cols,dy,dx,oy,ox,n=struct.unpack_from('>2I4dI',body,32)
                self.assertEqual((rows,cols,n),(2,2,1))
                self.assertEqual(body[76:],struct.pack('>4f',1.,2.,3.,4.))
                # Independent analytic gain plane g(u,v)=1+2u+v at a high
                # Bayer-site center, mapped through align_corners=False's
                # deinterleaved-plane coordinate transform.
                for row,col in ((10+t,14+l),(42+t,66+l)):
                    low_row=2*((row//2+.5)/2-.5)+t
                    low_col=2*((col//2+.5)/2-.5)+l
                    expected=1+2*(low_row+.5)/h+(low_col+.5)/w
                    actual=1+2*((row+.5)/(h*2)-oy)/dy+((col+.5)/(w*2)-ox)/dx
                    self.assertAlmostEqual(actual,expected,places=14)
            self.assertEqual(phases,{(0,0),(0,1),(1,0),(1,1)})
            self.assertEqual(pos,len(data))
        self.assertEqual(meta,original)
        unchanged,accounting=geometry.scaled(meta,1)
        self.assertEqual(unchanged,meta); self.assertEqual(accounting['omitted'],[])
        # Normalized lens coefficients are unchanged by a uniform scale.
        body=struct.pack('>I8d',1,1.,0.,0.,0.,0.,0.,.5,.5)
        data=struct.pack('>5I',1,1,0x01030000,0,len(body))+body
        case=copy.deepcopy(meta); case['image'].append(media.record(51022,7,len(data),data))
        high,_=geometry.scaled(case,2)
        self.assertEqual(media.unpack_record(next(t for t in high['image'] if t['code']==51022)),data)
        for bad_data in (b'\0',data[:-1],data+b'\0'):
            with self.assertRaises(ValueError): geometry.opcode_list(bad_data,3,2,h,w)
        unknown=struct.pack('>5I',1,5,0x01030000,1,0)
        with self.assertRaisesRegex(ValueError,'does not support opcode'):
            geometry.opcode_list(unknown,1,2,h,w)
        case=copy.deepcopy(meta); case['image'].append(media.record(50715,10,1,struct.pack('<2i',0,1)))
        with self.assertRaisesRegex(ValueError,'delta resampling'): geometry.scaled(case,2)
        case=copy.deepcopy(meta)
        case['image']=[t for t in case['image'] if t['code']!=50829]
        case['image'].append(media.record(50829,5,4,struct.pack('<8I',5,2,4,1,94,1,124,1)))
        with self.assertRaisesRegex(ValueError,'integral ActiveArea'): geometry.scaled(case,2)

    def test_simulated_gain_map_centroids_and_rejections(self):
        import copy
        import stock_cnn_metadata as geometry
        h,w=96,128
        for origin in (0.,.125):
            records=[]
            for sy,sx in ((0,0),(0,1),(1,0),(1,1)):
                phase=sy*2+sx
                grid=[1.+phase*.125+y*.25+x*.0625 for y in range(3) for x in range(4)]
                body=struct.pack('>8I',sy,sx,h,w,0,1,2,2)
                body+=struct.pack('>2I4dI12f',3,4,.5,1/3,origin,.0625,1,*grid)
                records.append(struct.pack('>4I',9,0x01040000,1,len(body))+body)
            data=struct.pack('>I',4)+b''.join(records)
            meta=metadata()
            meta['image']=[t for t in meta['image'] if t['code']!=51009]
            meta['image'] += [media.record(51009,7,len(data),data),
                              media.record(50829,4,4,struct.pack('<4I',0,0,h,w))]
            original=copy.deepcopy(meta)
            low,omitted=geometry.simulated_half(meta)
            self.assertEqual(meta,original);self.assertEqual(omitted,[])
            self.assertEqual((low['height'],low['width']),(h//2,w//2))
            low_data=media.unpack_record(next(t for t in low['image'] if t['code']==51009))
            pos=4
            for sy,sx in ((0,0),(0,1),(1,0),(1,1)):
                op,version,flags,size=struct.unpack_from('>4I',low_data,pos);pos+=16
                body=low_data[pos:pos+size];pos+=size
                self.assertEqual((op,version,flags),(9,0x01040000,1))
                self.assertEqual(struct.unpack_from('>8I',body),(sy,sx,h//2,w//2,0,1,2,2))
                rows,cols,dy,dx,oy,ox,planes=struct.unpack_from('>2I4dI',body,32)
                values=np.frombuffer(body,dtype='>f4',offset=76).reshape(rows,cols)
                for j in (0,4,12,h//4-1):
                    for i in (0,4,16,w//4-1):
                        # Independent source-plane ramp, evaluated at the
                        # centroid of the four original same-site samples.
                        source_y=np.clip(((4*j+sy+1.5)/h-origin)/.5,0,2)
                        source_x=np.clip(((4*i+sx+1.5)/w-.0625)/(1/3),0,3)
                        expected=1.+(sy*2+sx)*.125+source_y*.25+source_x*.0625
                        gy=np.clip(((2*j+sy+.5)/(h//2)-oy)/dy,0,rows-1)
                        gx=np.clip(((2*i+sx+.5)/(w//2)-ox)/dx,0,cols-1)
                        iy,ix=int(gy),int(gx);fy,fx=gy-int(gy),gx-int(gx)
                        measured=(1-fy)*((1-fx)*values[iy,ix]+fx*values[iy,min(ix+1,cols-1)])
                        measured+=fy*((1-fx)*values[min(iy+1,rows-1),ix]+fx*values[min(iy+1,rows-1),min(ix+1,cols-1)])
                        self.assertAlmostEqual(measured,expected,places=12)
            high,_=geometry.scaled(low,2)
            high_data=media.unpack_record(next(t for t in high['image'] if t['code']==51009))
            # Grid samples/headers stay exact; origin addition/subtraction can
            # round at a nonzero origin, so compare those doubles numerically.
            for index in range(4):
                offset=4+index*len(records[0]);size=struct.unpack_from('>I',data,offset+12)[0]
                a,b=data[offset+16:offset+16+size],high_data[offset+16:offset+16+size]
                self.assertEqual(a[:32],b[:32]);self.assertEqual(a[76:],b[76:])
                for original_value,restored_value in zip(struct.unpack_from('>2I4dI',a,32),struct.unpack_from('>2I4dI',b,32)):
                    self.assertAlmostEqual(original_value,restored_value,places=14)
        bad_cases=[data[:-1],data+b'\0']
        for offset,value in ((4,8),(20,1),(28,h-2)):
            changed=bytearray(data);struct.pack_into('>I',changed,offset,value);bad_cases.append(bytes(changed))
        changed=bytearray(data);struct.pack_into('>f',changed,96,0.);bad_cases.append(bytes(changed))
        for bad in bad_cases:
            case=copy.deepcopy(meta)
            case['image']=[media.record(51009,7,len(bad),bad) if t['code']==51009 else t for t in case['image']]
            with self.assertRaises(ValueError):geometry.simulated_half(case)
        cropped=copy.deepcopy(meta)
        cropped['image']=[media.record(50829,4,4,struct.pack('<4I',0,0,h-4,w)) if t['code']==50829 else t for t in cropped['image']]
        with self.assertRaisesRegex(ValueError,'full-frame active area'):geometry.simulated_half(cropped)
        stage_one=copy.deepcopy(meta);stage_one['image'].append(media.record(51008,7,len(data),data))
        with self.assertRaisesRegex(ValueError,'empty stage-1'):geometry.simulated_half(stage_one)

    def test_sdk_cli_quality_selection(self):
        def payload(path):
            with tifffile.TiffFile(path) as f:
                page=f.pages[0]
                if 33422 not in page.tags: page=page.pages[0]
                offset,size=page.dataoffsets[0],page.databytecounts[0]
                return path.read_bytes()[offset:offset+size]
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); y,x=np.indices((96,128),dtype=np.uint32)
            raw=((x*123+y*97+x*y*29+(x^y)*37)&16383).astype('<u2')
            raw.tofile(root/'input.raw'); media.write(root/'input.dng',metadata(),raw.tobytes())
            outputs=[]
            for q in range(6):
                raw_gpr,dng_gpr=root/f'raw-q{q}.gpr',root/f'dng-q{q}.gpr'
                run([str(args.sdk),'-i',str(root/'input.raw'),'-o',str(raw_gpr),'-w','128','-h','96','-x','rggb14','-q',str(q)])
                run([str(args.sdk),'-i',str(root/'input.dng'),'-o',str(dng_gpr),'--quality',str(q)])
                self.assertEqual(payload(raw_gpr),payload(dng_gpr)); outputs.append(payload(raw_gpr))
                run([str(args.sdk),'-i',str(dng_gpr),'-o',str(root/f'q{q}.raw')])
                self.assertEqual((root/f'q{q}.raw').stat().st_size,128*96*2)
            self.assertEqual(outputs[3],(Path(__file__).parent/'fixtures/stock_preview/128x96-q3.vc5').read_bytes())
            self.assertNotEqual(outputs[0],outputs[5])
            # JPEG previews are optional; disabling one must not change VC-5.
            no_preview=root/'without-preview.gpr'
            run([str(args.sdk),'-i',str(root/'input.dng'),'-o',str(no_preview),'--no-preview'])
            self.assertEqual(payload(no_preview),outputs[3])
            def compressions(path):
                def walk(pages):
                    for page in pages:
                        yield int(page.compression)
                        if page.pages: yield from walk(page.pages)
                with tifffile.TiffFile(path) as f: return list(walk(f.pages))
            self.assertIn(7,compressions(root/'dng-q3.gpr'))
            self.assertNotIn(7,compressions(no_preview))
            for q in ('6','-2'):
                path=root/f'bad-{q}.gpr'
                result=subprocess.run([str(args.sdk),'-i',str(root/'input.dng'),'-o',str(path),'-q',q],capture_output=True)
                self.assertNotEqual(result.returncode,0); self.assertFalse(path.exists())

    def test_sdk_bayer_formats_and_spatial_calibration(self):
        positions = {'RGGB': (0, 1, 2, 3), 'GBRG': (2, 0, 3, 1),
                     'GRBG': (1, 0, 3, 2), 'BGGR': (3, 1, 2, 0)}
        black_sites = {'RGGB': (0, 1, 2, 3), 'GBRG': (2, 3, 0, 1),
                       'GRBG': (1, 0, 3, 2), 'BGGR': (3, 2, 1, 0)}
        enum = {'RGGB': (0, 2, 5), 'GBRG': (3, 7, 6), 'GRBG': (8, 9, 10), 'BGGR': (11, 12, 13)}
        def gain_maps():
            records = []
            for site in range(4):
                area = struct.pack('>8I', site//2, site%2, 96, 128, 0, 1, 2, 2)
                grid = struct.pack('>2I4dI4f', 2, 2, 1., 1., 0., 0., 1,
                                   1.+site/8, 1.1+site/8, 1.2+site/8, 1.3+site/8)
                data = area+grid
                records.append(struct.pack('>4I', 9, 0x01030000, 1+(site%2)*2, len(data))+data)
            return struct.pack('>I', 4)+b''.join(records)
        def raw_page(f):
            def walk(pages):
                for page in pages:
                    if 33422 in page.tags: return page
                    if page.pages:
                        child = walk(page.pages)
                        if child is not None: return child
            result = walk(f.pages)
            self.assertIsNotNone(result)
            return result
        y, x = np.indices((96, 128), dtype=np.uint32)
        canonical14 = ((x*123+y*97+x*y*29+(x^y)*37)&16383).astype('<u2')
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); original = root/'reference.raw'; reference = root/'reference.decoded.raw'
            canonical14.tofile(original)
            fixture = Path(__file__).parent/'fixtures/stock_preview/128x96-q3.vc5'
            (root/'reference.vc5').write_bytes(fixture.read_bytes())
            run([str(args.decoder), '--decode', str(root/'reference.vc5'), '--output', str(reference),
                 '--width', '128', '--height', '96'])
            decoded14 = np.frombuffer(reference.read_bytes(), dtype='<u2').reshape(96, 128)
            for bits_index, bits in enumerate((12, 14, 16)):
              for phase, sites in positions.items():
                with self.subTest(bits=bits, phase=phase):
                    case = root/f'{phase}{bits}'; case.mkdir()
                    raw = np.empty_like(canonical14)
                    for c, site in enumerate(sites):
                        raw[site//2::2, site%2::2] = (canonical14[c//2::2, c%2::2]>>2) << (bits-12)
                    meta = metadata(bits=bits, cfa=phase)
                    # Distinct matrices/illuminants, orientation and per-site gains.
                    meta['image'] += [media.record(274, 3, 1, struct.pack('<H', 6)),
                                      media.record(51041, 12, 2, struct.pack('<2d', .001, .0001))]
                    if bits != 12:
                        matrix2 = (12,10, -1,10, 0,1, 0,1, 11,10, -1,10, -1,10, 0,1, 11,10)
                        meta['image'] += [media.record(50722, 10, 9, struct.pack('<18i', *matrix2)),
                                          media.record(50779, 3, 1, struct.pack('<H', 17))]
                    maps = gain_maps()
                    meta['image'] = [t for t in meta['image'] if t['code'] != 51009]
                    meta['image'].append(media.record(51009, 7, len(maps), maps))
                    source, compressed = case/'source.dng', case/'encoded.gpr'
                    media.write(source, meta, raw.tobytes())
                    run([str(args.sdk), '-i', str(source), '-o', str(case/'source.raw')])
                    self.assertEqual((case/'source.raw').read_bytes(), raw.tobytes())
                    run([str(args.sdk), '-i', str(source), '-o', str(compressed)])
                    run([str(args.sdk), '-i', str(compressed), '-o', str(case/'decoded.raw')])
                    run([str(args.sdk), '-i', str(compressed), '-o', str(case/'legacy.ppm'), '-r', '4:1'])
                    ppm=(case/'legacy.ppm').read_bytes()
                    self.assertTrue(ppm.startswith(b'P6\n32 24\n255\n'))
                    self.assertEqual(len(ppm.split(b'\n',3)[3]),32*24*3)
                    decoded = np.frombuffer((case/'decoded.raw').read_bytes(), dtype='<u2').reshape(96, 128)
                    for c, site in enumerate(sites):
                        actual = decoded[site//2::2, site%2::2].astype(np.uint32)
                        expected = decoded14[c//2::2, c%2::2].astype(np.uint32)
                        # The 16-bit inverse curve retains two bits omitted by the 14-bit reference.
                        if bits == 16: actual >>= 2
                        else: expected >>= 14-bits
                        self.assertTrue(np.array_equal(actual, expected), (bits, phase, c))
                    run([str(args.sdk), '-i', str(compressed), '-o', str(case/'editable.dng')])
                    run([str(args.sdk), '-i', str(source), '-o', str(case/'rewritten.dng')])
                    for path in (compressed, case/'editable.dng', case/'rewritten.dng'):
                        with tifffile.TiffFile(path) as f:
                            page = raw_page(f)
                            tags = {t.code: t.value for p in (f.pages[0], page) for t in p.tags.values()}
                            self.assertEqual(tags[33422], bytes('RGB'.index(ch) for ch in phase))
                            black = np.array(tags[50714]).reshape(-1,2)
                            self.assertTrue(np.array_equal(black[:,0]/black[:,1], (16,17,18,19)))
                            self.assertEqual(tags[50717], (1<<bits)-1)
                            self.assertEqual(int(tags[274]), 6)
                            self.assertEqual(tags[50778], 21)
                            matrix1 = np.array(tags[50721]).reshape(-1,2)
                            self.assertTrue(np.array_equal(matrix1[:,0]/matrix1[:,1], np.eye(3).reshape(-1)))
                            neutral = np.array(tags[50728]).reshape(-1,2)
                            self.assertTrue(np.allclose(neutral[:,0]/neutral[:,1], (.5,1.,2/3), atol=1e-6))
                            self.assertEqual(tags[51009], maps)
                            self.assertTrue(np.allclose(tags[51041], (.001, .0001), rtol=0, atol=1e-12))
                            if bits == 12: self.assertNotIn(50722, tags)
                            else:
                                self.assertEqual(tags[50779], 17)
                                m = np.array(tags[50722]).reshape(-1,2)
                                self.assertTrue(np.allclose(m[:,0]/m[:,1], np.array(matrix2)[::2]/np.array(matrix2)[1::2], atol=1e-6))
                            if path.suffix == '.gpr':
                                offset, size = page.dataoffsets[0], page.databytecounts[0]
                                self.assertEqual(path.read_bytes()[offset:offset+size], (root/'reference.vc5').read_bytes())
                            else:
                                expected = decoded if path.name == 'editable.dng' else raw
                                self.assertTrue(np.array_equal(page.asarray(), expected))
                        dumped = run([str(args.sdk), '-i', str(path), '-d', '1']).stdout
                        params = json.loads(dumped[dumped.index('{'):dumped.rindex('}')+1])
                        tuning = params['tuning_info']
                        self.assertEqual(tuning['pixel_format'], enum[phase][bits_index])
                        self.assertEqual(tuple(tuning['static_black_level'][k] for k in ('r_black','g_r_black','g_b_black','b_black')),
                                         tuple(16+i for i in black_sites[phase]))
                    # Raw CLI selections use the declared domain, independent of output extension.
                    raw.tofile(case/'input.raw')
                    for extension in ('dng', 'gpr'):
                        path = case/f'cli.{extension}'
                        run([str(args.sdk), '-i', str(case/'input.raw'), '-o', str(path),
                             '-w', '128', '-h', '96', '-x', phase.lower()+str(bits)])
                        with tifffile.TiffFile(path) as f:
                            page = raw_page(f)
                            self.assertEqual(page.tags[50717].value, (1<<bits)-1)
                            self.assertEqual(page.tags[33422].value, bytes('RGB'.index(ch) for ch in phase))
                            if extension == 'dng': self.assertTrue(np.array_equal(page.asarray(),raw))
                    padded = np.pad(raw, ((0,0),(0,8)), constant_values=0xCDEF)
                    padded.tofile(case/'padded.raw')
                    run([str(args.sdk), '-i', str(case/'padded.raw'), '-o', str(case/'padded.dng'),
                         '-w', '128', '-h', '96', '-p', '272', '-x', phase.lower()+str(bits)])
                    with tifffile.TiffFile(case/'padded.dng') as f:
                        self.assertTrue(np.array_equal(raw_page(f).asarray(),raw))

    def test_sdk_packed_input_and_metadata_rejection(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            raw = (np.arange(128*96,dtype=np.uint16).reshape(96,128)*13)&4095
            packed = np.empty((96,192),dtype=np.uint8)
            packed[:,0::3] = raw[:,0::2]&255
            packed[:,1::3] = (raw[:,0::2]>>8)|((raw[:,1::2]&15)<<4)
            packed[:,2::3] = raw[:,1::2]>>4
            packed.tofile(root/'packed.raw')
            for phase in ('rggb','gbrg'):
                for extension in ('dng','gpr'):
                    path = root/f'{phase}.{extension}'
                    run([str(args.sdk), '-i', str(root/'packed.raw'), '-o', str(path),
                         '-w', '128', '-h', '96', '-x', phase+'12p'])
                    if extension == 'dng':
                        with tifffile.TiffFile(path) as f: self.assertTrue(np.array_equal(f.asarray(),raw))
                    else:
                        run([str(args.sdk), '-i', str(path), '-o', str(root/f'{phase}.raw')])
                        self.assertEqual(len((root/f'{phase}.raw').read_bytes()),128*96*2)
            for kind in ('cfa','white','fractional-black','gain-map-count'):
                meta = metadata()
                if kind == 'white':
                    meta['image'] = [t for t in meta['image'] if t['code']!=50717]
                    meta['image'].append(media.record(50717,4,1,struct.pack('<I',15000)))
                if kind == 'fractional-black':
                    meta['image'] = [t for t in meta['image'] if t['code']!=50714]
                    meta['image'].append(media.record(50714,5,4,struct.pack('<8I',33,2,17,1,18,1,19,1)))
                if kind == 'gain-map-count':
                    body=struct.pack('>8I2I4dIf',0,0,96,128,0,1,1,1,1,1,1.,1.,0.,0.,1,1.)
                    opcode=struct.pack('>5I',1,9,0x01030000,1,len(body))+body
                    meta['image']=[t for t in meta['image'] if t['code']!=51009]
                    meta['image'].append(media.record(51009,7,len(opcode),opcode))
                path = root/f'{kind}.dng'; media.write(path,meta,raw.tobytes())
                if kind == 'cfa':
                    with tifffile.TiffFile(path) as f: offset=f.pages[0].tags[33422].valueoffset
                    data=bytearray(path.read_bytes()); data[offset:offset+4]=bytes((0,1,2,1)); path.write_bytes(data)
                output=root/f'{kind}-rejected.gpr'
                result=subprocess.run([str(args.sdk),'-i',str(path),'-o',str(output)],capture_output=True,text=True)
                self.assertNotEqual(result.returncode,0)
                self.assertFalse(output.exists())
                self.assertNotIn('Sanitizer',result.stderr)
            # A single black value repeats at every physical Bayer site.
            meta=metadata(cfa='GBRG',bits=12)
            meta['image']=[t for t in meta['image'] if t['code'] not in (50713,50714)]
            meta['image'] += [media.record(50713,3,2,struct.pack('<2H',1,1)),
                              media.record(50714,5,1,struct.pack('<2I',16,1))]
            media.write(root/'uniform.dng',meta,raw.tobytes())
            run([str(args.sdk),'-i',str(root/'uniform.dng'),'-o',str(root/'uniform-copy.dng')])
            with tifffile.TiffFile(root/'uniform-copy.dng') as f:
                black=np.array(f.pages[0].tags[50714].value).reshape(-1,2)
                self.assertTrue(np.array_equal(black[:,0]/black[:,1],(16,16,16,16)))

    def test_sdk_public_wrapping_apis(self):
        bits = (12,12,14,12,12,16,16,14,12,14,16,12,14,16)
        phases = ('RGGB','RGGB','RGGB','GBRG','GBRG','RGGB','GBRG','GBRG','GRBG','GRBG','GRBG','BGGR','BGGR','BGGR')
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); fixture = Path(__file__).parent/'fixtures/stock_preview/128x96-q3.vc5'
            for index in range(14):
                with self.subTest(format=index):
                    gpr, dng = root/f'{index}.gpr', root/f'{index}.dng'
                    run([str(args.sdk_probe), str(index), str(fixture), str(gpr), str(dng)])
                    for path in (gpr,dng):
                        with tifffile.TiffFile(path) as f:
                            page = f.pages[0]
                            if 33422 not in page.tags: page = page.pages[0]
                            self.assertEqual(page.compression, 9 if path==gpr else 1)
                            self.assertEqual(page.tags[33422].value, bytes('RGB'.index(ch) for ch in phases[index]))
                            self.assertEqual(page.tags[50717].value, (1<<bits[index])-1)
                            if path==gpr:
                                offset,length=page.dataoffsets[0],page.databytecounts[0]
                                self.assertEqual(path.read_bytes()[offset:offset+length],fixture.read_bytes())
                            else: self.assertEqual(page.asarray().shape,(96,128))

    def test_big_endian_subifd_and_global_calibration(self):
        m = metadata()
        def extra(tag):
            raw, dtype = media.unpack_record(tag), tag['type']
            if dtype in (1, 7): data = raw
            elif dtype == 2: data = raw.rstrip(b'\0').decode('ascii')
            else:
                fmt = {3: 'H', 4: 'I', 5: 'I', 10: 'i'}[dtype]
                data = struct.unpack('<'+fmt*(tag['count']*(2 if dtype in (5, 10) else 1)), raw)
            return (tag['code'], dtype, tag['count'], data, False)
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)/'subifd.dng'
            with tifffile.TiffWriter(path, byteorder='>') as f:
                f.write(np.zeros((4, 4, 3), dtype=np.uint8), photometric='rgb', subifds=1,
                        metadata=None, extratags=[extra(t) for t in m['image'] if t['code'] in media.GLOBAL_TAGS])
                f.write(np.zeros((96, 128), dtype=np.uint16), photometric=32803, metadata=None,
                        extratags=[extra(t) for t in m['image'] if t['code'] not in media.GLOBAL_TAGS])
            result = media.extract(path, 14)
            self.assertEqual((result['width'], result['height'], result['cfa']), (128, 96, 'RGGB'))
            self.assertEqual({t['code']: media.unpack_record(t) for t in result['image']},
                             {t['code']: media.unpack_record(t) for t in m['image']})

    def test_all_bayer_phases_and_bit_domains(self):
        for bits in (12, 14, 16):
            for phase in ('RGGB', 'GBRG', 'GRBG', 'BGGR'):
                with self.subTest(bits=bits, phase=phase), tempfile.TemporaryDirectory() as temp:
                    path = Path(temp) / 'editable.dng'; meta = metadata(bits=bits, cfa=phase)
                    raw = ((np.arange(128*96, dtype=np.uint32).reshape(96, 128)*13) % (1 << bits)).astype('<u2')
                    media.write(path, meta, raw.tobytes())
                    with tifffile.TiffFile(path) as f:
                        page = f.pages[0]
                        self.assertTrue(np.array_equal(page.asarray(), raw))
                        self.assertEqual(page.tags[33422].value, bytes('RGB'.index(x) for x in phase))
                        self.assertEqual(page.tags[50714].value, (16, 1, 17, 1, 18, 1, 19, 1))
                        self.assertEqual(page.tags[34665].value['ExposureTime'], (1, 120))
                    recovered = media.extract(path, bits)
                    for group in ('image', 'exif', 'gps'):
                        self.assertEqual({t['code']: media.unpack_record(t) for t in recovered[group]},
                                         {t['code']: media.unpack_record(t) for t in meta[group]})
                    with self.assertRaises(FileExistsError): media.write(path, meta, raw.tobytes())

    def test_invalid_calibration(self):
        variants = []
        m = metadata(); m['image'].append(media.record(50710,1,3,b'\2\1\0')); variants.append(m)
        m = metadata(); m['image'].append(media.record(50711,3,1,struct.pack('<H',2))); variants.append(m)
        m = metadata(); m['image'] += [m['image'][0]]; variants.append(m)
        m = metadata(); m['image'][6]['data'] += 'AAAA'; variants.append(m)
        m = metadata(); m['cfa'] = 'BGGR'; variants.append(m)
        m = metadata(); m['image'][9] = media.record(50728, 5, 3, struct.pack('<6I', 1, 0, 1, 1, 1, 1)); variants.append(m)
        m = metadata(); m['image'][10] = media.record(50721, 10, 9, struct.pack('<18i', *(0, 1)*9)); variants.append(m)
        m = metadata(); m['image'].append(media.record(50829, 4, 4, struct.pack('<4I', 0, 0, 100, 128))); variants.append(m)
        m = metadata(); m['image'].append(media.record(273, 4, 1, bytes(4))); variants.append(m)
        m = metadata(); m['exif'].append(media.record(37500, 7, 4, bytes(4))); variants.append(m)
        for m in variants:
            with self.subTest(m=m), self.assertRaises((ValueError, TypeError)): media.validate(m)
        with self.assertRaises(ValueError): media.validate(metadata(), dict(width=72, height=96, sample_bits=14, bayer='RGGB'))
        with tempfile.TemporaryDirectory() as temp, self.assertRaises(ValueError):
            media.write(Path(temp)/'bad.dng', metadata(bits=12), np.full((96, 128), 4096, dtype='<u2').tobytes())

    def test_independent_stock_payload_sdk_and_dng(self):
        for width in (128, 72):
            with self.subTest(width=width), tempfile.TemporaryDirectory() as temp:
                root = Path(temp); meta = metadata(width=width)
                source = Path(__file__).parent / f'fixtures/stock_preview/{width}x96-q3.vc5'
                gpr, raw, ref, dng = (root / name for name in ('editable.gpr', 'decoded.raw', 'reference.raw', 'editable.dng'))
                media.write(gpr, meta, source.read_bytes(), 'gpr')
                dumped = run([str(args.sdk), '-i', str(gpr), '-d', '1']).stdout
                params = json.loads(dumped[dumped.index('{'):dumped.rindex('}')+1])
                self.assertEqual(params['exif_info']['light_source'], 14)
                self.assertEqual(params['exif_info']['flash'], 0xFFFFFFFF)
                with tifffile.TiffFile(gpr) as f:
                    page = f.pages[0]; self.assertEqual(page.compression, 9)
                    offset, length = page.dataoffsets[0], page.databytecounts[0]
                    self.assertEqual(gpr.read_bytes()[offset:offset+length], source.read_bytes())
                run([str(args.sdk), '-i', str(gpr), '-o', str(raw)])
                run([str(args.decoder), '--decode', str(source), '--output', str(ref),
                     '--width', str(width), '--height', '96'])
                self.assertEqual(raw.read_bytes(), ref.read_bytes())
                media.write(dng, meta, raw.read_bytes())
                with tifffile.TiffFile(dng) as f:
                    self.assertEqual(f.asarray().astype('<u2').tobytes(), ref.read_bytes())
                roundtrip = root / 'sdk-dng.raw'
                run([str(args.sdk), '-i', str(dng), '-o', str(roundtrip)])
                self.assertEqual(roundtrip.read_bytes(), ref.read_bytes())
                failed = subprocess.run([str(args.sdk), '-i', str(gpr), '-o', str(root/'missing'/'out.raw')], capture_output=True, text=True)
                self.assertNotEqual(failed.returncode, 0)
                self.assertNotIn('Sanitizer', failed.stderr)
                wrong = metadata(width=width+8)
                with self.assertRaises(ValueError): media.write(root/'bad.gpr', wrong, source.read_bytes(), 'gpr')

    def test_capture_metadata_and_editable_sequence(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); clip = root/'capture.gvid'
            metas = [metadata(), metadata()]
            metas[1]['image'][9] = media.record(50728, 5, 3, struct.pack('<6I', 3, 4, 1, 1, 1, 2))
            command = [str(args.capture), '--output', str(clip), '--width', '128', '--height', '96',
                       '--quality', '3', '--frames', '4', '--workspace-mib', '16', '--leased', '--submit-timeout-ms', '10000']
            for i, m in enumerate(metas):
                raw, calibration = root/f'{i}.raw', root/f'{i}.json'
                (np.arange(128*96, dtype=np.uint16)+i).astype('<u2').tofile(raw)
                calibration.write_text(json.dumps(m))
                command += ['--input', str(raw), '--source-metadata', str(calibration)]
            subprocess.run(command, check=True, capture_output=True)
            with clip.open('rb') as f:
                h = stock_video.read_header(f); rows = list(stock_video.records(f, h))
            self.assertTrue(h['_end_marker_verified']); self.assertEqual(len(rows), 4)
            for i, row in enumerate(rows): self.assertEqual(row['metadata']['raw_metadata'], metas[i % 2])
            out = root/'editable'
            tool = Path(__file__).resolve().parents[1]/'tools/stock_video.py'
            base = [sys.executable, str(tool), 'extract', str(clip), '--decoder', str(args.decoder), '--editable', 'both']
            exported = subprocess.run(base+['--out-dir', str(out)], capture_output=True, text=True)
            self.assertEqual(exported.returncode, 0, exported.stderr)
            receipt = json.loads((out/'receipt.json').read_text())
            self.assertTrue(receipt['complete']); self.assertTrue(receipt['end_marker_verified'])
            for i, row in enumerate(rows):
                gpr, dng = out/f'{i:06d}.gpr', out/f'{i:06d}.dng'
                with tifffile.TiffFile(gpr) as f:
                    page = f.pages[0]; start, count = page.dataoffsets[0], page.databytecounts[0]
                    self.assertEqual(gpr.read_bytes()[start:start+count], row['payload'])
                    self.assertEqual(page.tags[50728].value, struct.unpack('<6I', media.unpack_record(metas[i % 2]['image'][9])))
                with tifffile.TiffFile(dng) as f:
                    self.assertEqual(f.asarray().astype('<u2').tobytes(), (out/f'{i:06d}.raw').read_bytes())
            bad = subprocess.run(command[:-2]+['--source-metadata', str(root/'0.json'), '--source-metadata', str(root/'1.json')], capture_output=True)
            self.assertNotEqual(bad.returncode, 0)
            metas[1]['width'] += 8
            (root/'1.json').write_text(json.dumps(metas[1]))
            bad_clip = root/'bad-calibration.gvid'
            bad_command = [str(bad_clip) if arg == str(clip) else arg for arg in command]
            run(bad_command)
            failed = subprocess.run([sys.executable, str(tool), 'extract', str(bad_clip), '--limit', '1',
                                     '--out-dir', str(root/'partial'), '--editable', 'gpr'], capture_output=True, text=True)
            self.assertNotEqual(failed.returncode, 0)
            self.assertIn('recording domains disagree', failed.stderr)


if __name__ == '__main__':
    p = argparse.ArgumentParser(); p.add_argument('--sdk', type=Path, required=True); p.add_argument('--decoder', type=Path, required=True)
    p.add_argument('--capture', type=Path, required=True)
    p.add_argument('--sdk-probe', type=Path, required=True)
    args = p.parse_args(); args.sdk = args.sdk.resolve(); args.decoder = args.decoder.resolve(); args.capture = args.capture.resolve()
    unittest.main(argv=[sys.argv[0]])
