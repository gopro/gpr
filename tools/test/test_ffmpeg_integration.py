#!/usr/bin/env python3
"""Build FFmpeg n8.0 in scratch and exercise the retained production integration.

Requires git, make, a C compiler, nasm on x86, zlib development headers,
an already-built tools/gvid_mov/gvid_mov_pack linked to system FFmpeg.
GPR_BUILD must already contain PIC uninstrumented libraries, bench_fused and
fused_decode_cli. No GPR builds or source-tree writes are performed here.
"""

import argparse
import json
import os
from pathlib import Path
import shutil
import signal
import struct
import subprocess
import tempfile


def run(args, *, cwd=None, env=None, timeout=120, fail=False):
    args = list(map(str, args))
    print("+", " ".join(args), flush=True)
    with subprocess.Popen(args, cwd=cwd, env=env, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True,
                          start_new_session=True) as proc:
        try:
            output, _ = proc.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGKILL)
            output, _ = proc.communicate()
            raise RuntimeError(f"timeout ({timeout}s): {args}\n{output[-8000:]}")
    if proc.returncode < 0 or (proc.returncode != 0) != fail:
        raise RuntimeError(f"exit {proc.returncode}: {args}\n{output[-12000:]}")
    return output


def executable(build, name):
    candidates = [build / name, build / "source/app" / name / name,
                  build / "source/app" / name, build / "bin" / name]
    for path in candidates:
        if path.is_file() and os.access(path, os.X_OK):
            return path
    raise RuntimeError(f"missing {name} in {build}; build the native GPR tools first")


