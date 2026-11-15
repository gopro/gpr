#!/usr/bin/env python3
"""Pack stock_still-produced GPRs into GVID without another image encode.

Input order is explicit. Timestamps are synthesized from the requested rate;
they are not sensor capture times. Every input needs its stock_still receipt.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import zlib

import raw_media
import stock_still as still
import stock_video as video


def metadata_bytes(value):
    data=json.dumps(value,sort_keys=True,separators=(',',':'),allow_nan=False).encode()
    if len(data)>65536: raise ValueError('GVID metadata exceeds 65536 bytes')
    return data


def checked_frame(path, bits, quality):
    path=Path(path).resolve()
    receipt=json.loads(path.with_suffix(path.suffix+'.json').read_text())
    source_sha=still.digest(path)
    meta,payload=raw_media.extract(path,bits,with_payload=True)
    if (receipt.get('schema')!='gpr.stock_still.v1' or receipt.get('action')!='encode'
            or type(receipt.get('quality')) is not int or receipt['quality']!=quality
            or receipt.get('sample_bits')!=bits or receipt.get('output_sha256')!=source_sha
            or receipt.get('payload_sha256')!=hashlib.sha256(payload).hexdigest()
            or receipt.get('metadata_sha256')!=hashlib.sha256(still.canonical(meta)).hexdigest()):
        raise ValueError('GPR and stock encoding receipt disagree')
    return meta,payload,source_sha


def pack(inputs, output, quality, sample_bits=14, fps_num=24, fps_den=1, receipt=None):
    if not inputs: raise ValueError('requires at least one GPR')
    if (type(quality) is not int or not 0<=quality<=5 or sample_bits not in (12,14,16)
            or any(type(v) is not int for v in (fps_num,fps_den))
            or not 0<fps_num<=240000 or not 0<fps_den<=10000 or fps_num>240*fps_den):
        raise ValueError('invalid stock preset, count domain or frame rate')
    output,receipt,_=still.outputs(output,receipt,None)
    with tempfile.TemporaryDirectory(prefix='.stock-sequence-',dir=output.parent) as temp:
        staged=Path(temp)/'sequence.gvid'
        first,_,_=checked_frame(inputs[0],sample_bits,quality)
        w,h=first['width'],first['height']
        if not (0<w<=8192 and 0<h<=6144 and w%8==h%8==0):
            raise ValueError('GVID requires dimensions divisible by eight within 8192x6144; no implicit crop')
        clip_meta=metadata_bytes(dict(schema='gpr.stock_sequence.v1',timestamp_source='synthetic_nominal_rate'))
        reserved=struct.pack('<5I',len(clip_meta),1,zlib.crc32(clip_meta),0,0)
        header=video.CLIP.pack(b'GVID',3,64,b'VC5\0',w,h,fps_num,fps_den,quality,
                               first['cfa'].encode(),sample_bits,48,reserved)
        header=header[:56]+struct.pack('<I',zlib.crc32(header[:56]))+header[60:]
        crc=0;total=0;rows=[]
        with staged.open('wb') as file:
            def write(data):
                nonlocal crc
                file.write(data);crc=zlib.crc32(data,crc)
            write(header);write(clip_meta)
            for index,path in enumerate(inputs):
                meta,payload,source_sha=checked_frame(path,sample_bits,quality)
                if (meta['width'],meta['height'],meta['cfa'])!=(w,h,first['cfa']):
                    raise ValueError('sequence geometry or CFA differs')
                if not 0<len(payload)<=w*h*8: raise ValueError('invalid stock payload length')
                frame_meta=metadata_bytes(dict(raw_metadata=meta,source_gpr_sha256=source_sha))
                pts=index*1_000_000_000*fps_den//fps_num
                record=video.RECORD3.pack(b'FRM3',48,index,pts,len(payload),len(frame_meta),
                                         zlib.crc32(payload),zlib.crc32(frame_meta),0)
                record=record[:44]+struct.pack('<I',zlib.crc32(record[:44]))
                write(record);write(frame_meta);write(payload);total+=len(payload)
                rows.append(dict(index=index,source_sha256=source_sha,payload_sha256=hashlib.sha256(payload).hexdigest(),
                                 metadata_sha256=hashlib.sha256(still.canonical(meta)).hexdigest()))
            end=video.RECORD3.pack(b'END3',48,len(rows),pts,total,crc,0,0,0)
            file.write(end[:36]+struct.pack('<I',zlib.crc32(end[:36]))+end[40:])
        with staged.open('rb') as file:
            verified=video.read_header(file);count=0
            for frame in video.records(file,verified):
                row=rows[count];count+=1
                raw_media.validate(frame['metadata']['raw_metadata'],verified)
                if (hashlib.sha256(frame['payload']).hexdigest()!=row['payload_sha256'] or
                    hashlib.sha256(still.canonical(frame['metadata']['raw_metadata'])).hexdigest()!=row['metadata_sha256']):
                    raise ValueError('packed sequence verification differs')
            if count!=len(rows) or not verified['_end_marker_verified']: raise ValueError('incomplete packed sequence')
        result=dict(schema='gpr.stock_sequence.v1',frames=rows,quality=quality,sample_bits=sample_bits,
                    width=w,height=h,cfa=first['cfa'],fps_num=fps_num,fps_den=fps_den,reencoded=False,
                    timestamp_source='synthetic_nominal_rate',output_sha256=still.digest(staged))
        log=Path(temp)/'receipt.json';log.write_text(json.dumps(result,indent=2)+'\n')
        still.publish([(staged,output),(log,receipt)])
        return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('inputs',nargs='+',type=Path)
    parser.add_argument('--output',required=True,type=Path)
    parser.add_argument('--quality',required=True,type=int)
    parser.add_argument('--sample-bits',type=int,default=14)
    parser.add_argument('--fps-num',type=int,default=24)
    parser.add_argument('--fps-den',type=int,default=1)
    parser.add_argument('--receipt',type=Path)
    args=parser.parse_args()
    print(json.dumps(pack(**vars(args)),indent=2))


if __name__=='__main__': main()
