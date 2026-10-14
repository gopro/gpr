#!/usr/bin/env python3
"""Validate trailer-resolved production checkpoints and independent adoption.

Prerequisites: git, CMake, make, C/C++ compilers; PYTHON_BIN must select the
intended venv (numpy/tifffile for stills, torch/opencv for reconstruction). Desktop
requires pkg-config and FFmpeg development libraries plus ffprobe; macOS also
requires Xcode Metal tools and libraw. The ffmpeg checkpoint builds FFmpeg via
the current integration driver (network, zlib headers, nasm on x86 required).
External model quality and the desktop backend matrix are deliberately separate.
Use --scratch-parent on an external drive for Mac builds; --audit-only builds
nothing. No production checkout, ref, or worktree registration is modified.
"""

import argparse
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]
FEATURES = ("stills", "fused", "capture", "preview", "desktop", "ffmpeg",
            "reconstruction", "integration")
PARTIALS = {
    "stills-alone": ("stills",),
    "native-preview": FEATURES[:4],
    "desktop-reconstruction": ("stills", "fused", "capture", "desktop", "reconstruction"),
}


def run(args, cwd, env=None, timeout=1800):
    print("+ " + shlex.join(map(str, args)), flush=True)
    result = subprocess.run(list(map(str, args)), cwd=cwd, env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            timeout=timeout)
    print(result.stdout, end="", flush=True)
    if result.returncode:
        raise RuntimeError(f"command exited {result.returncode}: {args[0]}")
    return result.stdout


def git(repo, *args):
    return subprocess.check_output(["git", "-C", str(repo), *args], text=True)


def resolve_series(repo, ref):
    # Git's trailer parser, not body grep: prose mentioning a feature is not a tag.
    log = git(repo, "log", "--reverse", "--format=%H%x00%(trailers:key=GPR-Feature,valueonly)%x00", ref)
    fields = log.split("\0")
    commits = {}
    for index in range(0, len(fields) - 1, 2):
        commit, trailers = fields[index].strip(), fields[index + 1].splitlines()
        for feature in trailers:
            feature = feature.strip()
            if feature not in FEATURES:
                raise RuntimeError(f"unknown GPR-Feature trailer: {feature!r}")
            if feature in commits:
                raise RuntimeError(f"duplicate GPR-Feature trailer: {feature}")
            commits[feature] = commit
    if tuple(commits) != FEATURES:
        raise RuntimeError(f"expected ordered trailers {FEATURES}; found {tuple(commits)}")
    for before, after in zip(FEATURES, FEATURES[1:]):
        if git(repo, "rev-parse", commits[after] + "^").strip() != commits[before]:
            raise RuntimeError("feature series must be eight consecutive, non-merge commits")
    for commit in commits.values():
        if len(git(repo, "rev-list", "--parents", "-n1", commit).split()) != 2:
            raise RuntimeError("feature checkpoints must have exactly one parent")
    return commits


def tree(repo, ref):
    entries = {}
    for record in git(repo, "ls-tree", "-r", "-z", ref).split("\0"):
        if record:
            metadata, path = record.split("\t", 1)
            entries[path] = tuple(metadata.split())
    return entries


def audit_source(source, selected, snapshots, baseline):
    actual = tree(source, "HEAD")
    final = snapshots["integration"]
    previous = baseline
    selected_versions = dict(baseline)
    for feature in FEATURES:
        snapshot = snapshots[feature]
        if feature in selected:
            for path, entry in snapshot.items():
                if entry != previous.get(path):
                    selected_versions[path] = entry
            for path in previous.keys() - snapshot.keys():
                selected_versions.pop(path, None)
        introduced = snapshot.keys() - previous.keys()
        for path in introduced:
            if feature not in selected and path in actual:
                raise RuntimeError(f"excluded {feature} source leaked into partial: {path}")
            if feature in selected and path in final and path not in actual:
                raise RuntimeError(f"missing selected {feature} source: {path}")
        previous = snapshot
    # Build selections and staged documentation vary; runtime files must be final.
    for path in actual:
        if actual[path] == baseline.get(path):
            continue
        if path not in final:
            raise RuntimeError(f"non-production source in adoption tree: {path}")
        staged_metadata = (path.startswith(("docs/", ".github/"))
                           or Path(path).suffix.lower() in (".md", ".rst")
                           or path == ".gitignore")
        if staged_metadata and actual[path] != selected_versions.get(path):
            raise RuntimeError(f"metadata differs from selected checkpoint: {path}")
        if not staged_metadata and Path(path).name != "CMakeLists.txt" and actual[path] != final[path]:
            raise RuntimeError(f"non-final production file at checkpoint: {path}")
    print("PASS: selected source ownership and final runtime files", flush=True)