def extract_gvid(path, frames):
    """Read the production bench container, without introducing a MOV writer."""
    data = path.read_bytes()
    header = struct.unpack_from("<IBBHHHIIIII", data)
    assert header[0:2] == (0x44495647, 1), header
    assert header[10] == 2, header
    frames.mkdir()
    off = 32
    payloads = []
    for i in range(2):
        magic, size, tag = struct.unpack_from("<IIQ", data, off)
        assert magic == 0x004D5246 and tag == i
        off += 16
        payload = data[off:off + size]
        assert len(payload) == size and payload[:4] == b"FUSD"
        (frames / f"{i:04}.gpr").write_bytes(payload)
        payloads.append(payload)
        off += size
    assert off == len(data)
    return payloads


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime-root", type=Path,
                        default=Path(os.environ.get("GPR_ROOT", Path(__file__).resolve().parents[2])))
    parser.add_argument("--build", type=Path, default=os.environ.get("GPR_BUILD"))
    parser.add_argument("--scratch-parent", type=Path,
                        default=Path(os.environ.get("RUNNER_TEMP", tempfile.gettempdir())))
    parser.add_argument("--jobs", type=int, default=3)
    parser.add_argument("--mov-pack", type=Path,
                        help="existing portable gvid_mov_pack (default: runtime-root/tools/gvid_mov/gvid_mov_pack)")
    parser.add_argument("--build-timeout", type=int, default=1800)
    parser.add_argument("--keep", action="store_true")
    args = parser.parse_args()
    if not 1 <= args.jobs <= 8 or not 1 <= args.build_timeout <= 3600:
        parser.error("jobs must be 1..8 and build-timeout 1..3600 seconds")
    root = args.runtime_root.resolve()
    build = (args.build or root / "build").resolve()
    bench = executable(build, "bench_fused")
    direct = executable(build, "fused_decode_cli")
    codec = root / "tools/gvid_codec"
    pack = (args.mov_pack or root / "tools/gvid_mov/gvid_mov_pack").resolve()
    if not pack.is_file() or not os.access(pack, os.X_OK):
        parser.error(f"build the portable wrapper against system FFmpeg first: {pack}")
    if not (codec / "install_patch.sh").is_file():
        parser.error("runtime-root must contain the retained tools/gvid_codec integration")
    for library in ("vc5_decoder/libvc5_decoder.a", "vc5_common/libvc5_common.a",
                    "common/libcommon.a"):
        if not (build / "source/lib" / library).is_file():
            parser.error(f"missing GPR library: {library}")
    args.scratch_parent.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="ffmpeg-integration-", dir=args.scratch_parent))
    print(f"Artifacts: {work}", flush=True)
    # Avoid ambient experimental codec knobs changing either side of the oracle.
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("GPR_", "FUSED_"))}
    env.update(GPR_ROOT=str(root), GPR_BUILD=str(build), JOBS=str(args.jobs))
    passed = False
    try:
        ffroot = work / "ffmpeg"
        run(["git", "clone", "--depth=1", "--branch=n8.0",
             "https://github.com/FFmpeg/FFmpeg.git", ffroot], timeout=300, env=env)
        for _ in range(2):  # Installer must be idempotent.
            run(["bash", codec / "install_patch.sh", ffroot], env=env)
        run(["bash", codec / "configure_and_build.sh"], cwd=ffroot,
            env=env, timeout=args.build_timeout)
        ffmpeg = ffroot / "ffmpeg"
        ffprobe = ffroot / "ffprobe"

        def pack_mov(frames, mov, pf=4, dimensions=None):
            opts = ["--fps", 24, "--quality", 3, "--cfa",
                    "RGGB" if pf in (0, 1, 4) else "GBRG", "--bit-depth",
                    (12, 14, 12, 14, 16, 16)[pf], "--black-level", 0,
                    "--white-level", (1 << (12, 14, 12, 14, 16, 16)[pf]) - 1]
            if dimensions:
                opts += ["--width", dimensions[0], "--height", dimensions[1]]
            run([pack, *opts, frames, mov], env=env)

        def probe(mov):
            return json.loads(run([ffprobe, "-v", "error", "-select_streams", "v:0",
                                   "-count_frames", "-show_streams", "-of", "json", mov], env=env))["streams"][0]

        def decode(mov, out, fmt, fail=False):
            return run([ffmpeg, "-nostdin", "-v", "error", "-xerror", "-y", "-i", mov,
                        "-map", "0:v:0", "-fps_mode", "passthrough", "-c:v", "rawvideo",
                        "-pix_fmt", fmt, "-f", "rawvideo", out], env=env, fail=fail)

        # Cover all CFA/input depths, single LL, two/three levels and decimation.
        base_payloads = None
        base_expected = None
        for pf, levels, decimate in ((0, 3, 0), (1, 2, 0), (2, 3, 0),
                                     (3, 2, 1), (4, 3, 0), (5, 0, 0)):
            case = work / f"pf{pf}-l{levels}-d{decimate}"
            case.mkdir()
            raw = case / "source.raw"
            bits = (12, 14, 12, 14, 16, 16)[pf]
            samples = [((x * 197 + y * 353 + (x ^ y) * 37) % (1 << bits))
                       for y in range(128) for x in range(128)]
            raw.write_bytes(struct.pack("<16384H", *samples))
            gvid = case / "source.gvid"
            encode_env = dict(env, FUSED_MULTI_LEVEL=str(int(levels != 0)),
                              FUSED_WAVELET_LEVELS=str(levels or 3), GPR_INCLUDE_LL="1",
                              GPR_DECIMATE_AA=str(decimate), FUSED_THREADS="1",
                              GPR_ROW_DECIMATE="2" if decimate else "1",
                              GPR_COL_DECIMATE="2" if decimate else "1",
                              FUSED_QUALITY="3", GPR_BENCH_PIXEL_FORMAT=str(pf),
                              GPR_BENCH_GVID=str(gvid))
            run([bench, raw, 128, 128, 2], env=encode_env)
            frames = case / "frames"
            payloads = extract_gvid(gvid, frames)
            hdr = struct.unpack_from("<12I", payloads[0])
            assert hdr[4] == pf and hdr[10] == {0: 16, 2: 28, 3: 40}[levels], hdr
            assert (hdr[11] == 2) == bool(decimate), hdr
            width = height = 64 if decimate else 128
            fmt = "bayer_rggb16le" if pf in (0, 1, 4) else "bayer_gbrg16le"
            expected = b""
            for i in range(2):
                out = case / f"direct-{i}.raw"
                log = run([direct, frames / f"{i:04}.gpr", 128, 128, out], env=env)
                assert f"DECODE: {width}x{height}" in log, log
                decoded = out.read_bytes()
                assert len(decoded) == width * height * 2
                expected += decoded
            mov = case / "wrapped.mov"
            pack_mov(frames, mov, pf)
            info = probe(mov)
            assert info["codec_tag_string"] == "GPRr", info
            assert (info["width"], info["height"], info["pix_fmt"],
                    int(info["nb_read_frames"]), int(info["bits_per_raw_sample"])) == (
                        width, height, fmt, 2, hdr[7]), info
            output = case / "ffmpeg.raw"
            decode(mov, output, fmt)
            assert output.read_bytes() == expected, case
            if pf == 4:
                base_payloads, base_expected = payloads, expected
                # Both undersized and oversized MOV sample descriptions.
                for dims in ((16, 16), (256, 256)):
                    mismatch = case / f"mismatch-{dims[0]}.mov"
                    pack_mov(frames, mismatch, pf, dims)
                    decode(mismatch, output, fmt)
                    assert output.read_bytes() == expected
                # Change only the existing sample-entry tag, not a new writer.
                data = mov.read_bytes()
                assert data.count(b"GPRr") == 1
                legacy = case / "legacy.mov"
                legacy.write_bytes(data.replace(b"GPRr", b"GPR1"))
                assert probe(legacy)["codec_tag_string"] == "GPR1"
                decode(legacy, output, fmt)
                assert output.read_bytes() == expected
                run([ffmpeg, "-nostdin", "-v", "error", "-xerror", "-i", mov,
                     "-pix_fmt", "rgb24", case / "frame_%04d.png"], env=env)
                pngs = sorted(case.glob("frame_*.png"))
                assert len(pngs) == 2
                for png in pngs:
                    data = png.read_bytes()
                    assert data[:8] == b"\x89PNG\r\n\x1a\n"
                    assert struct.unpack_from(">II", data, 16) == (128, 128)
                prores = case / "prores.mov"
                run([ffmpeg, "-nostdin", "-v", "error", "-xerror", "-i", mov,
                     "-vf", "scale,format=rgb48le,scale,format=yuv422p10le",
                     "-c:v", "prores_ks", "-profile:v", 3, "-pix_fmt", "yuv422p10le",
                     prores], env=env)
                info = probe(prores)
                assert (info["codec_name"], info["width"], info["height"],
                        int(info["nb_read_frames"])) == ("prores", 128, 128, 2), info

        assert base_payloads is not None

        def changed(offset, value):
            data = bytearray(base_payloads[1])
            struct.pack_into("<I", data, offset, value)
            return bytes(data)

        invalid = {"magic": changed(0, 0), "version": changed(4, 2),
                   "width": changed(8, 0xFFFFFFFF), "zero-height": changed(12, 0),
                   "odd-width": changed(8, 127), "pixel-format": changed(16, 6),
                   "quality": changed(20, 12), "cfa": changed(24, 2),
                   "log-bits": changed(28, 32), "prescale": changed(32, 32),
                   "levels": changed(36, 4), "bands": changed(40, 41),
                   "band-overflow": changed(48, 0xFFFFFFFF),
                   "short-header": base_payloads[1][:47],
                   "short-table": base_payloads[1][:60],
                   "short-data": base_payloads[1][:-1],
                   "trailing-data": base_payloads[1] + b"x",
                   "dimension-change": changed(8, 64)}
        for name, payload in invalid.items():
            frames = work / name
            frames.mkdir()
            (frames / "0000.gpr").write_bytes(base_payloads[0])
            (frames / "0001.gpr").write_bytes(payload)
            mov = work / f"{name}.mov"
            pack_mov(frames, mov)
            log = decode(mov, work / f"{name}.raw", "bayer_rggb16le", fail=True)
            assert "gpr:" in log, log

        # Legacy bool depth and non-2 decimation must match the direct API.
        for name, offset, value in (("legacy-depth", 36, 1), ("decim-one", 44, 1),
                                    ("decim-other", 44, 3)):
            frames = work / name
            frames.mkdir()
            for i in range(2):
                data = bytearray(base_payloads[i])
                struct.pack_into("<I", data, offset, value)
                (frames / f"{i:04}.gpr").write_bytes(data)
            mov = work / f"{name}.mov"
            pack_mov(frames, mov, dimensions=(128, 128))
            output = work / f"{name}.raw"
            decode(mov, output, "bayer_rggb16le")
            assert output.read_bytes() == base_expected
        passed = True
        print("test_ffmpeg_integration: PASS", flush=True)
    finally:
        if passed and not args.keep:
            shutil.rmtree(work)
        else:
            print(f"Retained artifacts: {work}", flush=True)


if __name__ == "__main__":
    main()
