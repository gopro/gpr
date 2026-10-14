#!/usr/bin/env python3
"""Extract one frame payload from a v1 .gvid stream."""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gvid_metadata import read_gvid_frames, sha256_gvid_payload  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("gvid", type=Path)
    ap.add_argument("output", type=Path)
    ap.add_argument("--frame-index", type=int, default=0)
    ap.add_argument("--receipt", type=Path)
    args = ap.parse_args()

    frames = read_gvid_frames(args.gvid)
    if args.frame_index < 0 or args.frame_index >= len(frames):
        raise IndexError(f"frame index {args.frame_index} out of range 0..{len(frames) - 1}")
    frame = frames[args.frame_index]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.gvid.open("rb") as src, args.output.open("wb") as dst:
        src.seek(int(frame["payload_offset"]))
        remaining = int(frame["payload_size"])
        while remaining:
            chunk = src.read(min(1024 * 1024, remaining))
            if not chunk:
                raise EOFError(f"{args.gvid} ended while extracting frame {args.frame_index}")
            dst.write(chunk)
            remaining -= len(chunk)
    receipt = {
        "schema": "gvid_frame_extract.v1",
        "gvid": str(args.gvid),
        "frame_count": len(frames),
        "frame_index": args.frame_index,
        "frame_tag": int(frame["frame_tag"]),
        "payload_offset": int(frame["payload_offset"]),
        "payload_size": int(frame["payload_size"]),
        "payload_sha256": sha256_gvid_payload(args.gvid, frame),
        "output": str(args.output),
    }
    if args.receipt:
        args.receipt.parent.mkdir(parents=True, exist_ok=True)
        args.receipt.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(receipt, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
