#!/usr/bin/env python3
"""Compare native preview pixels from two builds using the same recorded clip."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--clip", type=Path, required=True)
    parser.add_argument("--width", type=int, required=True)
    parser.add_argument("--height", type=int, required=True)
    parser.add_argument("--frames", type=int, default=3)
    parser.add_argument("--receipt", type=Path, required=True)
    args = parser.parse_args()
    if min(args.width, args.height, args.frames) <= 0:
        parser.error("dimensions and frames must be positive")
    rows = {}
    with tempfile.TemporaryDirectory(prefix="gpr_preview_compare_") as scratch:
        for label in ("baseline", "candidate"):
            binary = getattr(args, label).resolve()
            output = Path(scratch) / label
            subprocess.run([str(binary), str(args.clip.resolve()), str(args.width),
                            str(args.height), str(output), str(args.frames), "1"],
                           check=True, timeout=600)
            receipt = json.loads((output / "receipt.json").read_text())
            assert receipt["frames"] == args.frames, receipt
            assert (receipt["width"], receipt["height"]) == (args.width // 4, args.height // 4)
            files = sorted((output / "frames_rgb").glob("frame_*.rgb"))
            assert len(files) == args.frames
            expected_bytes = (args.width // 4) * (args.height // 4) * 3
            assert all(path.stat().st_size == expected_bytes for path in files)
            rows[label] = {"binary_sha256": digest(binary), "timing": receipt["summary"],
                           "frame_sha256": [digest(path) for path in files]}
    equal = rows["baseline"]["frame_sha256"] == rows["candidate"]["frame_sha256"]
    result = {"schema": "gpr_preview_checkpoint_comparison.v1",
              "clip_sha256": digest(args.clip), "frames": args.frames,
              "width": args.width // 4, "height": args.height // 4,
              "identical_pixels": equal, "rows": rows,
              "scope": "RGB equivalence; short run includes frame writes, not display presentation."}
    args.receipt.parent.mkdir(parents=True, exist_ok=True)
    args.receipt.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"identical_pixels": equal, "frames": args.frames}))
    return 0 if equal else 1


if __name__ == "__main__":
    raise SystemExit(main())
