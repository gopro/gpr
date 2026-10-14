#!/usr/bin/env python3
"""Lightweight Makefile checks using mock tools, no SDK/codecs/models needed."""

import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(os.environ.get("GPR_RUNTIME_ROOT", Path(__file__).resolve().parents[2])).resolve()


class BackendBuildTests(unittest.TestCase):
    def test_selections_and_switching(self):
        source = ROOT / "tools/gpr2prores"
        with tempfile.TemporaryDirectory(prefix="gpr-backends-") as tmp:
            root = Path(tmp)
            build = root / "gpr2prores"
            build.mkdir()
            for path in source.iterdir():
                if path.name == "Makefile" or path.suffix in (".m", ".h", ".metal"):
                    shutil.copy2(path, build / path.name)
            for name in ("src/gvid_mov.c", "include/gvid_mov.h"):
                path = root / "gvid_mov" / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()
            shim = root / "xcrun"
            shutil.copy2(__file__, shim)
            shim.chmod(0o755)
            env = dict(os.environ, PATH=str(root) + os.pathsep + os.environ["PATH"])
            command = [
                "make", "--no-print-directory", "-j3", "BUILD_TYPE=Release",
                "GPR_LIBS=", "LIBRAW_CFLAGS=", "LIBRAW_LIBS=",
                "FFMPEG_CFLAGS=", "FFMPEG_LIBS=",
                "CC=" + shlex.join([sys.executable, str(Path(__file__).resolve()),
                                   "--fake-tool"]),
            ]

            def make(*args):
                return subprocess.run(command + list(args), cwd=build, env=env,
                                      text=True, capture_output=True)

            def arguments(name):
                return json.loads((build / name).read_text())

            # Revisit old selections to catch stale artifacts in both directions.
            previous = None
            for selection in (None, "coreml", "metal", "both", "coreml", "none"):
                with self.subTest(selection=selection):
                    args = [] if selection is None else ["CNN_BACKENDS=" + selection]
                    result = make(*args)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    coreml = selection in ("coreml", "both")
                    metal = selection in ("metal", "both")
                    link = arguments("gpr2prores")
                    self.assertEqual("SuperResCNN.o" in link, coreml)
                    self.assertEqual("CoreML" in link, coreml)
                    self.assertEqual("SuperResMetal.o" in link, metal)
                    self.assertEqual("MetalPerformanceShadersGraph" in link, metal)
                    self.assertEqual("MetalPerformanceShaders" in link, metal)
                    self.assertEqual("Accelerate" in link, coreml or metal)
                    self.assertIn("Metal", link)
                    compile_args = arguments("GPRPipeline.o")
                    self.assertIn(f"-DGPR_HAVE_SUPER_RES_COREML={int(coreml)}", compile_args)
                    self.assertIn(f"-DGPR_HAVE_SUPER_RES_METAL={int(metal)}", compile_args)
                    library = arguments("default.metallib")
                    self.assertIn("Demosaic.air", library)
                    self.assertIn("Downscale.air", library)
                    self.assertEqual("SuperResPost.air" in library, metal)
                    self.assertEqual("NAFBlockKernels_LNConv.air" in library, metal)
                    self.assertEqual((build / "default.metallib").read_bytes(),
                                     (build / "Demosaic.metallib").read_bytes())
                    for framework in ("CoreML", "MetalPerformanceShadersGraph", "Accelerate"):
                        self.assertNotIn(framework, arguments("gpr_mov_tool"))
                    tracked = ("GPRPipeline.o", "gpr2prores", "default.metallib")
                    times = [(build / name).stat().st_mtime_ns for name in tracked]
                    if previous:
                        self.assertTrue(all(a > b for a, b in zip(times, previous)))
                    result = make(*args)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(times, [(build / name).stat().st_mtime_ns
                                             for name in tracked])
                    previous = times
            for invalid in ("bogus", "", "coreml metal", "%"):
                result = make("CNN_BACKENDS=" + invalid)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("CNN_BACKENDS must", result.stderr)

    @unittest.skipUnless(sys.platform == "darwin", "requires macOS SDK")
    def test_cli_and_pipeline_guards(self):
        source = ROOT / "tools/gpr2prores"
        with tempfile.TemporaryDirectory(prefix="gpr-cli-") as tmp:
            root = Path(tmp)
            # Only exercise argument parsing; do not link real readers/backends.
            stub = root / "stubs.m"
            stub.write_text('#import "DNGReader.h"\n#import "GPRPipeline.h"\n'
                            '@implementation DNGReader\n@end\n'
                            '@implementation GPRPipeline\n@end\n')
            for coreml, metal in ((0, 0), (1, 0), (0, 1), (1, 1)):
                with self.subTest(coreml=coreml, metal=metal):
                    flags = ["xcrun", "clang", "-fobjc-arc", "-Wall",
                             "-Wno-deprecated-declarations", "-Wno-nullability-completeness",
                             "-mmacosx-version-min=14.0", "-I", str(source),
                             f"-DGPR_HAVE_SUPER_RES_COREML={coreml}",
                             f"-DGPR_HAVE_SUPER_RES_METAL={metal}"]
                    result = subprocess.run(flags + ["-fsyntax-only",
                                            str(source / "GPRPipeline.m")],
                                            text=True, capture_output=True)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    binary = root / "cli"
                    result = subprocess.run(flags + ["-Wno-incomplete-implementation",
                                            str(source / "gpr2prores.m"), str(stub),
                                            "-framework", "Foundation", "-framework", "Metal",
                                            "-o", str(binary)], text=True, capture_output=True)
                    self.assertEqual(result.returncode, 0, result.stderr)

                    def cli(*args):
                        return subprocess.run([str(binary), *args], text=True,
                                              capture_output=True)

                    help_result = cli("--help")
                    self.assertEqual(help_result.returncode, 0)
                    default = "coreml" if coreml else "metal" if metal else "none"
                    self.assertIn("default=" + default, help_result.stdout)
                    for backend, available in (("coreml", coreml), ("metal", metal),
                                               ("mpsgraph", metal)):
                        for prefix in ([], ["--no-cnn"]):
                            result = cli(*prefix, "--cnn-backend", backend)
                            self.assertEqual(result.returncode, 1)
                            self.assertEqual("unavailable in this build" in result.stderr,
                                             not available)
                    result = cli("--ckpt", "missing", "--no-cnn")
                    self.assertEqual(result.returncode, 1)
                    self.assertEqual("--ckpt requires CNN support" in result.stderr,
                                     not (coreml or metal))


if __name__ == "__main__":
    if "--fake-tool" in sys.argv or Path(sys.argv[0]).name == "xcrun":
        args = sys.argv[1:]
        Path(args[args.index("-o") + 1]).write_text(json.dumps(args))
    else:
        unittest.main()
