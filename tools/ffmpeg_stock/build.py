#!/usr/bin/env python3
"""Install/build the stock GVS2/GVS3 decoder in an explicit FFmpeg n8.0 checkout."""
import argparse
from pathlib import Path
import platform
import shutil
import subprocess

PIN='140fd653aed8cad774f991ba083e2d01e86420c7'


def install(root):
    if subprocess.check_output(['git','-C',str(root),'rev-parse','HEAD'],text=True).strip()!=PIN:
        raise ValueError('requires official FFmpeg n8.0 commit '+PIN)
    edits={}
    def add(path,anchor,insertion):
        path=root/path
        source=edits.get(path,path.read_text())
        if insertion.strip() in source:return
        if source.count(anchor)!=1:raise ValueError(f'unsupported patch anchor in {path}')
        edits[path]=source.replace(anchor,anchor+insertion,1)
    add(Path('libavcodec/Makefile'),
        'OBJS-$(CONFIG_CFHD_ENCODER)            += cfhdenc.o cfhddata.o cfhdencdsp.o\n',
        'OBJS-$(CONFIG_GPR_STOCK_DECODER)       += gpr_stock.o\n')
    add(Path('libavcodec/allcodecs.c'),'extern const FFCodec ff_cfhd_decoder;\n',
        'extern const FFCodec ff_gpr_stock_decoder;\n')
    add(Path('libavcodec/codec_id.h'),'    AV_CODEC_ID_PRORES_RAW,\n',
        '    AV_CODEC_ID_GPR_STOCK,\n')
    descriptor='''    {
        .id        = AV_CODEC_ID_GPR_STOCK,
        .type      = AVMEDIA_TYPE_VIDEO,
        .name      = "gpr_stock",
        .long_name = NULL_IF_CONFIG_SMALL("Stock GoPro RAW VC-5 framed GVID"),
        .props     = AV_CODEC_PROP_INTRA_ONLY | AV_CODEC_PROP_LOSSY,
    },

'''
    path=root/'libavcodec/codec_desc.c';source=path.read_text()
    if 'AV_CODEC_ID_GPR_STOCK' not in source:
        anchor='    /* various PCM "codecs" */'
        if source.count(anchor)!=1:raise ValueError('codec descriptor anchor changed')
        edits[path]=source.replace(anchor,descriptor+anchor,1)
    add(Path('libavformat/isom_tags.c'),"    { AV_CODEC_ID_CFHD, MKTAG('C', 'F', 'H', 'D') },\n",
        "    { AV_CODEC_ID_GPR_STOCK, MKTAG('G', 'V', 'S', '2') },\n"
        "    { AV_CODEC_ID_GPR_STOCK, MKTAG('G', 'V', 'S', '3') },\n")
    for path,source in edits.items():path.write_text(source)
    shutil.copyfile(Path(__file__).with_name('gpr_stock.c'),root/'libavcodec/gpr_stock.c')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action',choices=('install','build'))
    parser.add_argument('--ffmpeg-source',type=Path,required=True)
    parser.add_argument('--gpr-prefix',type=Path)
    parser.add_argument('--jobs',type=int,default=4)
    args=parser.parse_args();root=args.ffmpeg_source.resolve()
    if not 1<=args.jobs<=8:parser.error('--jobs must be 1..8')
    prefix=args.gpr_prefix.resolve() if args.gpr_prefix else None
    if args.action=='build' and (prefix is None or not (prefix/'lib/libgpr_preview.a').is_file()):
        parser.error('build requires an installed GPR capture/preview prefix')
    if args.action=='build':
        for header in ('vc5_preview.h','vc5_decoder.h','gpr_preview.h','gpr_gvid_reader.h'):
            if not (prefix/'include/gpr'/header).is_file():parser.error('incomplete/stale GPR prefix: missing '+header)
    install(root)
    if args.action=='install':return
    cpp='-lc++' if platform.system()=='Darwin' else '-lstdc++'
    command=['./configure','--disable-doc','--disable-network','--disable-autodetect',
             '--disable-everything','--disable-shared','--enable-static','--enable-zlib',
             '--enable-ffmpeg','--enable-ffprobe','--enable-demuxer=mov',
             '--enable-muxer=mov,rawvideo,null,image2,md5',
             '--enable-protocol=file,pipe','--enable-decoder=gpr_stock,rawvideo',
             '--enable-encoder=rawvideo,wrapped_avframe,png,prores_ks',
             '--enable-filter=scale,format,null,buffer,buffersink',
             '--extra-cflags=-I'+str(prefix/'include/gpr'),
             '--extra-ldflags=-L'+str(prefix/'lib'),
             '--extra-libs=-lgpr_preview -lvc5_decoder -lgpr_gvid -lvc5_common -lcommon -lz -lm -lpthread '+cpp]
    subprocess.run(command,cwd=root,check=True)
    subprocess.run(['make','-j'+str(args.jobs)],cwd=root,check=True)
    result=subprocess.check_output([str(root/'ffmpeg'),'-hide_banner','-decoders'],text=True)
    if ' gpr_stock ' not in result:raise RuntimeError('stock decoder was not registered')
    print('PASS: stock GVS2/GVS3 decoder registered in pinned FFmpeg n8.0')


if __name__=='__main__':main()
