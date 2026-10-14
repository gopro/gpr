#!/usr/bin/env python3
"""Check target/source isolation, optionally compiling each supported profile."""
import argparse
import json
import os
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def run(args, **kwargs):
    subprocess.run(args, check=True, timeout=900, **kwargs)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", action="store_true")
    args = parser.parse_args()
    camera = ["-DGPR_BUILD_SDK=OFF", "-DGPR_BUILD_STILL_APPS=OFF"]
    profiles = {
        "default": ([], {"gpr_tools", "gvid_preview_rgb_cli", "fused_decode_cli"},
                    {"bench_fused", "labs_encoder_bench_cli", "coeff_io_tool",
                     "vc5_encoder_app", "vc5_decoder_app", "ans_test"}),
        "camera": (camera, {"vc5_encoder", "gvid_preview_rgb_cli"},
                   {"gpr_sdk", "dng_sdk", "xmp_core", "gpr_tools", "bench_fused"}),
        "stills": (["-DGPR_ENABLE_VIDEO=OFF", "-DGPR_BUILD_PREVIEW_TOOLS=OFF"],
                   {"gpr_tools"}, {"gvid_preview_rgb_cli", "fused_decode_cli", "bench_fused"}),
        "utilities": (camera + ["-DGPR_BUILD_CODEC_UTILITIES=ON",
                                "-DGPR_BUILD_DIAGNOSTICS=ON", "-DGPR_BUILD_LEGACY_CLI=ON"],
                      {"bench_fused", "labs_encoder_bench_cli", "coeff_io_tool",
                       "vc5_encoder_app", "vc5_decoder_app"}, {"gpr_sdk", "dng_sdk"}),
        "codec_test": (camera + ["-DGPR_ENABLE_VIDEO=OFF", "-DGPR_BUILD_PREVIEW_TOOLS=OFF",
                                 "-DBUILD_TESTING=ON", "-DGPR_TEST_CODEC=ON"],
                       {"test_fused_band_roundtrip", "test_jans_inline_tail_flush"},
                       {"test_video_format", "test_labs_encoder_api", "ans_test", "bench_fused", "gpr_sdk"}),
        "preview_test": (camera + ["-DGPR_ENABLE_VIDEO=OFF", "-DBUILD_TESTING=ON",
                                   "-DGPR_TEST_PREVIEW=ON"],
                         {"gvid_preview_rgb_cli", "test_preview_fixture"},
                         {"test_fused_band_roundtrip", "test_labs_encoder_api", "ans_test", "bench_fused", "gpr_sdk"}),
    }
    for name, (flags, present, absent) in profiles.items():
        with tempfile.TemporaryDirectory(prefix="gpr-profile-") as scratch:
            build = Path(scratch)
            query = build / ".cmake/api/v1/query/codemodel-v2"
            query.parent.mkdir(parents=True)
            query.touch()
            run(["cmake", "-S", str(ROOT), "-B", str(build),
                 "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
                 "-DBUILD_TESTING=OFF", *flags])
            reply = build / ".cmake/api/v1/reply"
            model = json.loads(next(reply.glob("codemodel-v2-*.json")).read_text())
            targets = {t["name"] for c in model["configurations"] for t in c["targets"]}
            assert present <= targets, (name, "missing", present - targets)
            assert not absent & targets, (name, "unexpected", absent & targets)
            sources = {Path(c["file"]).name for c in
                       json.loads((build / "compile_commands.json").read_text())}
            if name == "stills":
                assert not sources & {"gpr_video.c", "gpr_labs_encoder.c", "gpr_video_format.c"}
            if name in ("codec_test", "preview_test"):
                assert not sources & {"gpr_video.c", "gpr_labs_encoder.c", "dng_host.cpp"}
            if name == "camera":
                assert not sources & {"dng_host.cpp", "gpr.cpp", "XMPMeta.cpp"}
            if name in ("default", "camera", "stills"):
                assert "bench_fused.c" not in sources
                assert "coeff_io_tool.c" not in sources
            if args.build:
                run(["cmake", "--build", str(build), "--config", "Release", "--parallel", "3"])
                if name in ("codec_test", "preview_test"):
                    run(["ctest", "--test-dir", str(build), "-C", "Release", "--output-on-failure"])
                for binary in present & {"gpr_tools", "gvid_preview_rgb_cli", "fused_decode_cli"}:
                    paths = list(build.rglob(binary))
                    assert any(p.is_file() and os.access(p, os.X_OK) for p in paths), binary
            print(f"PASS: {name} profile ({'built' if args.build else 'configured'})")
    for flags in (["-DGPR_BUILD_SDK=OFF"],
                  ["-DGPR_ENABLE_VIDEO=OFF", "-DGPR_BUILD_DIAGNOSTICS=ON"]):
        with tempfile.TemporaryDirectory(prefix="gpr-profile-invalid-") as scratch:
            result = subprocess.run(["cmake", "-S", str(ROOT), "-B", scratch, *flags],
                                    capture_output=True, text=True, timeout=60)
            assert result.returncode != 0 and "requires" in result.stderr, result.stderr
    print("PASS: contradictory profile requests rejected")


if __name__ == "__main__":
    main()
