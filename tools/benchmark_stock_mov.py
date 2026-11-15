#!/usr/bin/env python3
"""Measure verified offline transport; independently inspect all MOV packets.

No image decoding or CNN is timed. The input must be a complete stock GVID.
Publication/fsync are outside the wrapper's preparation timing endpoints.
"""
import argparse
import hashlib
import json
from pathlib import Path
import statistics
import subprocess

import stock_mov
import stock_still
import stock_video


def benchmark(source,bridge,ffprobe,output_dir,repeats=3):
    if not 1<=repeats<=20:raise ValueError('repeats must be 1..20')
    source=Path(source).resolve()
    if not source.is_file():raise ValueError('benchmark input must be a regular recording file')
    root=Path(output_dir).resolve();root.mkdir(parents=True,exist_ok=False)
    source_sha=stock_still.digest(source);attempts=[]
    for repeat in range(repeats):
        movie=root/f'wrap-{repeat}.mov';returned=root/f'returned-{repeat}.gvid'
        wrap=stock_mov.convert('wrap',source,bridge,movie)
        unwrap=stock_mov.convert('unwrap',movie,bridge,returned)
        if wrap['gvid_sha256']!=source_sha or unwrap['gvid_sha256']!=source_sha:
            raise ValueError('transport changed input recording')
        attempts.append(dict(wrap=wrap,unwrap=unwrap))
    movie=root/'wrap-0.mov'
    probe=json.loads(subprocess.check_output([str(ffprobe),'-v','error','-show_streams','-show_packets',
                                             '-show_data_hash','sha256','-of','json',str(movie)],text=True))
    packets=iter(probe['packets']);rows=[];first=None;unit=attempts[0]['wrap']['mov_tick_ns']
    with source.open('rb') as file:
        header=stock_video.read_header(file);start=file.tell()
        for row in stock_video.records(file,header):
            packet=next(packets,None)
            if packet is None:raise ValueError('ffprobe packet count is too small')
            end=file.tell();file.seek(start);record=file.read(end-start);file.seek(end);start=end
            if first is None:first=row['pts_ns']
            digest=hashlib.sha256(record).hexdigest()
            if (packet['data_hash'].lower()!='sha256:'+digest or
                int(packet['pts'])!=(row['pts_ns']-first+unit//2)//unit or not packet['flags'].startswith('K')):
                raise ValueError('independent MOV packet data/timing differs')
            rows.append(dict(index=row['index'],pts_ns=row['pts_ns'],record_sha256=digest,
                             payload_sha256=hashlib.sha256(row['payload']).hexdigest()))
    if next(packets,None) is not None:raise ValueError('ffprobe has extra packets')
    stream=probe['streams'][0]
    if (len(probe['streams'])!=1 or stream['codec_tag_string']!=attempts[0]['wrap']['profile'] or
        (stream['width'],stream['height'])!=(header['width'],header['height']) or
        stream['time_base']!='1/'+str(1000000000//unit)):
        raise ValueError('independent MOV stream description differs')
    repo=Path(__file__).resolve().parents[1]
    names=('tools/benchmark_stock_mov.py','tools/stock_mov.py','tools/stock_video.py','tools/stock_still.py',
           'tools/raw_media.py','source/app/gpr_mov.cpp','source/lib/gpr_video/gpr_gvid.c',
           'source/lib/gpr_video/gpr_gvid_reader.cpp','source/lib/gpr_video/gpr_gvid_reader.h','source/lib/gpr_video/gpr_gvid.h')
    result=dict(schema='gpr.stock_mov_benchmark.v1',complete=True,source_path=str(source),source_sha256=source_sha,
                source_bytes=source.stat().st_size,frames=rows,attempts=attempts,
                median_wrap_s=statistics.median(a['wrap']['elapsed_before_publication_s'] for a in attempts),
                median_unwrap_s=statistics.median(a['unwrap']['elapsed_before_publication_s'] for a in attempts),
                ffprobe_packets_timestamps_and_description_verified=True,
                native_version=subprocess.check_output([str(bridge),'--version'],text=True).strip(),
                ffprobe_version=subprocess.check_output([str(ffprobe),'-version'],text=True).splitlines()[0],
                source_files={name:stock_still.digest(repo/name) for name in names},
                timing_scope='Warm-cache offline wrapper with validation, hashes, native I/O and inverse readback; no image decode/CNN, no final publication/fsync. Independent ffprobe verification is outside timings.')
    (root/'benchmark.json').write_text(json.dumps(result,indent=2)+'\n')
    return result


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input',type=Path,required=True);parser.add_argument('--bridge',type=Path,required=True)
    parser.add_argument('--ffprobe',type=Path,required=True);parser.add_argument('--output-dir',type=Path,required=True)
    parser.add_argument('--repeats',type=int,default=3);args=parser.parse_args()
    result=benchmark(args.input,args.bridge.resolve(),args.ffprobe.resolve(),args.output_dir,args.repeats)
    print(json.dumps(dict(frames=len(result['frames']),wrap_s=result['median_wrap_s'],unwrap_s=result['median_unwrap_s'])))