def copy_driver(source, relative):
    origin, destination = ROOT / relative, source / relative
    if not origin.is_file():
        raise RuntimeError(f"missing current validation driver: {origin}")
    if destination.exists():
        raise RuntimeError(f"validation driver would overwrite production: {relative}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(origin, destination)
    return destination


def executable(path):
    if not path.is_file() or not os.access(path, os.X_OK):
        raise RuntimeError(f"required executable missing: {path}")
    return path


def native(source, build, name, env):
    driver = copy_driver(source, f"source/app/{name}.c")
    binary = build / "adoption-tests" / name
    binary.parent.mkdir(exist_ok=True)
    includes = ["vc5_encoder", "vc5_decoder", "vc5_common", "common/private", "common/public"]
    libs = [build / f"source/lib/{lib}/lib{lib}.a"
            for lib in ("vc5_decoder", "vc5_encoder", "vc5_common", "common")]
    if sys.platform.startswith("linux"):
        libs = ["-Wl,--start-group", *libs, "-Wl,--end-group"]
    run([*shlex.split(os.environ.get("CC", "cc")), "-std=c99", "-O2",
         "-D_POSIX_C_SOURCE=200809L",
         *[f"-I{source / 'source/lib' / inc}" for inc in includes],
         driver, *libs, "-pthread", "-lm", "-o", binary], source, env)
    return executable(binary)


def python_test(source, relative, env, *args):
    return run([sys.executable, copy_driver(source, relative), *args], source, env)


def desktop(source, build, env, jobs):
    run(["make", "-C", "tools/gvid_mov", f"-j{jobs}"], source, env)
    pack = executable(source / "tools/gvid_mov/gvid_mov_pack")
    unpack = executable(source / "tools/gvid_mov/gvid_mov_unpack")
    fixture = native(source, build, "test_preview_fixture", env)
    work = build / "desktop-fixture"
    work.mkdir()
    raw, clip = work / "frame.raw", work / "clip.gvid"
    pixels = [(1024 + x * 11 + y * 7) % 12000 for y in range(256) for x in range(256)]
    raw.write_bytes(struct.pack(f"<{len(pixels)}H", *pixels))
    run([fixture, raw, 256, 256, 4, clip], source, env)
    frames = work / "frames"
    frames.mkdir()
    data, offset, payloads = clip.read_bytes(), 32, []
    for index in range(2):
        magic, size, tag = struct.unpack_from("<IIQ", data, offset)
        assert magic == 0x004D5246 and tag == index
        payload = data[offset + 16:offset + 16 + size]
        assert len(payload) == size and payload[:4] == b"FUSD"
        (frames / f"frame_{index:04d}.gpr").write_bytes(payload)
        payloads.append(payload)
        offset += 16 + size
    assert offset == len(data)
    decoded = work / "decoded.raw"
    run([executable(build / "bin/fused_decode_cli"), frames / "frame_0000.gpr",
         256, 256, decoded], source, env)
    assert decoded.stat().st_size == 256 * 256 * 2 and len(set(decoded.read_bytes())) > 1
    mov, extracted = work / "wrapped.mov", work / "extracted"
    run([pack, "--fps", 24, "--quality", 3, "--bit-depth", 16,
         "--black-level", 0, "--white-level", 65535, frames, mov], source, env)
    run([unpack, mov, extracted], source, env)
    assert [p.read_bytes() for p in sorted(extracted.glob("*.gpr"))] == payloads
    if sys.platform == "darwin":
        # Deliberately omit CNN_BACKENDS: adoption must exercise the default.
        run(["make", "-C", "tools/gpr2prores", f"-j{jobs}", f"GPR_BUILD={build}"], source, env)
        signature = (source / "tools/gpr2prores/.build-config").read_text().splitlines()
        assert signature[0] == "none", "desktop default must be no-CNN"
        metadata, output = work / "meta.dng", work / "review.mov"
        run([env["GPR_TOOLS"], "-i", raw, "-w", 256, "-h", 256,
             "-x", "rggb16", "-o", metadata], source, env)
        run([executable(source / "tools/gpr2prores/gpr2prores"), "--meta-dng", metadata,
             "--no-cnn", "--demosaic", "core-image", "--out-resolution", "2k",
             "--fps", 24, clip, output], source, env)
        info = json.loads(run(["ffprobe", "-v", "error", "-count_frames",
                              "-select_streams", "v:0", "-show_streams", "-of", "json",
                              output], source, env))
        stream, = info["streams"]
        assert stream["codec_name"] == "prores" and int(stream["nb_read_frames"]) == 2
    else:
        print("Desktop scope: portable MOV and fused decode; macOS renderer is covered on Mac.")


def exercise(source, build, selection, selected, jobs):
    partial = selection in PARTIALS
    video = not partial and "capture" in selected
    preview_tools = "fused" in selected
    diagnostics = selection == "ffmpeg"
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("GPR_", "FUSED_"))
           and k not in ("CNN_BACKENDS", "MAKEFLAGS", "MFLAGS", "PYTHONPATH", "PYTHONOPTIMIZE")}
    env.update(BUILD_DIR=str(build), GPR_BUILD=str(build), PYTHON_BIN=sys.executable,
               PY=sys.executable, PYTHONDONTWRITEBYTECODE="1", OMP_NUM_THREADS="1",
               TMPDIR=str(build.parent),
               GPR_TOOLS=str(build / "source/app/gpr_tools/gpr_tools"))
    run(["cmake", "-S", source, "-B", build, "-DCMAKE_BUILD_TYPE=Release",
         "-DCMAKE_POSITION_INDEPENDENT_CODE=ON", f"-DGPR_ENABLE_VIDEO={'ON' if video else 'OFF'}",
         f"-DGPR_BUILD_PREVIEW_TOOLS={'ON' if preview_tools else 'OFF'}",
         f"-DGPR_BUILD_DIAGNOSTICS={'ON' if diagnostics else 'OFF'}"], source, env)
    run(["cmake", "--build", build, "--parallel", jobs], source, env)
    executable(Path(env["GPR_TOOLS"]))
    if preview_tools:
        executable(build / "bin/fused_decode_cli")
    if "preview" in selected:
        executable(build / "bin/gvid_preview_rgb_cli")
    elif (build / "bin/gvid_preview_rgb_cli").exists():
        raise RuntimeError("preview target leaked into no-preview build")
    if not video:
        archive = build / "source/lib/vc5_encoder/libvc5_encoder.a"
        symbols = subprocess.check_output(["nm", "-g", str(archive)], cwd=source,
                                          env=env, text=True)
        if re.search(r"\b[TDB] _?(?:gpr_video_encoder|gpr_labs_encoder)\w*", symbols):
            raise RuntimeError("capture/API symbols leaked into video-disabled build")
    if selection in ("stills", "stills-alone", "integration"):
        python_test(source, "tools/test/test_still_metadata.py", env)
    if selection == "fused":
        test_env = dict(env, GPR_INCLUDE_LL="0", GPR_ROW_DECIMATE="1",
                        GPR_COL_DECIMATE="1", GPR_DROP_HIGHPASS="0", FUSED_INLINE_TOKENIZE="0")
        run([native(source, build, "test_fused_roundtrip", env)], source, test_env)
    if selection in ("capture", "integration"):
        for name in ("test_video_format", "test_labs_encoder_api", "test_video_encoder_abort",
                     "test_video_full_chain"):
            run([native(source, build, name, env)], source, env)
    if selection in ("preview", "native-preview"):
        fixture = native(source, build, "test_preview_fixture", env)
        python_test(source, "tools/test/test_native_preview.py", env,
                    "--fixture-bin", fixture, "--preview-bin", build / "bin/gvid_preview_rgb_cli")
    if selection in ("desktop", "desktop-reconstruction"):
        desktop(source, build, env, jobs)
    if selection in ("reconstruction", "desktop-reconstruction"):
        python_test(source, "tools/test/test_mission1_sr_runtime.py", env)
        python_test(source, "tests/test_reconstruction_models.py", env)
        python_test(source, "tools/test/test_bayer_resample.py", env)
    if selection == "ffmpeg":
        run(["make", "-C", "tools/gvid_mov", f"-j{jobs}"], source, env)
        python_test(source, "tools/test/test_ffmpeg_integration.py", env,
                    "--runtime-root", source, "--build", build, "--jobs", jobs,
                    "--scratch-parent", build.parent)
    if selection == "integration":
        driver = copy_driver(source, "tools/test/test_installed_sdk.sh")
        run(["bash", driver, "--build-tree", build], source, env)


