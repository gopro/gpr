#!/usr/bin/env python3
"""Exact Bayer decoding through the optional patched FFmpeg and real stock MOV writer."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import platform

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import stock_video
from stock_mov_test import fixture


def run(command,okay=True):
    result=subprocess.run(list(map(str,command)),capture_output=True,timeout=120)
    if (result.returncode==0)!=okay:raise RuntimeError(f'{command}: {result.stderr.decode(errors="replace")[-4000:]}')
    return result


def require(condition,message):
    if not condition:raise RuntimeError(message)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('ffmpeg','ffprobe','bridge','capture','decoder'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--receipt',type=Path)
    args=parser.parse_args();receipts=[]
    def decode(movie,output,okay=True):
        return run([args.ffmpeg,'-v','error','-xerror','-i',movie,'-map','0:v:0','-fps_mode','passthrough',
                    '-c:v','rawvideo','-f','rawvideo','-y',output],okay)
    with tempfile.TemporaryDirectory(prefix='stock-ffmpeg-test-') as temporary:
        root=Path(temporary)
        for phase in ('RGGB','GBRG','GRBG','BGGR'):
            for bits in (12,14,16):
                name=f'{phase}-{bits}';source=root/(name+'.raw');clip=root/(name+'.gvid');movie=root/(name+'.mov')
                source.write_bytes(struct.pack('<'+'H'*(128*96),*((i*1337+(i//128)*997) % (1<<bits) for i in range(128*96))))
                run([args.capture,'--width',128,'--height',96,'--cfa',phase,'--sample-bits',bits,
                     '--quality',3,'--frames',2,'--input',source,'--output',clip,'--submit-timeout-ms',10000])
                run([args.bridge,'mux',clip,movie])
                payload=root/(name+'.vc5');expected=root/(name+'-expected.raw')
                with clip.open('rb') as file:
                    header=stock_video.read_header(file);frames=list(stock_video.records(file,header))
                payload.write_bytes(frames[0]['payload'])
                run([args.decoder,'--decode',payload,'--output',expected,'--width',128,'--height',96,'--cfa',phase,'--sample-bits',bits])
                output=root/(name+'-ffmpeg.raw');decode(movie,output)
                require(output.read_bytes()==expected.read_bytes()*2,'Bayer pixels differ: '+name)
                probe=json.loads(run([args.ffprobe,'-v','error','-show_streams','-of','json',movie]).stdout)['streams'][0]
                require(probe['codec_name']=='gpr_stock' and probe['pix_fmt']=='bayer_'+phase.lower()+'16le','wrong codec/CFA: '+name)
                require((probe['width'],probe['height'],int(probe['bits_per_raw_sample']))==(128,96,bits),'wrong geometry/depth: '+name)
                receipts.append(dict(cfa=phase,sample_bits=bits,frames=2,raw_sha256=hashlib.sha256(output.read_bytes()).hexdigest()))
        # Frozen independent VC-5 fixture, both supported container generations.
        frozen=root/'frozen.raw'
        run([args.decoder,'--decode',ROOT/'tests/fixtures/stock_preview/128x96-q3.vc5',
             '--output',frozen,'--width',128,'--height',96,'--cfa','RGGB','--sample-bits',14])
        for version in (2,3):
            clip=root/f'v{version}.gvid';movie=root/f'v{version}.mov';clip.write_bytes(fixture(version))
            run([args.bridge,'mux',clip,movie]);decode(movie,root/f'v{version}.raw')
            require((root/f'v{version}.raw').read_bytes()==frozen.read_bytes()*3,'frozen fixture pixels differ')
            if version==3:
                original=movie.read_bytes();record=original.index(b'FRM3');prefix=original.index(b'GVID')
                for label,offset in [('record-crc',record+44),('prefix-crc',prefix+56),('payload',record+48+struct.unpack_from('<I',original,record+32)[0]+80)]:
                    bad=bytearray(original);bad[offset]^=1;path=root/(label+'.mov');path.write_bytes(bad)
                    decode(path,root/(label+'.raw'),okay=False)
                run([args.ffmpeg,'-v','error','-i',movie,'-frames:v',1,root/'preview.png'])
                run([args.ffmpeg,'-v','error','-i',movie,'-frames:v',1,'-vf','scale,format=yuv422p10le','-c:v','prores_ks',root/'preview-prores.mov'])
        report=dict(schema='gpr.stock_ffmpeg.v1',platform=platform.platform(),formats=receipts,versions=[2,3],negative_cases=3,
                    png=True,prores=True,all_bayer_pixels_exact=True,
                    ffmpeg_sha256=hashlib.sha256(args.ffmpeg.read_bytes()).hexdigest(),
                    binaries={name:hashlib.sha256(getattr(args,name).read_bytes()).hexdigest()
                              for name in ('ffmpeg','ffprobe','bridge','capture','decoder')})
        if args.receipt:
            with args.receipt.open('x') as file:file.write(json.dumps(report,indent=2)+'\n')
        print(json.dumps(report))


if __name__=='__main__':main()
