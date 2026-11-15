#!/usr/bin/env python3
"""Lossless stock GVID/MOV interchange with private GVS2/GVS3 sample profiles.

Each MOV sample contains one original framed record, including its calibration
and absolute nanosecond timestamp. This is distinct from CFHD and fused GPRr/
GPR1. The optional native FFmpeg bridge does not encode or decode image data.
"""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import time

import raw_media
import stock_still
import stock_video
import stock_av


def recording(path):
    count=0;calibrated=0;first=last=None
    with Path(path).open('rb') as file:
        header=stock_video.read_header(file)
        clip=header.get('metadata',{})
        if 'raw_metadata' in clip:raw_media.validate(clip['raw_metadata'],header)
        for row in stock_video.records(file,header):
            meta=row['metadata']
            if 'raw_metadata' in meta:raw_media.validate(meta['raw_metadata'],header)
            if 'raw_metadata' in meta or 'raw_metadata' in clip:calibrated+=1
            if first is None:first=row['pts_ns']
            last=row['pts_ns'];count+=1
        if not count:raise ValueError('MOV interchange requires at least one frame')
    return dict(video={k:header[k] for k in ('width','height','fps_num','fps_den','quality','bayer','sample_bits','version')},
                frames=count,calibrated_frames=calibrated,first_timestamp_ns=first,last_timestamp_ns=last,
                complete=header['_complete'],end_marker_verified=header['_end_marker_verified'])


def run(bridge,action,source,output):
    result=subprocess.run([str(Path(bridge).resolve()),action,str(source),str(output)],capture_output=True,text=True)
    if result.returncode:raise RuntimeError(f'gpr_mov {action} failed: {result.stderr[-8000:]}')
    return json.loads(result.stdout)


def convert(action,source,bridge,output,receipt=None,audio=None,timecode=None,ffmpeg=None,probe=None,audio_output=None):
    if action not in ('wrap','unwrap'):raise ValueError('action must be wrap or unwrap')
    start=time.perf_counter();source=Path(source).resolve();bridge=Path(bridge).resolve()
    if not source.is_file() or not bridge.is_file():raise ValueError('requires regular input and native bridge files')
    if (action!='wrap' and (audio is not None or timecode is not None)) or (action!='unwrap' and audio_output is not None):
        raise ValueError('audio/timecode attachment requires wrap; audio-output requires unwrap')
    if audio is not None or timecode is not None or audio_output is not None:
        ffmpeg,probe=stock_av.executables(ffmpeg,probe)
    if audio is not None:stock_av.wav_info(audio)
    output,receipt,audio_output=stock_still.outputs(output,receipt,audio_output)
    source_info=recording(source) if action=='wrap' else None
    if timecode is not None:
        stock_av.timecode_info(timecode,source_info['video']['fps_num'],source_info['video']['fps_den'])
    source_sha=stock_still.digest(source)
    with tempfile.TemporaryDirectory(prefix='.stock-mov-',dir=output.parent) as temp:
        root=Path(temp);staged=root/('output.mov' if action=='wrap' else 'output.gvid')
        attachment=audio is not None or timecode is not None
        target=root/'video.mov' if attachment else staged
        native=run(bridge,'mux' if action=='wrap' else 'demux',source,target)
        auxiliary=None
        if attachment:
            auxiliary=stock_av.attach(target,staged,ffmpeg,probe,source_info['video']['fps_num'],
                                      source_info['video']['fps_den'],audio,timecode,bridge)
        if action=='wrap':
            # Validate the MOV actually written by this installed FFmpeg, not
            # merely the source stream or the muxer's return status.
            verified=root/'verified.gvid';inverse=run(bridge,'demux',staged,verified)
            if stock_still.digest(verified)!=source_sha or inverse['gvid_sha256']!=source_sha or native['gvid_sha256']!=source_sha:
                raise ValueError('MOV round trip changed original GVID bytes')
            native=inverse
            info=source_info;gvid_sha=source_sha;movie_sha=stock_still.digest(staged)
        else:
            info=recording(staged);gvid_sha=stock_still.digest(staged);movie_sha=source_sha
            if gvid_sha!=native['gvid_sha256']:raise ValueError('recovered GVID identity differs')
        audio_export=None
        if audio_output is not None:
            audio_export=stock_av.extract_audio(source,root/'audio.wav',ffmpeg,probe)
            audio_export=dict(**audio_export,output_sha256=stock_still.digest(root/'audio.wav'),
                              output_bytes=(root/'audio.wav').stat().st_size)
        if native['frames']!=info['frames']:raise ValueError('MOV frame count differs from verified GVID')
        result=dict(schema='gpr.stock_mov.v1',action=action,profile=native['profile'],reencoded=False,
                    source_sha256=source_sha,output_sha256=stock_still.digest(staged),output_bytes=staged.stat().st_size,
                    gvid_sha256=gvid_sha,movie_sha256=movie_sha,native_binary_sha256=stock_still.digest(bridge),
                    native_libavformat_version=native['libavformat_version'],
                    mov_tick_ns=native['unit_ns'],mov_track_timestamps_quantized=native['unit_ns']>1,
                    original_timestamps_preserved_exactly=True,
                    audio_tracks=native.get('audio_tracks',0),timecode=native.get('timecode'),
                    auxiliary=auxiliary,audio_exported=audio_output is not None,
                    audio_export=audio_export,
                    **info,elapsed_before_publication_s=time.perf_counter()-start)
        log=root/'receipt.json';log.write_text(json.dumps(result,indent=2)+'\n')
        files=[(staged,output),(log,receipt)]
        if audio_output is not None:files.append((root/'audio.wav',audio_output))
        stock_still.publish(files)
        return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action',choices=('wrap','unwrap','inspect'));parser.add_argument('input',type=Path)
    parser.add_argument('--bridge',type=Path,required=True,help='optional native gpr_mov executable')
    parser.add_argument('--output',type=Path);parser.add_argument('--receipt',type=Path)
    parser.add_argument('--audio',type=Path,help='wrap: caller-synchronized PCM16/PCM24/float32 WAV at video time zero')
    parser.add_argument('--timecode',help='wrap: HH:MM:SS:FF, or HH:MM:SS;FF for valid drop-frame rates')
    parser.add_argument('--audio-output',type=Path,help='unwrap: also recover verified PCM samples to WAV')
    parser.add_argument('--ffmpeg',type=Path);parser.add_argument('--ffprobe',dest='probe',type=Path)
    args=parser.parse_args()
    if args.action=='inspect':
        if args.output or args.receipt or args.audio or args.timecode or args.audio_output:parser.error('inspect prints its report; no output/attachment paths')
        with tempfile.TemporaryDirectory(prefix='stock-mov-inspect-') as temp:
            data=Path(temp)/'recording.gvid';native=run(args.bridge,'demux',args.input.resolve(),data)
            result=dict(schema='gpr.stock_mov.inspect.v1',native=native,**recording(data),gvid_sha256=stock_still.digest(data))
    else:
        if not args.output:parser.error('--output is required')
        result=convert(args.action,args.input,args.bridge,args.output,args.receipt,args.audio,args.timecode,args.ffmpeg,args.probe,args.audio_output)
    print(json.dumps(result,indent=2))


if __name__=='__main__':main()
