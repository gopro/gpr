#!/usr/bin/env python3
"""Stock MOV transport, independent FFprobe packets and failure publication."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import raw_media
import stock_mov
import stock_av
import stock_video as video


def calibration(white):
    def tag(code,dtype,fmt,values):
        values=tuple(values)
        return raw_media.record(code,dtype,len(values)//2 if dtype in (5,10) else len(values),struct.pack('<'+fmt*len(values),*values))
    return dict(schema=raw_media.SCHEMA,width=128,height=96,sample_bits=14,cfa='RGGB',
                image=[raw_media.record(271,2,5,b'Test\0'),raw_media.record(272,2,7,b'Camera\0'),
                       raw_media.record(50708,2,12,b'Test Camera\0'),raw_media.record(50706,1,4,b'\1\4\0\0'),
                       tag(33421,3,'H',(2,2)),raw_media.record(33422,1,4,b'\0\1\1\2'),
                       tag(50713,3,'H',(2,2)),tag(50714,5,'I',(33,2,35,2,37,2,39,2)),tag(50717,4,'I',(white,)),
                       tag(50721,10,'i',(1,1,0,1,0,1,0,1,1,1,0,1,0,1,0,1,1,1)),
                       tag(50728,5,'I',(1,2,1,1,2,3)),tag(50778,3,'H',(21,))],exif=[],gps=[])


def fixture(version=3,timestamps=None,clip_metadata=None,frame_metadata=None,fps=(30000,1001)):
    payload=(ROOT/'tests/fixtures/stock_preview/128x96-q3.vc5').read_bytes()
    times=timestamps if timestamps is not None else [900000000000,900033410007,900066733344]
    encode=lambda value:value if isinstance(value,bytes) else json.dumps(value,ensure_ascii=False).encode()
    if version==2:
        data=video.CLIP.pack(b'GVID',2,64,b'VC5\0',128,96,30,1,3,b'RGGB',14,32,bytes(20))
        for i in range(len(times)):data+=video.FRAME.pack(b'FRM2',32,i,i*1000000000//30,len(payload))+payload
        return data
    metadata=encode(clip_metadata if clip_metadata is not None else dict(camera='影像',raw_metadata=calibration(15000)))
    header=bytearray(video.CLIP.pack(b'GVID',3,64,b'VC5\0',128,96,*fps,3,b'RGGB',14,48,
                                    struct.pack('<III',len(metadata),1,zlib.crc32(metadata))+bytes(8)))
    struct.pack_into('<I',header,56,zlib.crc32(header[:56]));data=bytes(header)+metadata
    for i,pts in enumerate(times):
        fm=encode(frame_metadata if frame_metadata is not None else (dict(raw_metadata=calibration(14900),exposure_us=800) if i==1 else dict(exposure_us=1000)))
        record=bytearray(video.RECORD3.pack(b'FRM3',48,i,pts,len(payload),len(fm),zlib.crc32(payload),zlib.crc32(fm),0))
        struct.pack_into('<I',record,44,zlib.crc32(record[:44]));data+=record+fm+payload
    ending=bytearray(video.RECORD3.pack(b'END3',48,len(times),times[-1] if times else 0,len(times)*len(payload),zlib.crc32(data),0,0,0))
    struct.pack_into('<I',ending,36,zlib.crc32(ending[:36]));return data+ending


def wav(path, bits=16, channels=1, samples=5001):
    kind=3 if bits==32 else 1
    data=(struct.pack('<'+'f'*(samples*channels),*((i%201-100)/100 for i in range(samples*channels)))
          if kind==3 else b''.join(((i*7331)%(1<<bits)).to_bytes(bits//8,'little') for i in range(samples*channels)))
    block=channels*bits//8
    fmt=struct.pack('<HHIIHH',kind,channels,48000,48000*block,block,bits)
    body=b'WAVEfmt '+struct.pack('<I',16)+fmt+b'data'+struct.pack('<I',len(data))+data+bytes(len(data)%2)
    path.write_bytes(b'RIFF'+struct.pack('<I',len(body))+body)
    return hashlib.sha256(data).hexdigest()


def source_records(data):
    file=io.BytesIO(data);header=video.read_header(file);start=file.tell();rows=[]
    for row in video.records(file,header):
        end=file.tell();rows.append((row,data[start:end]));start=end
    return header,rows


class Transport(unittest.TestCase):
    def wrap(self,root,data):
        source=root/'source.gvid';source.write_bytes(data);movie=root/'movie.mov'
        report=stock_mov.convert('wrap',source,args.bridge,movie)
        return source,movie,report

    def test_real_payload_calibration_and_v2_v3_roundtrip(self):
        for version in (2,3):
            with self.subTest(version=version),tempfile.TemporaryDirectory() as temp:
                root=Path(temp);data=fixture(version);source,movie,receipt=self.wrap(root,data)
                self.assertEqual(receipt['profile'],'GVS'+str(version));self.assertFalse(receipt['reencoded'])
                self.assertEqual(receipt['frames'],3);self.assertEqual(receipt['mov_tick_ns'],1)
                self.assertFalse(receipt['mov_track_timestamps_quantized'])
                # Interchange is self-contained; publication receipts are not
                # required input sidecars for recovering calibration or time.
                movie.with_suffix('.mov.json').unlink()
                returned=root/'returned.gvid';report=stock_mov.convert('unwrap',movie,args.bridge,returned)
                self.assertEqual(returned.read_bytes(),data)
                self.assertEqual(report['calibrated_frames'],3 if version==3 else 0)
                header,rows=source_records(data)
                probe=subprocess.run([str(args.ffprobe),'-v','error','-show_streams','-show_packets','-show_format',
                                      '-show_data_hash','sha256','-of','json',str(movie)],capture_output=True,text=True,check=True)
                parsed=json.loads(probe.stdout);stream=parsed['streams'][0]
                self.assertEqual(stream['codec_tag_string'],'GVS'+str(version))
                self.assertEqual((stream['width'],stream['height']),(128,96));self.assertEqual(stream['time_base'],'1/1000000000')
                self.assertEqual(len(parsed['packets']),3)
                self.assertEqual(parsed['format']['tags']['gpr.stock.sha256'],hashlib.sha256(data).hexdigest())
                for packet,(row,record) in zip(parsed['packets'],rows):
                    self.assertEqual(int(packet['pts']),row['pts_ns']-rows[0][0]['pts_ns'])
                    self.assertEqual(packet['data_hash'].lower(),'sha256:'+hashlib.sha256(record).hexdigest())
                    self.assertTrue(packet['flags'].startswith('K'))
                if version==3:self.assertTrue(report['end_marker_verified'])

    def test_large_absolute_timestamps_and_adaptive_mov_clock(self):
        first=2**64-9000000000
        data=fixture(timestamps=[first,first+33410007,first+6000000017])
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);_,movie,receipt=self.wrap(root,data)
            self.assertEqual(receipt['mov_tick_ns'],10)
            self.assertTrue(receipt['mov_track_timestamps_quantized'])
            returned=root/'returned.gvid';stock_mov.convert('unwrap',movie,args.bridge,returned)
            self.assertEqual(returned.read_bytes(),data)
            self.assertEqual(receipt['first_timestamp_ns'],first)

    def test_maximum_clip_and_frame_json_survive_without_sidecars(self):
        data=b'{"padding":"'+b'x'*(65536-len(b'{"padding":""}'))+b'"}'
        self.assertEqual(len(data),65536)
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);source,movie,_=self.wrap(root,fixture(clip_metadata=data,frame_metadata=data))
            out=root/'returned.gvid';stock_mov.convert('unwrap',movie,args.bridge,out)
            self.assertEqual(out.read_bytes(),source.read_bytes())

    def test_unrepresentable_timeline_does_not_publish(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);source=root/'source.gvid';source.write_bytes(fixture(timestamps=[0,1,5000000000]))
            movie=root/'bad.mov'
            with self.assertRaisesRegex(RuntimeError,'cannot represent'):
                stock_mov.convert('wrap',source,args.bridge,movie)
            self.assertFalse(movie.exists());self.assertFalse(movie.with_suffix('.mov.json').exists())

    def test_invalid_sources_and_incomplete_recordings_do_not_publish(self):
        good=fixture();payload=(ROOT/'tests/fixtures/stock_preview/128x96-q3.vc5').read_bytes()
        bad=bytearray(good);bad[good.index(payload)+40]^=1
        cases=(good[:-1],good[:-48],good+b'x',good[:64],bytes(bad),fixture(timestamps=[]),fixture(clip_metadata=b'{"a":1,"a":2}'))
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp)
            for i,data in enumerate(cases):
                with self.subTest(case=i):
                    source=root/f'{i}.gvid';source.write_bytes(data);movie=root/f'{i}.mov'
                    with self.assertRaises((ValueError,RuntimeError)):stock_mov.convert('wrap',source,args.bridge,movie)
                    self.assertFalse(movie.exists());self.assertFalse(movie.with_suffix('.mov.json').exists())

    def test_corrupt_payload_digest_codec_and_timing_are_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);source,movie,_=self.wrap(root,fixture());data=movie.read_bytes()
            probe=json.loads(subprocess.check_output([str(args.ffprobe),'-v','error','-show_packets','-of','json',str(movie)],text=True))
            payload=bytearray(data);payload[int(probe['packets'][0]['pos'])+60]^=1
            digest=hashlib.sha256(source.read_bytes()).hexdigest().encode();self.assertEqual(data.count(digest),1)
            changed_digest=data.replace(digest,(b'0' if digest[:1]!=b'0' else b'1')+digest[1:])
            self.assertEqual(data.count(b'GVS3'),1);wrong_codec=data.replace(b'GVS3',b'GPR1')
            timing=bytearray(data);offset=data.index(b'stts')+16
            value,=struct.unpack_from('>I',timing,offset);struct.pack_into('>I',timing,offset,value+100)
            oversized=bytearray(data);offset=data.index(b'stsz')+16;struct.pack_into('>I',oversized,offset,0xffffffff)
            for i,content in enumerate((bytes(payload),changed_digest,wrong_codec,bytes(timing),bytes(oversized),data[:-10],b'not a movie')):
                with self.subTest(case=i):
                    bad=root/f'bad-{i}.mov';bad.write_bytes(content);returned=root/f'bad-{i}.gvid'
                    with self.assertRaises(RuntimeError):stock_mov.convert('unwrap',bad,args.bridge,returned)
                    self.assertFalse(returned.exists());self.assertFalse(returned.with_suffix('.gvid.json').exists())

    def test_exclusive_outputs_and_dangling_symlinks(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);source,movie,_=self.wrap(root,fixture());protected=root/'protected'
            with self.assertRaisesRegex(ValueError,'regular input'):
                stock_mov.convert('wrap',Path('/dev/null'),args.bridge,root/'device.mov')
            self.assertFalse((root/'device.mov').exists())
            fifo=root/'pipe';os.mkfifo(fifo)
            with self.assertRaisesRegex(ValueError,'regular input'):
                stock_mov.convert('wrap',fifo,args.bridge,root/'pipe.mov')
            result=subprocess.run([str(args.bridge),'mux',str(fifo),str(root/'pipe.mov')],capture_output=True,timeout=20)
            self.assertNotEqual(result.returncode,0);self.assertFalse((root/'pipe.mov').exists())
            protected.write_bytes(b'keep')
            for action,input_path in (('wrap',source),('unwrap',movie)):
                with self.assertRaises(FileExistsError):stock_mov.convert(action,input_path,args.bridge,protected)
                self.assertEqual(protected.read_bytes(),b'keep')
                result=subprocess.run([str(args.bridge),'mux' if action=='wrap' else 'demux',str(input_path),str(protected)],capture_output=True)
                self.assertNotEqual(result.returncode,0);self.assertEqual(protected.read_bytes(),b'keep')
            link=root/'dangling';link.symlink_to(root/'absent')
            with self.assertRaises(FileExistsError):stock_mov.convert('wrap',source,args.bridge,link)
            self.assertTrue(link.is_symlink())

    def test_wrapper_validates_json_after_native_opaque_transport(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);source=root/'opaque.gvid';source.write_bytes(fixture(clip_metadata=b'{"a":1,"a":2}'))
            movie=root/'opaque.mov';stock_mov.run(args.bridge,'mux',source,movie)
            out=root/'bad.gvid'
            with self.assertRaisesRegex(ValueError,'duplicate GVID metadata key'):
                stock_mov.convert('unwrap',movie,args.bridge,out)
            self.assertFalse(out.exists())


    def test_pcm_audio_and_timecode_roundtrip_preserves_original_gvid(self):
        for bits in (16,24,32):
            for channels in (1,2):
                with self.subTest(bits=bits,channels=channels),tempfile.TemporaryDirectory() as temp:
                    root=Path(temp);source=root/'source.gvid';source.write_bytes(fixture())
                    audio=root/'source.wav';expected=wav(audio,bits,channels)
                    movie=root/'av.mov'
                    receipt=stock_mov.convert('wrap',source,args.bridge,movie,audio=audio,timecode='00:01:00;02',
                        ffmpeg=args.ffmpeg,probe=args.ffprobe)
                    self.assertTrue(receipt['auxiliary']['video_packets_exact'])
                    self.assertEqual(receipt['auxiliary']['audio']['sha256'],expected)
                    self.assertEqual(receipt['timecode'],'00:01:00;02')
                    recovered=root/'recovered.gvid';sound=root/'recovered.wav'
                    receipt=stock_mov.convert('unwrap',movie,args.bridge,recovered,audio_output=sound,
                        ffmpeg=args.ffmpeg,probe=args.ffprobe)
                    self.assertEqual(recovered.read_bytes(),source.read_bytes());self.assertTrue(receipt['audio_exported'])
                    self.assertEqual(stock_av.wav_info(sound)['sha256'],expected)

    def test_twenty_fps_and_invalid_drop_frame_labels(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);source=root/'source.gvid';source.write_bytes(fixture(fps=(20,1)))
            movie=root/'20.mov'
            result=stock_mov.convert('wrap',source,args.bridge,movie,timecode='01:02:03:04',ffmpeg=args.ffmpeg,probe=args.ffprobe)
            self.assertEqual(result['timecode'],'01:02:03:04');self.assertEqual(result['audio_tracks'],0)
            source.write_bytes(fixture())
            for index,label in enumerate(('00:01:00;00','00:01:00;01','24:00:00:00','00:00:00:30','broken')):
                output=root/f'bad-{index}.mov'
                with self.assertRaises(ValueError):
                    stock_mov.convert('wrap',source,args.bridge,output,timecode=label,ffmpeg=args.ffmpeg,probe=args.ffprobe)
                self.assertFalse(output.exists())

    def test_damaged_audio_and_timecode_fail_native_inverse_verification(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);source=root/'source.gvid';source.write_bytes(fixture())
            sound=root/'source.wav';wav(sound);movie=root/'av.mov'
            stock_mov.convert('wrap',source,args.bridge,movie,audio=sound,timecode='01:02:03;04',
                              ffmpeg=args.ffmpeg,probe=args.ffprobe)
            parsed=stock_av.probe_movie(movie,args.ffprobe);data=movie.read_bytes()
            for kind in ('audio','data'):
                stream=next(s['index'] for s in parsed['streams'] if s['codec_type']==kind)
                packet=next(p for p in parsed['packets'] if p['stream_index']==stream)
                damaged=bytearray(data);damaged[int(packet['pos'])]^=1
                path=root/(kind+'.mov');path.write_bytes(damaged);output=root/(kind+'.gvid')
                with self.assertRaises(RuntimeError):stock_mov.convert('unwrap',path,args.bridge,output)
                self.assertFalse(output.exists());self.assertFalse(output.with_suffix('.gvid.json').exists())

    def test_long_timecode_track_keeps_nanosecond_video_clock(self):
        for fps,period,label in (((20,1),50000000,'01:00:00:00'),
                                 ((30000,1001),33366667,'00:01:00;02')):
            with self.subTest(fps=fps),tempfile.TemporaryDirectory() as temp:
                root=Path(temp);source=root/'long.gvid'
                source.write_bytes(fixture(timestamps=[900000000000+i*period for i in range(120)],fps=fps))
                movie=root/'long.mov'
                receipt=stock_mov.convert('wrap',source,args.bridge,movie,timecode=label,
                                         ffmpeg=args.ffmpeg,probe=args.ffprobe)
                self.assertEqual(receipt['mov_tick_ns'],1)
                self.assertTrue(receipt['auxiliary']['video_packets_exact'])
                self.assertLess(receipt['auxiliary']['timecode_clock']['timescale'],1000000000)
                self.assertEqual(receipt['timecode'],label)
                returned=root/'long.gvid.copy'
                stock_mov.convert('unwrap',movie,args.bridge,returned)
                self.assertEqual(returned.read_bytes(),source.read_bytes())

    def test_invalid_wav_and_protected_audio_output_leave_no_partial_recording(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);source=root/'source.gvid';source.write_bytes(fixture())
            good=root/'good.wav';wav(good);data=good.read_bytes()
            for index,content in enumerate((b'not a WAV',data[:-1],data[:8]+b'BAD!'+data[12:])):
                bad=root/f'bad-{index}.wav';bad.write_bytes(content);movie=root/f'bad-{index}.mov'
                with self.assertRaises(ValueError):
                    stock_mov.convert('wrap',source,args.bridge,movie,audio=bad,ffmpeg=args.ffmpeg,probe=args.ffprobe)
                self.assertFalse(movie.exists());self.assertFalse(movie.with_suffix('.mov.json').exists())
            movie=root/'good.mov'
            stock_mov.convert('wrap',source,args.bridge,movie,audio=good,ffmpeg=args.ffmpeg,probe=args.ffprobe)
            protected=root/'protected.wav';protected.write_bytes(b'keep');recording=root/'returned.gvid'
            with self.assertRaises(FileExistsError):
                stock_mov.convert('unwrap',movie,args.bridge,recording,audio_output=protected,
                                  ffmpeg=args.ffmpeg,probe=args.ffprobe)
            self.assertEqual(protected.read_bytes(),b'keep');self.assertFalse(recording.exists())


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bridge',type=Path,required=True);parser.add_argument('--ffprobe',type=Path,required=True)
    parser.add_argument('--ffmpeg',type=Path,required=True)
    args=parser.parse_args();args.bridge=args.bridge.resolve();args.ffprobe=args.ffprobe.resolve();args.ffmpeg=args.ffmpeg.resolve()
    unittest.main(argv=[sys.argv[0]])