def preflight(selections):
    for command in ("git", "cmake", "make", "nm"):
        if not shutil.which(command):
            raise RuntimeError(f"required tool not found: {command}")
    imports = set()
    if set(selections) & {"stills", "stills-alone", "integration"}:
        imports.update(("numpy", "tifffile"))
    if set(selections) & {"reconstruction", "desktop-reconstruction"}:
        imports.update(("numpy", "torch", "cv2"))
    if imports:
        run([sys.executable, "-c", "import " + ", ".join(sorted(imports))], ROOT)
    if set(selections) & {"desktop", "desktop-reconstruction", "ffmpeg"}:
        for command in ("pkg-config", "ffmpeg", "ffprobe"):
            if not shutil.which(command):
                raise RuntimeError(f"required tool not found: {command}")
        packages = ["libavformat", "libavcodec", "libavutil", "libswresample"]
        if sys.platform == "darwin" and set(selections) & {"desktop", "desktop-reconstruction"}:
            packages.append("libraw")
            run(["xcrun", "--find", "metal"], ROOT)
        run(["pkg-config", "--exists", *packages], ROOT)


def self_test():
    import unittest
    from unittest.mock import patch

    module = sys.modules[__name__]

    class HarnessTests(unittest.TestCase):
        def history(self, tags=FEATURES):
            # Synthetic identifiers are only mock data, never production refs.
            commits = {feature: f"commit-{i}" for i, feature in enumerate(FEATURES)}

            def fake_git(repo, *args):
                if args[0] == "log":
                    return "\n".join(f"{commits[f]}\0{f}\n\0" for f in tags)
                if args[0] == "rev-parse":
                    index = int(args[1].removesuffix("^").split("-")[1])
                    return f"commit-{index - 1}\n"
                if args[0] == "rev-list":
                    return f"{args[-1]} parent\n"
                raise AssertionError(args)

            return commits, fake_git

        def test_resolve_current_trailers(self):
            expected, fake = self.history()
            with patch.object(module, "git", side_effect=fake):
                self.assertEqual(resolve_series(Path("unused"), "HEAD"), expected)

        def test_missing_duplicate_and_reordered_trailers_fail(self):
            for tags in (FEATURES[:-1], (*FEATURES, "stills"), tuple(reversed(FEATURES))):
                with self.subTest(tags=tags):
                    _, fake = self.history(tags)
                    with patch.object(module, "git", side_effect=fake):
                        with self.assertRaises(RuntimeError):
                            resolve_series(Path("unused"), "HEAD")

        def test_partial_source_guard(self):
            snapshots, current = {}, {}
            for feature in FEATURES:
                current = dict(current, **{feature + ".c": ("100644", "blob", feature)})
                snapshots[feature] = current
            selected = PARTIALS["desktop-reconstruction"]
            partial = {f + ".c": snapshots[f][f + ".c"] for f in selected}
            with patch.object(module, "tree", return_value=partial):
                audit_source(Path("unused"), selected, snapshots, {})
            invalid = [dict(partial, **{"preview.c": snapshots["preview"]["preview.c"]}),
                       {p: v for p, v in partial.items() if p != "desktop.c"},
                       dict(partial, **{"desktop.c": ("100644", "blob", "old")})]
            for actual in invalid:
                with patch.object(module, "tree", return_value=actual):
                    with self.assertRaises(RuntimeError):
                        audit_source(Path("unused"), selected, snapshots, {})

        def test_missing_target_is_failure(self):
            with tempfile.TemporaryDirectory() as tmp:
                with self.assertRaises(RuntimeError):
                    executable(Path(tmp) / "missing")

        def test_documentation_matches_selected_checkpoint_not_future(self):
            path = "docs/GETTING_STARTED.md"
            early, final = ("100644", "blob", "stills-doc"), ("100644", "blob", "final-doc")
            snapshots = {feature: {path: early} for feature in FEATURES}
            snapshots["integration"] = {path: final}
            with patch.object(module, "tree", return_value={path: early}):
                audit_source(Path("unused"), ("stills",), snapshots, {})
            with patch.object(module, "tree", return_value={path: final}):
                with self.assertRaises(RuntimeError):
                    audit_source(Path("unused"), ("stills",), snapshots, {})

        def test_copy_driver_cannot_overwrite_runtime(self):
            with tempfile.TemporaryDirectory() as tmp:
                source = Path(tmp)
                relative = "tools/test/test_feature_series.py"
                copy_driver(source, relative)
                with self.assertRaises(RuntimeError):
                    copy_driver(source, relative)

    result = unittest.TextTestRunner(verbosity=2).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(HarnessTests))
    if not result.wasSuccessful():
        raise RuntimeError("harness self-tests failed")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", type=Path, default=ROOT, help="repository containing NEW trailers (default: validation repo)")
    parser.add_argument("--production-ref", "--ref", dest="ref", default="HEAD",
                        help="production ancestry tip containing the feature trailers (default: HEAD)")
    parser.add_argument("--selection", action="append",
                        help="comma-separated: " + ", ".join((*FEATURES, *PARTIALS))
                        + "; groups: all (default), checkpoints, partials")
    parser.add_argument("--jobs", type=int, default=int(os.environ.get("JOBS", "2")))
    parser.add_argument("--scratch-parent", type=Path, default=Path(tempfile.gettempdir()))
    parser.add_argument("--audit-only", action="store_true", help="resolve, compose, and audit source trees without builds")
    parser.add_argument("--self-test", action="store_true", help="test harness guards without builds or production history")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    if sys.flags.optimize:
        parser.error("Python assertions must be enabled (unset PYTHONOPTIMIZE; do not use -O)")
    if not 1 <= args.jobs <= 8:
        parser.error("--jobs must be 1..8")
    selections = []
    groups = {"all": (*FEATURES, *PARTIALS), "checkpoints": FEATURES, "partials": tuple(PARTIALS)}
    for item in args.selection or ["all"]:
        for name in item.split(","):
            for selection in groups.get(name, (name,)):
                if selection not in (*FEATURES, *PARTIALS):
                    parser.error(f"unknown selection: {selection}")
                if selection not in selections:
                    selections.append(selection)
    repo = args.repo.resolve()
    commits = resolve_series(repo, args.ref)
    print(json.dumps(commits, indent=2), flush=True)
    if not args.audit_only:
        preflight(selections)
    snapshots = {feature: tree(repo, commit) for feature, commit in commits.items()}
    base = git(repo, "rev-parse", commits["stills"] + "^").strip()
    baseline = tree(repo, base)
    args.scratch_parent.mkdir(parents=True, exist_ok=True)
    for selection in selections:
        selected = PARTIALS.get(selection, FEATURES[:FEATURES.index(selection) + 1]
                                if selection in FEATURES else ())
        with tempfile.TemporaryDirectory(prefix=f"gpr-adoption-{selection}-", dir=args.scratch_parent) as tmp:
            work = Path(tmp)
            source, build = work / "source", work / "build"
            run(["git", "clone", "--quiet", "--shared", "--no-checkout", repo, source], work)
            run(["git", "checkout", "--quiet", "--detach",
                 base if selection in PARTIALS else commits[selection]], source)
            if selection in PARTIALS:
                run(["git", "-c", "user.name=Adoption Test", "-c", "user.email=adoption@example.invalid",
                     "-c", "commit.gpgSign=false", "cherry-pick", *[commits[f] for f in selected]], source)
            audit_source(source, selected, snapshots, baseline)
            if not args.audit_only:
                exercise(source, build, selection, selected, args.jobs)
                if git(source, "diff", "HEAD", "--"):
                    raise RuntimeError("tests modified tracked production content")
            print(f"PASS: {selection} {'source audit only (not build evidence)' if args.audit_only else 'adoption'}", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.SubprocessError, OSError) as error:
        raise SystemExit(f"FAIL: {error}")
