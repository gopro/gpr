#!/usr/bin/env python3
"""Check synthetic camera metadata across DNG -> GPR -> DNG, without media files."""
import array
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile

import tifffile


def main():
    repo = Path(__file__).resolve().parents[2]
    codec = Path(os.environ.get("GPR_TOOLS", repo / "build/source/app/gpr_tools/gpr_tools"))
    if not codec.is_file():
        raise SystemExit(f"Missing gpr_tools: {codec}")

    def run(*args):
        result = subprocess.run([str(codec), *map(str, args)], capture_output=True,
                                text=True, check=True)
        return result.stdout

    def metadata(path):
        return json.loads(run("-i", path, "-d", "1"))

    def equal(actual, expected, label):
        if isinstance(expected, dict):
            for key, value in expected.items():
                equal(actual[key], value, f"{label}.{key}")
        elif isinstance(expected, list):
            assert len(actual) == len(expected), label
            for i, value in enumerate(expected):
                equal(actual[i], value, f"{label}[{i}]")
        else:
            assert math.isclose(actual, expected, rel_tol=1e-5, abs_tol=1e-5), (
                label, actual, expected)

    with tempfile.TemporaryDirectory(prefix="gpr_metadata_") as directory:
        root = Path(directory)
        raw = root / "source.raw"
        pixels = array.array("H", ((i * 31) % 12000 + 512 for i in range(128 * 128)))
        if sys.byteorder != "little":
            pixels.byteswap()
        raw.write_bytes(pixels.tobytes())
        for bits in (14, 16):
            for phase in ("rggb", "gbrg", "grbg", "bggr"):
                fmt = f"{phase}{bits}"
                seed = root / "seed.dng"
                run("-i", raw, "-w", 128, "-h", 128, "-x", fmt, "-o", seed)
                params = metadata(seed)
                tuning = params["tuning_info"]
                tuning["static_black_level"] = dict(zip(
                    tuning["static_black_level"], (64, 80, 96, 112)))
                white = 15000 if bits == 14 else 60000
                tuning["dgain_saturation_level"] = {
                    key: white for key in tuning["dgain_saturation_level"]}
                tuning["wb_gains"].update(r_gain=2.0, g_gain=1.0, b_gain=1.5)
                profile = params["profile_info"]
                profile["compute_color_matrix"] = False
                profile["color_matrix_1"] = [0.8, -0.1, 0.02, -0.2, 1.1, 0.1, 0.01, 0.1, 0.7]
                profile["color_matrix_2"] = [0.9, -0.1, 0.01, -0.1, 1.0, 0.1, 0.02, 0.1, 0.8]
                config = root / "params.json"
                config.write_text(json.dumps(params))
                source, encoded, decoded = (root / name for name in
                                             ("source.dng", "encoded.gpr", "decoded.dng"))
                run("-i", raw, "-w", 128, "-h", 128, "-x", fmt,
                    "-a", config, "-o", source)
                # The DNG SDK normalizes matrix rows when writing the source.
                # The codec round trip must preserve that canonical metadata.
                canonical = metadata(source)["profile_info"]
                seed_profile = metadata(seed)["profile_info"]
                for key in ("color_matrix_1", "color_matrix_2"):
                    assert canonical[key] != seed_profile[key], (fmt, key, "fixture used default matrix")
                run("-i", source, "-o", encoded)
                run("-i", encoded, "-o", decoded)
                for path in (source, encoded, decoded):
                    actual = metadata(path)
                    for key in ("static_black_level", "dgain_saturation_level", "wb_gains"):
                        equal(actual["tuning_info"][key], tuning[key], f"{fmt}:{path.name}:{key}")
                    for key in ("color_matrix_1", "color_matrix_2"):
                        equal(actual["profile_info"][key], canonical[key], f"{fmt}:{path.name}:{key}")
                    with tifffile.TiffFile(path) as image:
                        pages = list(image.pages)
                        patterns = []
                        while pages:
                            page = pages.pop()
                            tag = page.tags.get("CFAPattern")
                            if tag is not None:
                                patterns.append("".join("rgb"[i] for i in tag.value))
                            if page.pages is not None:
                                pages.extend(page.pages)
                        assert patterns == [phase], (fmt, path.name, patterns)
                print(f"PASS: {fmt} CFA, black/white levels, white balance, color matrices")


if __name__ == "__main__":
    main()
