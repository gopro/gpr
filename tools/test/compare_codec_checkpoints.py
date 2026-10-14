#!/usr/bin/env python3
"""Compare encoded and decoded bytes from two fused round-trip executables."""
import argparse
import hashlib
import json
import os
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
    parser.add_argument("--raw", type=Path, action="append", required=True)
    parser.add_argument("--width", type=int, required=True)
    parser.add_argument("--height", type=int, required=True)
    parser.add_argument("--quality", type=int, default=8)
    parser.add_argument("--pixel-format", type=int, default=1)
    parser.add_argument("--environment", type=Path, help="JSON object of explicit codec environment overrides")
    parser.add_argument("--receipt", type=Path)
    args = parser.parse_args()
    if args.width <= 0 or args.height <= 0:
        parser.error("dimensions must be positive")
    for raw in args.raw:
        if raw.stat().st_size != args.width * args.height * 2:
            parser.error(f"expected one unpacked uint16 Bayer frame: {raw}")
    env = {k: v for k, v in os.environ.items() if not k.startswith(("GPR_", "FUSED_"))}
    overrides = json.loads(args.environment.read_text()) if args.environment else {"GPR_INCLUDE_LL": "1"}
    if not isinstance(overrides, dict) or not all(
        isinstance(k, str) and k.startswith(("GPR_", "FUSED_")) and isinstance(v, str)
        for k, v in overrides.items()
    ):
        parser.error("environment must map GPR_/FUSED_ variable names to strings")
    env.update(overrides)
    env.update(FUSED_QUALITY=str(args.quality), GPR_BENCH_PIXEL_FORMAT=str(args.pixel_format))
    rows = []
    with tempfile.TemporaryDirectory(prefix="codec_checkpoint_") as directory:
        root = Path(directory)
        for raw in args.raw:
            row = {"input": raw.name, "input_sha256": digest(raw)}
            for label in ("baseline", "candidate"):
                encoded, decoded = root / f"{label}.fusd", root / f"{label}.raw"
                encoded.unlink(missing_ok=True)
                decoded.unlink(missing_ok=True)
                binary = getattr(args, label).resolve()
                result = subprocess.run(
                    [str(binary), str(raw.resolve()), str(args.width), str(args.height), str(decoded)],
                    env=dict(env, GPR_SAVE_ENC_TO=str(encoded)), capture_output=True, text=True,
                    check=True, timeout=300,
                )
                if not encoded.stat().st_size or not decoded.stat().st_size:
                    raise RuntimeError(f"{label} produced empty output")
                row[label] = {"binary_sha256": digest(binary),
                              "encoded_sha256": digest(encoded), "encoded_bytes": encoded.stat().st_size,
                              "decoded_sha256": digest(decoded), "decoded_bytes": decoded.stat().st_size,
                              "log": result.stdout + result.stderr}
            row["equal"] = all(row["baseline"][k] == row["candidate"][k]
                               for k in ("encoded_sha256", "encoded_bytes", "decoded_sha256", "decoded_bytes"))
            rows.append(row)
    receipt = {"schema": "gpr_checkpoint_equivalence.v1", "width": args.width, "height": args.height,
               "quality": args.quality, "pixel_format": args.pixel_format, "environment": overrides,
               "rows": rows, "all_equal": all(row["equal"] for row in rows)}
    payload = json.dumps(receipt, indent=2) + "\n"
    if args.receipt:
        args.receipt.parent.mkdir(parents=True, exist_ok=True)
        args.receipt.write_text(payload)
    print(payload)
    return 0 if receipt["all_equal"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
