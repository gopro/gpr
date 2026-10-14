#!/usr/bin/env bash
# Explicit macOS validation only; no weights, CMake builds, clean, or binary copies.
# Requires prebuilt Release codecs/bench_fused/gpr_tools and desktop dependencies.
# GPR_TEST_DESKTOP_BUILD_PROFILES=1 bash tools/test/test_desktop_build_profiles.sh
# GPR_RUNTIME_ROOT defaults to this checkout; BUILD_DIR defaults to ROOT/build.
# Leaves CNN_BACKENDS=none. The caller rebuilds both before standard output tests.
set -euo pipefail

if [ "${GPR_TEST_DESKTOP_BUILD_PROFILES:-0}" != 1 ]; then
  echo "SKIP: desktop profile builds require GPR_TEST_DESKTOP_BUILD_PROFILES=1"
  exit 0
fi
if [ "$(uname -s)" != Darwin ]; then
  echo "ERROR: desktop profile builds require macOS" >&2
  exit 1
fi

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
export GPR_RUNTIME_ROOT=${GPR_RUNTIME_ROOT:-"$REPO"}
export BUILD_DIR=${BUILD_DIR:-"$GPR_RUNTIME_ROOT/build"}

"${PYTHON_BIN:-python3}" - <<'PY'
import array
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

root = Path(os.environ["GPR_RUNTIME_ROOT"]).resolve()
build = Path(os.environ["BUILD_DIR"]).resolve()
desktop = root / "tools/gpr2prores"
renderer = desktop / "gpr2prores"
mov_tool = desktop / "gpr_mov_tool"
bench = build / "source/app/bench_fused/bench_fused"
gpr_tools = build / "source/app/gpr_tools/gpr_tools"


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


for tool in ("make", "xcrun", "nm", "otool", "ffprobe", "ffmpeg"):
    require(shutil.which(tool), f"Missing required tool: {tool}")
for binary in (bench, gpr_tools):
    require(binary.is_file() and os.access(binary, os.X_OK), f"Missing prebuilt tool: {binary}")
cache = build / "CMakeCache.txt"
require(cache.is_file() and re.search(r"^CMAKE_BUILD_TYPE:[^=]+=Release$",
                                     cache.read_text(), re.M),
        f"Expected preconfigured Release codecs in {build}")
require((desktop / "CNNConfig.h").is_file(), "Runtime checkout lacks optional CNN build support")

# Isolate model, codec-tuning and inherited make settings, including jobserver flags.
env = {k: v for k, v in os.environ.items()
       if not k.startswith(("GPR_", "FUSED_", "SUPERRES_", "CNN_"))
       and k not in ("MAKEFLAGS", "MFLAGS", "MAKEOVERRIDES", "MAKEFILES")}


def run(*args, expected=0, timeout=180, extra_env=None):
    result = subprocess.run([str(a) for a in args], cwd=root,
                            env=dict(env, **(extra_env or {})),
                            capture_output=True, text=True, timeout=timeout)
    require(result.returncode == expected,
            f"Command {args} returned {result.returncode}, expected {expected}:\n"
            f"{result.stdout}\n{result.stderr}")
    return result.stdout + result.stderr


def check_binary(binary, coreml, metal):
    # Darwin nm -gU reports defined external symbols, not mere ObjC references.
    defined = {line.split()[-1] for line in run("nm", "-gU", binary).splitlines()
               if line.split()}
    all_symbols = run("nm", "-g", binary)
    for name, enabled in (("SuperResCNN", coreml), ("SuperResMetal", metal)):
        symbol = "_OBJC_CLASS_$_" + name
        require((symbol in defined) == enabled, f"{binary.name}: wrong definition for {symbol}")
        if not enabled:
            require(symbol not in all_symbols, f"{binary.name}: disabled backend reference: {symbol}")
    linked = set(re.findall(r"/([^/]+)\.framework/", run("otool", "-L", binary)))
    for framework, enabled in (("CoreML", coreml),
                               ("MetalPerformanceShaders", metal),
                               ("MetalPerformanceShadersGraph", metal),
                               ("Accelerate", coreml or metal)):
        require((framework in linked) == enabled,
                f"{binary.name}: unexpected direct {framework} dependency state: {linked}")


