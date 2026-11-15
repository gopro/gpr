"""Optional local PCM/timecode MOV tracks around unchanged stock or ProRes video.

The ordinary still/capture workflows do not need FFmpeg executables. Explicit
auxiliary-track operations require caller-selected ffmpeg and ffprobe tools.
"""
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile
from fractions import Fraction

import stock_still

PROFILE = 'pcm_tmcd.v1'
PREFIX = 'gpr.media.'
CODECS = {(1, 16): 'pcm_s16le', (1, 24): 'pcm_s24le', (3, 32): 'pcm_f32le'}


def run(command):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=3600)
    if result.returncode:
        raise RuntimeError(f'{Path(command[0]).name} failed: {result.stderr[-8000:]}')
    return result.stdout


def executables(ffmpeg, probe):
    if ffmpeg is None or probe is None or any(not Path(p).is_file() for p in (ffmpeg, probe)):
        raise ValueError('audio/timecode operations require regular --ffmpeg and --ffprobe executables')
    return Path(ffmpeg).resolve(), Path(probe).resolve()


def wav_info(path):
    path = Path(path)
    if not path.is_file():
        raise ValueError('WAV source must be a regular local file')
    with path.open('rb') as file:
        header = file.read(12)
        if len(header) != 12 or header[:4] != b'RIFF' or header[8:] != b'WAVE':
            raise ValueError('audio requires little-endian RIFF/WAVE')
        end = struct.unpack_from('<I', header, 4)[0]+8
        if end != path.stat().st_size:
            raise ValueError('WAV RIFF extent differs from complete file')
        fmt = data = None
        chunks = 0
        while file.tell() < end:
            chunks += 1
            chunk = file.read(8)
            if len(chunk) != 8 or chunks > 10000:
                raise ValueError('invalid WAV chunk list')
            size = struct.unpack_from('<I', chunk, 4)[0]
            offset = file.tell()
            if offset+size+(size & 1) > end:
                raise ValueError('truncated WAV chunk')
            if chunk[:4] == b'fmt ':
                if fmt is not None or not 16 <= size <= 65536:
                    raise ValueError('invalid WAV format chunk')
                fmt = file.read(size)
            elif chunk[:4] == b'data':
                if data is not None or not size:
                    raise ValueError('requires one nonempty WAV data chunk')
                data = (offset, size)
            file.seek(offset+size+(size & 1))
        if fmt is None or data is None:
            raise ValueError('WAV format/data chunk absent')
        kind, channels, rate, byte_rate, block, bits = struct.unpack_from('<HHIIHH', fmt)
        if kind == 0xfffe:
            if (len(fmt) < 40 or struct.unpack_from('<H', fmt, 16)[0] < 22 or
                    struct.unpack_from('<H', fmt, 18)[0] != bits or
                    struct.unpack_from('<I', fmt, 20)[0] not in (0, 4 if channels == 1 else 3) or
                    fmt[28:40] != bytes.fromhex('00001000800000aa00389b71')):
                raise ValueError('unsupported extensible WAV format')
            kind = struct.unpack_from('<I', fmt, 24)[0]
        codec = CODECS.get((kind, bits))
        if (codec is None or channels not in (1, 2) or not 8000 <= rate <= 192000 or
                block != channels*bits//8 or byte_rate != rate*block or data[1] % block):
            raise ValueError('requires mono/stereo PCM16, PCM24 or float32 WAV with valid sample geometry')
        digest = hashlib.sha256()
        file.seek(data[0]); remaining = data[1]
        while remaining:
            chunk = file.read(min(remaining, 1024*1024))
            if not chunk:
                raise ValueError('WAV changed during reading')
            digest.update(chunk); remaining -= len(chunk)
    return dict(codec=codec, rate=rate, channels=channels, samples=data[1]//block,
                sha256=digest.hexdigest(), source_sha256=stock_still.digest(path))


def timecode_info(text, fps_num, fps_den):
    rate = Fraction(fps_num, fps_den)
    match = re.fullmatch(r'(\d{2}):(\d{2}):(\d{2})([:;])(\d{2})', text)
    supported = {Fraction(n, 1) for n in (20, 24, 25, 30, 50, 60)} | {
        Fraction(24000, 1001), Fraction(30000, 1001), Fraction(60000, 1001)}
    if match is None or rate not in supported:
        raise ValueError('invalid timecode syntax or unsupported timecode frame rate')
    h, m, s, separator, frame = match.groups()
    h, m, s, frame = map(int, (h, m, s, frame))
    nominal = (rate.numerator+rate.denominator-1)//rate.denominator
    if h >= 24 or m >= 60 or s >= 60 or frame >= nominal:
        raise ValueError('timecode component outside its frame-rate domain')
    drop = 0
    if separator == ';':
        if rate not in (Fraction(30000, 1001), Fraction(60000, 1001)):
            raise ValueError('drop-frame timecode requires 30000/1001 or 60000/1001')
        drop = 2 if nominal == 30 else 4
        if m % 10 and s == 0 and frame < drop:
            raise ValueError('timecode names a skipped drop-frame label')
    start = ((h*3600+m*60+s)*nominal+frame)-drop*((h*60+m)-(h*60+m)//10)
    return dict(text=text, fps_num=rate.numerator, fps_den=rate.denominator,
                start_frame=start, drop_frame=bool(drop))


def probe_movie(path, probe):
    return json.loads(run([probe, '-v', 'error', '-show_streams', '-show_format',
                          '-show_packets', '-show_data_hash', 'sha256', '-of', 'json', path]))


def pcm_hash(path, ffmpeg):
    text = run([ffmpeg, '-v', 'error', '-nostdin', '-protocol_whitelist', 'file,pipe',
                '-i', path, '-map', '0:a:0', '-c:a', 'copy', '-f', 'hash', '-hash', 'sha256', '-']).strip()
    if not re.fullmatch(r'SHA256=[0-9a-f]{64}', text):
        raise ValueError('unexpected PCM digest response')
    return text.split('=', 1)[1]


def timecode_flags(path, info, scale):
    """Restore the tmcd drop flag omitted by FFmpeg when copying that track.

    Only the generated timecode sample description is changed. Video bytes,
    packet locations and timing tables retain their original extents.
    """
    with Path(path).open('r+b') as file:
        def atoms(start, end):
            while start < end:
                file.seek(start); header = file.read(8)
                if len(header) != 8:
                    raise ValueError('incomplete generated MOV atom')
                size, kind = struct.unpack('>I4s', header); length = 8
                if size == 1:
                    extended = file.read(8)
                    if len(extended) != 8:
                        raise ValueError('incomplete extended MOV atom')
                    size = struct.unpack('>Q', extended)[0]; length = 16
                elif size == 0:
                    size = end-start
                if size < length or start+size > end:
                    raise ValueError('generated MOV atom extent differs')
                yield start, length, size, kind
                start += size
        descriptions = []
        def walk(start, end):
            for offset, header, size, kind in atoms(start, end):
                if kind in (b'moov', b'trak', b'mdia', b'minf', b'stbl'):
                    walk(offset+header, offset+size)
                elif kind == b'stsd':
                    file.seek(offset+header); table = file.read(8)
                    if len(table) != 8:
                        raise ValueError('incomplete sample description table')
                    entries = list(atoms(offset+header+8, offset+size))
                    if len(entries) != struct.unpack_from('>I', table, 4)[0]:
                        raise ValueError('sample description count differs')
                    descriptions.extend((entry, size) for entry, _, size, tag in entries if tag == b'tmcd')
        walk(0, Path(path).stat().st_size)
        if len(descriptions) != 1 or descriptions[0][1] < 36:
            raise ValueError('expected one complete timecode sample description')
        offset = descriptions[0][0]
        file.seek(offset+20); flags, actual_scale, period = struct.unpack('>III', file.read(12))
        if actual_scale != scale or Fraction(period, scale) != Fraction(info['fps_den'], info['fps_num']):
            raise ValueError('timecode sample description clock/rate differs')
        file.seek(offset+20); file.write(struct.pack('>I', (flags & ~1) | int(info['drop_frame'])))


def auxiliary(path, probe, ffmpeg=None, parsed=None):
    data = parsed if parsed is not None else probe_movie(path, probe)
    streams = data['streams']; tags = data.get('format', {}).get('tags', {})
    video = [s for s in streams if s.get('codec_type') == 'video']
    audio = [s for s in streams if s.get('codec_type') == 'audio']
    tc = [s for s in streams if s.get('codec_tag_string') == 'tmcd']
    if len(video) != 1 or len(audio) > 1 or len(tc) > 1 or len(streams) != 1+len(audio)+len(tc):
        raise ValueError('unsupported video/audio/timecode track layout')
    if bool(audio) != bool(tags.get(PREFIX+'audio_codec')) or bool(tc) != bool(tags.get(PREFIX+'timecode')):
        raise ValueError('declared auxiliary tracks are missing')
    if not audio and not tc:
        return dict(audio=None, timecode=None, timecode_clock=None)
    if tags.get(PREFIX+'aux_profile') != PROFILE:
        raise ValueError('auxiliary tracks lack the declared PCM/timecode profile')
    result = dict(audio=None, timecode=None, timecode_clock=None)
    if audio:
        stream = audio[0]
        try:
            info = dict(codec=tags[PREFIX+'audio_codec'], rate=int(tags[PREFIX+'audio_rate']),
                        channels=int(tags[PREFIX+'audio_channels']), samples=int(tags[PREFIX+'audio_samples']),
                        sha256=tags[PREFIX+'audio_sha256'])
        except (KeyError, ValueError) as error:
            raise ValueError('missing or invalid PCM metadata') from error
        if (info['codec'] not in CODECS.values() or info['channels'] not in (1, 2) or
                not 8000 <= info['rate'] <= 192000 or info['samples'] <= 0 or
                not re.fullmatch('[0-9a-f]{64}', info['sha256']) or
                stream.get('codec_name') != info['codec'] or int(stream.get('sample_rate', 0)) != info['rate'] or
                stream.get('channels') != info['channels'] or Fraction(stream['time_base']) != Fraction(1, info['rate']) or
                int(stream.get('duration_ts', 0)) != info['samples'] or int(stream.get('start_pts', -1)) != 0):
            raise ValueError('PCM track disagrees with declared source samples')
        if ffmpeg is not None and pcm_hash(path, ffmpeg) != info['sha256']:
            raise ValueError('PCM sample digest differs')
        result['audio'] = info
    if tc:
        try:
            num, den = map(int, tags[PREFIX+'timecode_rate'].split('/'))
            info = timecode_info(tags[PREFIX+'timecode'], num, den)
        except (KeyError, ValueError, ZeroDivisionError) as error:
            raise ValueError('missing or invalid timecode metadata') from error
        track = tc[0]
        packets = [p for p in data.get('packets', []) if p['stream_index'] == track['index']]
        duration = Fraction(int(video[0]['duration_ts']))*Fraction(video[0]['time_base'])
        scale = int(tags.get(PREFIX+'timecode_scale', 0))
        exact_ticks = duration*scale
        expected_ticks = (exact_ticks.numerator*2+exact_ticks.denominator)//(2*exact_ticks.denominator)
        if (track.get('tags', {}).get('timecode') != info['text'] or len(packets) != 1 or
                scale <= 0 or Fraction(track['time_base']) != Fraction(1, scale) or
                int(packets[0].get('size', 0)) != 4 or int(packets[0].get('pts', -1)) != 0 or
                int(packets[0].get('dts', -1)) != 0 or
                int(packets[0].get('duration', 0)) != expected_ticks):
            raise ValueError('timecode track label, extent or duration differs')
        with Path(path).open('rb') as file:
            file.seek(int(packets[0]['pos'])); sample = file.read(4)
        if sample != struct.pack('>I', info['start_frame']):
            raise ValueError('timecode sample frame number differs')
        result['timecode'] = info
        result['timecode_clock'] = dict(timescale=scale, duration_ticks=expected_ticks,
            duration_error_ns=float((Fraction(expected_ticks, scale)-duration)*1000000000))
    return result


def attach(video, output, ffmpeg, probe, fps_num, fps_den, audio=None, timecode=None, bridge=None):
    ffmpeg, probe = executables(ffmpeg, probe)
    before = probe_movie(video, probe)
    track = next(s for s in before['streams'] if s.get('codec_type') == 'video')
    if len(before['streams']) != 1 or track.get('codec_tag_string') not in ('GVS2', 'GVS3', 'apch', 'ap4h'):
        raise ValueError('auxiliary attachment requires one stock or ProRes HQ/4444 video track')
    audio_info = None
    if audio is not None:
        audio = Path(audio).resolve()
        if not audio.is_file():
            raise ValueError('audio source must be a regular local file')
        with audio.open('rb') as file:
            is_wav = file.read(4) == b'RIFF'
        audio_info = wav_info(audio) if is_wav else auxiliary(audio, probe, ffmpeg)['audio']
        if audio_info is None:
            raise ValueError('audio source contains no declared PCM track')
    tc = timecode_info(timecode, fps_num, fps_den) if timecode else None
    if tc is not None and (bridge is None or not Path(bridge).is_file()):
        raise ValueError('timecode requires the updated native --mov-bridge tool')
    if audio_info is None and tc is None:
        raise ValueError('requires audio or timecode to attach')
    time_base = Fraction(track['time_base'])
    if time_base.numerator != 1:
        raise ValueError('video time base requires integer track timescale')
    with tempfile.TemporaryDirectory(prefix='.stock-av-', dir=Path(output).parent) as temporary:
        template = Path(temporary)/'timecode.mov'; scale = None
        if tc:
            duration = int(track['duration_ts'])*time_base*1000000000
            duration_ns = (duration.numerator+duration.denominator-1)//duration.denominator
            carrier = json.loads(run([Path(bridge).resolve(), 'timecode', video, template,
                                      fps_num, fps_den, duration_ns, tc['text']]))
            if carrier.get('schema') != 'gpr.timecode_template.v1':
                raise ValueError('invalid native timecode carrier response')
            scale = carrier['timescale']
        command = [ffmpeg, '-v', 'error', '-nostdin', '-n', '-protocol_whitelist', 'file,pipe',
                   '-enable_drefs', '0', '-use_absolute_path', '0', '-i', video]
        if audio is not None:
            command += ['-protocol_whitelist', 'file,pipe', '-i', audio]
        if tc:
            command += ['-protocol_whitelist', 'file,pipe', '-enable_drefs', '0', '-use_absolute_path', '0', '-i', template]
        command += ['-map', '0:v:0']
        if audio is not None:
            command += ['-map', '1:a:0']
        if tc:
            command += ['-map', ('2' if audio is not None else '1')+':d:0']
        command += ['-map_metadata', '0', '-c', 'copy', '-movflags', 'use_metadata_tags', '-use_editlist', '0',
                    '-video_track_timescale', str(time_base.denominator),
                    '-movie_timescale', str(time_base.denominator), '-write_tmcd', '0']
        tags = dict(aux_profile=PROFILE)
        if audio_info:
            tags.update({'audio_'+k: str(audio_info[k]) for k in ('codec', 'rate', 'channels', 'samples', 'sha256')})
        if tc:
            tags.update(timecode=tc['text'], timecode_rate=f'{tc["fps_num"]}/{tc["fps_den"]}', timecode_scale=str(scale))
        for key, value in tags.items():
            command += ['-metadata', PREFIX+key+'='+value]
        run(command+[output])
        if tc:
            timecode_flags(output, tc, scale)
    after = probe_movie(output, probe)
    result = auxiliary(output, probe, ffmpeg, after)
    def packets(data):
        index = next(s['index'] for s in data['streams'] if s.get('codec_type') == 'video')
        return [(p['pts'], p['dts'], p['duration'], p['size'], p['data_hash'])
                for p in data['packets'] if p['stream_index'] == index]
    if packets(before) != packets(after):
        raise ValueError('auxiliary remux changed video packets or timestamps')
    if audio_info and result['audio'] != {k: v for k, v in audio_info.items() if k != 'source_sha256'}:
        raise ValueError('attached audio differs from its source')
    if result['timecode'] != tc:
        raise ValueError('attached timecode differs from requested label/rate')
    return dict(**result, video_packets_exact=True, ffmpeg_sha256=stock_still.digest(ffmpeg),
                ffprobe_sha256=stock_still.digest(probe),
                audio_source_sha256=stock_still.digest(audio) if audio is not None else None)


def extract_audio(movie, output, ffmpeg, probe):
    ffmpeg, probe = executables(ffmpeg, probe)
    info = auxiliary(movie, probe, ffmpeg)['audio']
    if info is None:
        raise ValueError('MOV contains no declared PCM track')
    run([ffmpeg, '-v', 'error', '-nostdin', '-n', '-protocol_whitelist', 'file,pipe',
         '-enable_drefs', '0', '-use_absolute_path', '0', '-i', movie,
         '-map', '0:a:0', '-c:a', 'copy', output])
    result = wav_info(output)
    if {k: v for k, v in result.items() if k != 'source_sha256'} != info:
        raise ValueError('extracted WAV samples differ')
    return info
