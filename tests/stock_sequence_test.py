#!/usr/bin/env python3
"""Sequence payload identity, native-reader interchange and rejection contracts."""
import argparse
import json
from pathlib import Path
import sys
import tempfile
import unittest

import numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import raw_media
import stock_mov
import stock_sequence
import stock_still
import stock_video
from raw_media_test import metadata


class Sequence(unittest.TestCase):
    def test_payloads_native_interchange_and_rejections(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);inputs=[]
            for index in range(3):
                raw=root/f'{index}.raw'
                ((np.arange(128*96,dtype=np.uint32)*73+index*997)%16384).astype('<u2').tofile(raw)
                gpr=root/f'{index}.gpr'
                stock_still.encode(raw,metadata(),3,args.sdk,gpr);inputs.append(gpr)
            clip=root/'sequence.gvid'
            report=stock_sequence.pack(inputs,clip,3,fps_num=30000,fps_den=1001)
            self.assertFalse(report['reencoded'])
            with clip.open('rb') as file:
                header=stock_video.read_header(file);frames=list(stock_video.records(file,header))
            self.assertTrue(header['_end_marker_verified'])
            for index,frame in enumerate(frames):
                meta,payload=raw_media.extract(inputs[index],14,with_payload=True)
                self.assertEqual(frame['payload'],payload)
                self.assertEqual(stock_still.canonical(frame['metadata']['raw_metadata']),stock_still.canonical(meta))
                self.assertEqual(frame['pts_ns'],index*1_000_000_000*1001//30000)
            movie=root/'sequence.mov'
            stock_mov.convert('wrap',clip,args.bridge,movie)
            recovered=root/'recovered.gvid'
            stock_mov.convert('unwrap',movie,args.bridge,recovered)
            self.assertEqual(recovered.read_bytes(),clip.read_bytes())
            with self.assertRaises(FileExistsError):stock_sequence.pack(inputs,clip,3)
            with self.assertRaises(ValueError):stock_sequence.pack(inputs,root/'wrong-q.gvid',2)
            self.assertFalse((root/'wrong-q.gvid').exists())
            receipt=inputs[1].with_suffix('.gpr.json');saved=receipt.read_text()
            row=json.loads(saved);row['output_sha256']='0'*64;receipt.write_text(json.dumps(row))
            with self.assertRaises(ValueError):stock_sequence.pack(inputs,root/'mismatch.gvid',3)
            self.assertFalse((root/'mismatch.gvid').exists());receipt.write_text(saved)
            raw=root/'different.raw';np.zeros(136*96,dtype='<u2').tofile(raw)
            different=root/'different.gpr'
            stock_still.encode(raw,metadata(width=136),3,args.sdk,different)
            with self.assertRaises(ValueError):stock_sequence.pack(inputs+[different],root/'geometry.gvid',3)
            self.assertFalse((root/'geometry.gvid').exists())
            for rate in ((0,1),(241,1),(24,0)):
                with self.assertRaises(ValueError):stock_sequence.pack(inputs,root/'rate.gvid',3,fps_num=rate[0],fps_den=rate[1])


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--sdk',required=True,type=Path)
    parser.add_argument('--bridge',required=True,type=Path)
    args,remaining=parser.parse_known_args();unittest.main(argv=[sys.argv[0],*remaining])