with tempfile.TemporaryDirectory(prefix="gpr desktop profiles ") as directory:
    work = Path(directory)
    env["TMPDIR"] = str(work)
    raw, clip, metadata = work / "frame.raw", work / "clip.gvid", work / "metadata.dng"
    samples = array.array("H", (1024 + (x * 11 + y * 7) % 12000
                                for y in range(512) for x in range(512)))
    if sys.byteorder != "little":
        samples.byteswap()
    raw.write_bytes(samples.tobytes())
    run(bench, raw, 512, 512, 2,
        extra_env={"GPR_BENCH_GVID": str(clip), "GPR_BENCH_GVID_FPS": "24",
                   "GPR_BENCH_PIXEL_FORMAT": "4", "GPR_INCLUDE_LL": "1", "FUSED_QUALITY": "3"})
    run(gpr_tools, "-i", raw, "-w", 512, "-h", 512, "-x", "rggb16", "-o", metadata)
    base = ["--meta-dng", metadata, "--max-frames", "2", "--fps", "24"]
    missing = work / "nonexistent-checkpoint"
    baselines = {}

    def check_cli(coreml, metal):
        default = "coreml" if coreml else "metal" if metal else "none"
        help_text = run(renderer, "--help")
        require(f"coreml={'yes' if coreml else 'no'}, "
                f"metal/mpsgraph={'yes' if metal else 'no'}; default={default}" in help_text,
                "Help does not match the actual compiled profile")
        for backend, available in (("coreml", coreml), ("metal", metal), ("mpsgraph", metal)):
            for bypass in ([], ["--no-cnn"]):
                options = [*base, *bypass, "--cnn-backend", backend]
                if available:
                    # Positive parsing uses phase0 + bypass: it must not require weights.
                    text = run(renderer, *options, "--no-cnn", "--phase0", clip)
                    require("unavailable" not in text and "falling back" not in text,
                            f"Available backend {backend} was rejected or fell back")
                else:
                    text = run(renderer, *options, clip, work / "must-not-render.mov", expected=1)
                    require(f"--cnn-backend '{backend}' is unavailable in this build" in text,
                            f"Missing specific unavailable-backend error: {text}")
                    require("falling back" not in text.lower(), "Backend request silently fell back")
        for bypass in ([], ["--no-cnn"]):
            if not (coreml or metal):
                text = run(renderer, *base, *bypass, "--ckpt", missing, clip, expected=1)
                require("--ckpt requires CNN support" in text, "Disabled checkpoint accepted")
        require(not (work / "must-not-render.mov").exists(), "Rejected CLI rendered output")

    def render(label, bypass):
        # Compare decoded pixels, not MOV bytes (container timestamps may differ).
        for demosaic in ("metal-bilinear", "core-image"):
            output = work / f"{label}-{demosaic}.mov"
            text = run(renderer, *base, *bypass, "--demosaic", demosaic,
                       "--out-resolution", "8k", clip, output,
                       extra_env={"GPR_SUPER_RES_MLPACKAGE": str(missing),
                                  "GPR_MODEL_ROOT": str(missing)})
            require("pipeline:" in text and "errors=0" in text, f"Render did not complete: {text}")
            require("CNN[" not in text and "SuperResCNN:" not in text and "SuperResMetal:" not in text,
                    "No-CNN render reports restoration")
            probe = json.loads(run("ffprobe", "-v", "error", "-count_frames", "-select_streams", "v:0",
                                   "-show_streams", "-of", "json", output))
            stream, = probe["streams"]
            require(stream["codec_name"] == "prores" and int(stream["nb_read_frames"]) == 2
                    and stream["avg_frame_rate"] == "24/1"
                    and (stream["width"], stream["height"]) == (512, 512),
                    f"Unexpected synthetic ProRes output: {stream}")
            pixels = subprocess.run(["ffmpeg", "-v", "error", "-i", str(output), "-map", "0:v:0",
                                     "-threads", "1", "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
                                    env=env, capture_output=True, timeout=180)
            require(pixels.returncode == 0, pixels.stderr.decode(errors="replace"))
            require(len(pixels.stdout) == 2 * 512 * 512 * 3 and len(set(pixels.stdout)) > 1,
                    "Rendered pixels are blank or incomplete")
            digest = hashlib.sha256(pixels.stdout).hexdigest()
            if demosaic in baselines:
                require(baselines[demosaic] == digest, f"No-CNN output changed: {label}/{demosaic}")
            else:
                baselines[demosaic] = digest
            output.unlink()
            print(f"PASS: {label}/{demosaic}: two ProRes frames, decoded SHA256 {digest}", flush=True)

    for index, profile in enumerate(("none", "coreml", "metal", "both", "none")):
        print(f"BUILD {index + 1}/5: CNN_BACKENDS={profile} (no clean, jobs=3)", flush=True)
        # No inherited jobserver/unlimited -j; no alternate binary snapshots.
        command = ["make", "-C", str(desktop), "-j3", f"GPR_BUILD={build}"]
        subprocess.run([*command, f"CNN_BACKENDS={profile}"], env=env, check=True, timeout=1800)
        if index == 0:
            # Exercise the omitted/default flag too, before rendering without --no-cnn.
            subprocess.run(command, env=env, check=True, timeout=1800)
        coreml, metal = profile in ("coreml", "both"), profile in ("metal", "both")
        check_binary(renderer, coreml, metal)
        check_binary(mov_tool, False, False)
        check_cli(coreml, metal)
        if profile == "none":
            render(f"none-{index}", [])
        elif profile == "both":
            render("both-bypass", ["--no-cnn"])
        print(f"PASS: {profile} actual symbols, direct frameworks and CLI availability", flush=True)

print("PASS: none -> coreml -> metal -> both -> none; final build is none; no model quality claim")
PY
