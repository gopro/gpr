#!/usr/bin/env python3
"""Run stock-video quality/rate experiments and verify every payload against stock.

Inputs are preloaded still-frame replays, not live sensor footage. Recordings
are removed after verification unless --keep-clips is set. Receipts persist.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

from stock_video import frames, read_header, sha256


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--binary", type=Path, required=True)
    p.add_argument("--reference", type=Path, required=True, help="parent stock vc5_encoder_app")
    p.add_argument("--input", type=Path, action="append", required=True)
    p.add_argument("--out-dir", type=Path, required=True)
    p.add_argument("--frames", type=int, default=120)
    p.add_argument("--long-frames", type=int, default=0, help="additional paced Q3/Q4 30fps runs")
    p.add_argument("--keep-clips", action="store_true")
    args = p.parse_args()
    args.out_dir.mkdir(parents=True, exist_ok=False)
    fixtures = [dict(name=path.name, sha256=sha256(path)) for path in args.input]
    reference_hashes = {}
    for q in range(6):
        reference_hashes[q] = []
        for i, path in enumerate(args.input):
            out = args.out_dir / f"reference-q{q}-{i}.vc5"
            env = {k: v for k, v in os.environ.items() if not k.startswith("GPR_")}
            env.update(GPR_BENCH_QUALITY=str(q))
            subprocess.run([str(args.reference.resolve()), "-i", str(path), "-o", str(out),
                            "-w", "4096", "-h", "3072", "-p", "8192"],
                           check=True, env=env, stdout=subprocess.DEVNULL)
            reference_hashes[q].append(sha256(out))
            out.unlink()
    configurations = [(q, 30, False, False, args.frames) for q in range(6)]
    configurations += [(q, fps, True, False, args.frames) for q in (0, 3, 4) for fps in (24, 30)]
    configurations += [(3, 30, True, True, args.frames)]
    if args.long_frames:
        configurations += [(q, 30, True, False, args.long_frames) for q in (3, 4)]
    binaries = dict(encoder_sha256=sha256(args.binary), reference_sha256=sha256(args.reference))
    with (args.out_dir / "summary.jsonl").open("w") as summaries:
        for q, fps, paced, sync_each, count_target in configurations:
            name = f"q{q}-{fps}fps-{'paced' if paced else 'unpaced'}-sync{int(sync_each)}-n{count_target}"
            clip = args.out_dir / (name + ".gvid")
            command = [str(args.binary.resolve()), "--output", str(clip), "--quality", str(q),
                       "--fps", str(fps), "--frames", str(count_target)]
            for path in args.input:
                command += ["--input", str(path)]
            if paced:
                command += ["--paced"]
            if sync_each:
                command += ["--fsync-each"]
            log = args.out_dir / (name + ".jsonl")
            with log.open("w") as f:
                subprocess.run(command, stdout=f, check=True)
            summary = json.loads(log.read_text().splitlines()[-1])
            summary.update(name=name, fixtures=fixtures, replay=True, cnn_in_capture=False,
                           reference_payload_sha256=reference_hashes[q], clip_sha256=sha256(clip), **binaries)
            count = 0
            with clip.open("rb") as f:
                h = read_header(f)
                for index, _, payload in frames(f, h):
                    if hashlib.sha256(payload).hexdigest() != reference_hashes[q][index % len(args.input)]:
                        raise RuntimeError("stock payload mismatch")
                    count += 1
            if count != count_target:
                raise RuntimeError("frame count mismatch")
            summary["verified_stock_frames"] = count
            summaries.write(json.dumps(summary) + "\n")
            summaries.flush()
            print(json.dumps(summary), flush=True)
            if not args.keep_clips:
                clip.unlink()


if __name__ == "__main__":
    main()
