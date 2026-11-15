#!/usr/bin/env python3
"""Build stripped source profiles, relocate packages, run external C/C++ clients.

SDK sources and the unused codec are physically absent from minimal builds.
This is an integration check, not an in-tree target-name smoke test.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import time
import zipfile
import wave


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--work-dir', type=Path, required=True, help='new directory')
    parser.add_argument('--full-build', type=Path, help='also install/test an existing full build')
    parser.add_argument('--cmake-arg', action='append', default=[])
    args = parser.parse_args(); root = args.source.resolve(); work = args.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=False); results = []
    revision = subprocess.run(['git', '-C', str(root), 'rev-parse', 'HEAD'], capture_output=True, text=True)
    revision = revision.stdout.strip() if not revision.returncode else 'unversioned-source'
    def run(command, log, okay=True):
        command = [str(x) for x in command]
        environment = os.environ.copy()
        if platform.system() == 'Windows':
            # Persistent MSBuild nodes retain a cwd handle, preventing the
            # producer deletion that makes this relocation check meaningful.
            environment['MSBUILDDISABLENODEREUSE'] = '1'
            if command[:2] == ['cmake', '--build']:
                cache = Path(command[2])/'CMakeCache.txt'
                if 'CMAKE_GENERATOR:INTERNAL=Visual Studio' in cache.read_text():
                    command += ['--', '/nodeReuse:false']
        result = subprocess.run(command, capture_output=True, text=True, env=environment)
        log.write_text(result.stdout+'\n'+result.stderr)
        if (result.returncode == 0) != okay:
            raise RuntimeError(f'command failed: {command}\n{log.read_text()[-6000:]}')
        return result
    def remove_producer(path):
        for attempt in range(30):
            try:
                shutil.rmtree(path)
                return
            except PermissionError as error:
                if platform.system() != 'Windows' or getattr(error, 'winerror', None) != 32 or attempt == 29:
                    raise
                time.sleep(0.1)
    def source_digest(src):
        hashes = {p.relative_to(src).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                  for directory in ('source', 'cmake', 'tools', 'tests') for p in sorted((src/directory).rglob('*'))
                  if p.is_file() and '__pycache__' not in p.parts and p.name != '.DS_Store'}
        hashes['CMakeLists.txt'] = hashlib.sha256((src/'CMakeLists.txt').read_bytes()).hexdigest()
        return hashlib.sha256(json.dumps(hashes, sort_keys=True).encode()).hexdigest()
    def unpack(archive, dest):
        dest.mkdir()
        with zipfile.ZipFile(archive) as package:
            package.extractall(dest)
            # zipfile does not restore the executable modes stored by CPack.
            if platform.system() != 'Windows':
                for item in package.infolist():
                    mode = (item.external_attr >> 16) & 0o777
                    if mode: (dest/item.filename).chmod(mode)
    def consume(prefix, dest, expected):
        build = dest/'client'
        configure = ['cmake', '-S', root/'tests/package_consumer', '-B', build,
                     '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF',
                     '-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF',
                     '-DGPR_DIR='+str(prefix/'lib/cmake/GPR'),
                     '-DEXPECTED_COMPONENTS='+';'.join(expected),
                     '-DFIXTURES='+str(root/'tests/fixtures/stock_preview'), *args.cmake_arg]
        run(configure, dest/'client-configure.log')
        run(['cmake', '--build', build, '--config', 'Release', '--parallel', '4'], dest/'client-build.log')
        run(['ctest', '--test-dir', build, '-C', 'Release', '--output-on-failure'], dest/'client-test.log')
        tools = prefix/'share/gpr/tools'
        run([sys.executable,tools/'stock_preview.py','--help'],dest/'installed-preview-prepare-help.log')
        run([sys.executable,'-c',
             "import ast,sys; from pathlib import Path; sys.path.insert(0,sys.argv[1]); import stock_cnn_metadata,stock_sequence,stock_upres,stock_coreml,stock_metal,stock_native; ast.parse((Path(sys.argv[1])/'stock_cnn_models.py').read_text()); assert 'torch' not in sys.modules and 'coremltools' not in sys.modules",
             tools],dest/'installed-optional-cnn-contract.log')
        inspected = run([sys.executable, tools/'stock_video.py', 'inspect', root/'tests/fixtures/stock_preview/container-v3.gvid'], dest/'python-inspect.log')
        report = json.loads(inspected.stdout)
        if report['frame_count'] != 2 or not report['complete'] or not report['end_marker_verified']:
            raise RuntimeError('installed Python inspector failed')
        if (prefix/'bin/gpr_mov').is_file():
            if not (prefix/'share/gpr/docs/stock-mov.md').is_file():raise RuntimeError('MOV package lacks its interchange guide')
            if 'Optional MOV tool: ON' not in (prefix/'share/gpr/gpr-build-info.txt').read_text():
                raise RuntimeError('MOV package does not declare its optional external dependency')
            movie=build/'interchange.mov';returned=build/'interchange.gvid'
            original=root/'tests/fixtures/stock_preview/container-v3.gvid'
            for action,source,output in (('wrap',original,movie),('unwrap',movie,returned)):
                run([sys.executable,tools/'stock_mov.py',action,source,'--bridge',prefix/'bin/gpr_mov',
                     '--output',output],dest/f'installed-mov-{action}.log')
            if returned.read_bytes()!=original.read_bytes():raise RuntimeError('installed MOV interchange changed recording bytes')
            ffmpeg=shutil.which('ffmpeg');ffprobe=shutil.which('ffprobe')
            if not ffmpeg or not ffprobe:raise RuntimeError('MOV auxiliary package checks require ffmpeg and ffprobe')
            sound=build/'audio.wav';pcm=bytes(range(256))*8
            with wave.open(str(sound),'wb') as file:
                file.setnchannels(2);file.setsampwidth(2);file.setframerate(48000);file.writeframes(pcm)
            movie=build/'av.mov';returned=build/'av.gvid';recovered=build/'av.wav'
            run([sys.executable,tools/'stock_mov.py','wrap',original,'--bridge',prefix/'bin/gpr_mov',
                 '--output',movie,'--audio',sound,'--timecode','01:00:00:00','--ffmpeg',ffmpeg,'--ffprobe',ffprobe],dest/'installed-mov-av-wrap.log')
            decoded=run([sys.executable,tools/'stock_mov.py','unwrap',movie,'--bridge',prefix/'bin/gpr_mov',
                 '--output',returned,'--audio-output',recovered,'--ffmpeg',ffmpeg,'--ffprobe',ffprobe],dest/'installed-mov-av-unwrap.log')
            receipt=json.loads(decoded.stdout)
            if (returned.read_bytes()!=original.read_bytes() or receipt['audio_export']['sha256']!=hashlib.sha256(pcm).hexdigest()
                    or receipt['timecode']!='01:00:00:00'):
                raise RuntimeError('installed audio/timecode interchange changed samples or recording')
        if (prefix/'bin/gpr_render').is_file():
            if 'Optional RAW/ProRes renderer: ON' not in (prefix/'share/gpr/gpr-build-info.txt').read_text():
                raise RuntimeError('renderer package does not declare its macOS dependency')
            for helper in ('gpr_render', 'gpr_prores'):
                run([prefix/'bin'/helper, '--version'], dest/f'installed-{helper}.log')
            run([sys.executable, tools/'stock_prores.py', '--help'], dest/'installed-prores-help.log')
        if 'sdk' in expected:
            meta = build/'calibration.json'
            run([sys.executable, tools/'raw_media.py', 'metadata', build/'sample.gpr', '--sample-bits', '14', '--output', meta], dest/'python-metadata.log')
            if (prefix/'bin/gpr_preview_profile').is_file():
                color=run([prefix/'bin/gpr_preview_profile',build/'sample.gpr'],dest/'installed-preview-color.log')
                if json.loads(color.stdout)['schema']!='gpr.preview_color.v1':raise RuntimeError('installed DNG color compiler failed')
                clip=build/'preview.gvid'
                run([sys.executable,'-c',
                     "import struct,sys;from pathlib import Path;p=Path(sys.argv[1]).read_bytes();Path(sys.argv[2]).write_bytes(struct.pack('<4sHH4sIIIII4sII20s',b'GVID',2,64,b'VC5\\0',128,96,30,1,3,b'RGGB',14,32,bytes(20))+struct.pack('<4sIQQQ',b'FRM2',32,0,0,len(p))+p)",
                     root/'tests/fixtures/stock_preview/128x96-q3.vc5',clip],dest/'installed-preview-input.log')
                profiles=build/'preview-profiles'
                run([sys.executable,tools/'stock_preview.py',clip,'--output-dir',profiles,'--metadata-file',meta,
                     '--compiler',prefix/'bin/gpr_preview_profile'],dest/'installed-preview-prepare.log')
                if (prefix/'bin/gpr_preview_app').is_file():
                    playback=run([prefix/'bin/gpr_preview_app','--input',clip,'--profile-dir',profiles],dest/'installed-preview-playback.log')
                    summary=json.loads(playback.stdout.splitlines()[-1])
                    if not summary['calibrated'] or summary['rendered']!=1:raise RuntimeError('installed calibrated preview failed')
            raw = build/'original.raw'; dng = build/'editable.dng'; gpr = build/'editable.gpr'
            run([prefix/'bin/stock_video', '--decode', root/'tests/fixtures/stock_preview/128x96-q3.vc5',
                 '--width', '128', '--height', '96', '--output', raw], dest/'installed-decoder.log')
            for kind, source, output in (('dng', raw, dng), ('gpr', root/'tests/fixtures/stock_preview/128x96-q3.vc5', gpr)):
                run([sys.executable, tools/'raw_media.py', 'export', source, '--metadata', meta, '--kind', kind, '--output', output], dest/f'python-{kind}.log')
                decoded = build/f'{kind}.raw'
                run([prefix/'bin/gpr_tools', '-i', output, '-o', decoded], dest/f'installed-sdk-{kind}.log')
                if decoded.read_bytes() != raw.read_bytes(): raise RuntimeError('installed editable workflow changed Bayer values')
            preset=build/'preset.gpr'; editable=build/'preset.dng'
            run([sys.executable,tools/'stock_still.py','encode',raw,'--metadata',meta,'--quality','0',
                 '--sdk',prefix/'bin/gpr_tools','--output',preset],dest/'installed-still-encode.log')
            run([sys.executable,tools/'stock_still.py','decode',preset,'--sample-bits','14',
                 '--sdk',prefix/'bin/gpr_tools','--output',editable],dest/'installed-still-decode.log')
            for image,name in ((preset,'preset'),(editable,'preset-dng')):
                run([prefix/'bin/gpr_tools','-i',image,'-o',build/(name+'.raw')],dest/(name+'-decode.log'))
            if (build/'preset.raw').read_bytes()!=(build/'preset-dng.raw').read_bytes():
                raise RuntimeError('installed still preset workflow changed decoded samples')
    for name, encoder, decoder, capture, preview in (
            ('capture', True, False, True, False), ('preview', False, True, False, True),
            ('camera', True, True, True, True)):
        start = time.perf_counter(); dest = work/name; src = dest/'source'; src.mkdir(parents=True)
        for path in ('CMakeLists.txt', 'LICENSE.txt', 'LICENSE-MIT', 'LICENSE-APACHE'):
            shutil.copy2(root/path, src/path)
        shutil.copytree(root/'cmake', src/'cmake')
        shutil.copytree(root/'tests', src/'tests', ignore=shutil.ignore_patterns('__pycache__'))
        (src/'docs').mkdir()
        for doc in (root/'docs').glob('stock-*'):
            if doc.is_dir(): shutil.copytree(doc, src/'docs'/doc.name)
            else: shutil.copy2(doc, src/'docs'/doc.name)
        (src/'tools').mkdir()
        for tool in ('stock_video.py', 'raw_media.py', 'stock_still.py', 'stock_mov.py', 'stock_prores.py', 'stock_preview.py', 'stock_cnn_models.py', 'stock_cnn_metadata.py', 'stock_av.py', 'stock_coreml.py', 'stock_metal.py', 'stock_native.py', 'stock_sequence.py', 'stock_upres.py'): shutil.copy2(root/'tools'/tool, src/'tools'/tool)
        shutil.copytree(root/'tools/ffmpeg_stock',src/'tools/ffmpeg_stock',ignore=shutil.ignore_patterns('__pycache__'))
        (src/'source/app').mkdir(parents=True)
        shutil.copy2(root/'source/app/gpr_mov.cpp',src/'source/app/gpr_mov.cpp')
        for helper_name in ('gpr_render.m', 'gpr_prores.m'):
            shutil.copy2(root/'source/app'/helper_name, src/'source/app'/helper_name)
        components = ['common', 'vc5_common', 'gpr_video']
        if encoder: components.append('vc5_encoder')
        if decoder: components.append('vc5_decoder')
        for component in components: shutil.copytree(root/'source/lib'/component, src/'source/lib'/component)
        for absent in ('gpr_sdk', 'dng_sdk', 'xmp_core', 'expat_lib', 'md5_lib', 'tiny_jpeg'):
            if (src/'source/lib'/absent).exists(): raise RuntimeError('SDK source leaked into minimal profile')
        for codec, present in (('vc5_encoder', encoder), ('vc5_decoder', decoder)):
            if (src/'source/lib'/codec).exists() != present: raise RuntimeError('codec isolation failed')
        digest = source_digest(src)
        build = dest/'build'; prefix = dest/'install'
        settings = dict(GPR_BUILD_SDK=False, GPR_BUILD_STILL_APPS=False, GPR_BUILD_LEGACY_CLI=False,
                        GPR_BUILD_ENCODER=encoder, GPR_BUILD_DECODER=decoder,
                        GPR_ENABLE_VIDEO=capture, GPR_ENABLE_PREVIEW=preview,
                        GPR_BUILD_VIDEO_TESTS=capture, GPR_BUILD_PREVIEW_TESTS=preview,
                        GPR_BUILD_CODEC_TESTS=encoder and decoder, GPR_BUILD_PACKAGE=True)
        command = ['cmake', '-S', src, '-B', build, '-DCMAKE_BUILD_TYPE=Release',
                   '-DCMAKE_INSTALL_LIBDIR=lib', '-DGPR_VIDEO_PYTHON='+sys.executable,
                   '-DGPR_SOURCE_REVISION='+revision, '-DGPR_SOURCE_DIGEST='+digest,
                   '-DCMAKE_INSTALL_PREFIX='+str(prefix), *args.cmake_arg]
        command += [f'-D{k}={"ON" if v else "OFF"}' for k,v in settings.items()]
        run(command, dest/'configure.log')
        run(['cmake', '--build', build, '--config', 'Release', '--parallel', '4'], dest/'build.log')
        run(['ctest', '--test-dir', build, '-C', 'Release', '--output-on-failure'], dest/'test.log')
        run(['cmake', '--install', build, '--config', 'Release'], dest/'install.log')
        run(['cpack', '--config', build/'CPackConfig.cmake', '-C', 'Release', '-G', 'ZIP', '-B', dest/'archives'], dest/'package.log')
        archive = next((dest/'archives').glob('*.zip'))
        archive_sha = hashlib.sha256(archive.read_bytes()).hexdigest()
        # Delete producer paths before finding/linking the relocated package.
        remove_producer(src); remove_producer(build); remove_producer(prefix)
        unpacked = dest/'relocated'; unpack(archive, unpacked)
        configs = list(unpacked.rglob('GPRConfig.cmake'))
        if len(configs) != 1: raise RuntimeError('ambiguous/missing package config')
        relocated = configs[0].parents[3]
        if 'Source snapshot SHA-256: '+digest not in (relocated/'share/gpr/gpr-build-info.txt').read_text():
            raise RuntimeError('source identity missing from archive')
        expected = []
        for component, present in (('encoder', encoder), ('decoder', decoder), ('capture', capture), ('preview', preview)):
            if present: expected.append(component)
        consume(relocated, dest, expected)
        # A package without the SDK must reject a required SDK component.
        run(['cmake', '-S', root/'tests/package_consumer', '-B', dest/'missing-sdk',
             '-DGPR_DIR='+str(configs[0].parent), '-DEXPECTED_COMPONENTS=sdk', *args.cmake_arg], dest/'missing-sdk.log', okay=False)
        results.append(dict(profile=name, sdk_sources_present=False, encoder=encoder, decoder=decoder,
                            producer_removed_before_consumer=True, archive_sha256=archive_sha,
                            source_digest=digest,
                            seconds=time.perf_counter()-start))
        print(f'PASS: {name} stripped build, tests, ZIP, relocated C/C++ client', flush=True)
    if args.full_build:
        dest = work/'full'; dest.mkdir()
        full_digest = source_digest(root)
        run(['cmake', '-S', root, '-B', args.full_build.resolve(), '-DGPR_BUILD_PACKAGE=ON',
             '-DGPR_SOURCE_REVISION='+revision, '-DGPR_SOURCE_DIGEST='+full_digest, *args.cmake_arg], dest/'configure.log')
        run(['cmake', '--build', args.full_build.resolve(), '--config', 'Release', '--parallel', '4'], dest/'build.log')
        run(['cpack', '--config', args.full_build.resolve()/'CPackConfig.cmake', '-C', 'Release', '-G', 'ZIP',
             '-B', dest/'archives'], dest/'package.log')
        archive = next((dest/'archives').glob('*.zip'))
        unpacked = dest/'relocated'; unpack(archive, unpacked)
        configs = list(unpacked.rglob('GPRConfig.cmake'))
        if len(configs) != 1: raise RuntimeError('ambiguous/missing full package config')
        relocated = configs[0].parents[3]
        if 'Source snapshot SHA-256: '+full_digest not in (relocated/'share/gpr/gpr-build-info.txt').read_text():
            raise RuntimeError('full source identity missing from archive')
        consume(relocated, dest, ['encoder', 'decoder', 'sdk', 'capture', 'preview'])
        results.append(dict(profile='full', relocated_sdk_consumer=True,
                            source_digest=full_digest,
                            archive_sha256=hashlib.sha256(archive.read_bytes()).hexdigest()))
        print('PASS: full package native and standalone SDK C/C++ clients', flush=True)
    receipt = dict(schema='gpr.build_profiles.v1', platform=platform.platform(), profiles=results)
    (work/'receipt.json').write_text(json.dumps(receipt, indent=2)+'\n')


if __name__ == '__main__': main()
