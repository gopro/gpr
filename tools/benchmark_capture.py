#!/usr/bin/env python3
"""Measure bounded stock capture replay; verify payloads, metadata and finalization."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

from stock_video import read_header, records, sha256


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--binary", type=Path, required=True)
    p.add_argument("--reference", type=Path, required=True, help="instrumented parent vc5_encoder_app")
    p.add_argument("--input", type=Path, action="append", required=True)
    p.add_argument("--out-dir", type=Path, required=True)
    p.add_argument("--frames", type=int, default=900)
    p.add_argument("--queue", type=int, action="append")
    p.add_argument("--quality", type=int, action="append")
    p.add_argument("--fsync-each", action="store_true")
    p.add_argument("--keep-clips", action="store_true")
    p.add_argument("--leased", action="store_true")
    args = p.parse_args()
    args.out_dir.mkdir(parents=True, exist_ok=False)
    inputs = [dict(name=path.name, sha256=sha256(path)) for path in args.input]
    hashes = {}
    qualities = args.quality or [3, 4]
    for q in qualities:
        hashes[q] = []
        for i, source in enumerate(args.input):
            output = args.out_dir / f"reference-q{q}-{i}.vc5"
            env = {k: v for k, v in os.environ.items() if not k.startswith("GPR_")}
            env["GPR_BENCH_QUALITY"] = str(q)
            subprocess.run([str(args.reference.resolve()), "-i", str(source), "-o", str(output),
                            "-w", "4096", "-h", "3072", "-p", "8192"],
                           env=env, check=True, stdout=subprocess.DEVNULL)
            hashes[q].append(sha256(output))
            output.unlink()
    with (args.out_dir / "summary.jsonl").open("w") as receipt:
        for q in qualities:
            for depth in args.queue or [1, 4]:
                name = f"q{q}-queue{depth}-n{args.frames}-sync{int(args.fsync_each)}"
                clip, log = args.out_dir / (name + ".gvid"), args.out_dir / (name + ".jsonl")
                command = [str(args.binary.resolve()), "--output", str(clip), "--quality", str(q),
                           "--queue", str(depth), "--frames", str(args.frames), "--paced"]
                if args.leased:
                    command += ["--leased"]
                for source in args.input:
                    command += ["--input", str(source)]
                if args.fsync_each:
                    command += ["--fsync-each"]
                with log.open("w") as output:
                    subprocess.run(command, stdout=output, check=True)
                summary = json.loads(log.read_text().splitlines()[-1])
                count = 0
                with clip.open("rb") as f:
                    header = read_header(f)
                    for row in records(f, header):
                        source_index = row["metadata"]["source_index"]
                        attempt_index = row["metadata"]["attempt_index"]
                        if (source_index != attempt_index % len(inputs) or
                                hashlib.sha256(row["payload"]).hexdigest() != hashes[q][source_index]):
                            raise RuntimeError("stock payload/source metadata mismatch")
                        count += 1
                if (not header["_end_marker_verified"] or count != summary["accepted"] or
                        count != summary["written"] or summary["attempts"] != count + summary["overruns"]):
                    raise RuntimeError("recording finalization/frame accounting mismatch")
                summary.update(name=name, inputs=inputs, verified_stock_frames=count,
                               reference_payload_sha256=hashes[q], clip_sha256=sha256(clip),
                               encoder_sha256=sha256(args.binary), reference_sha256=sha256(args.reference),
                               replay=True, cnn=False, preview=False, fps_target=30,
                               fsync_each=args.fsync_each, finalized=True)
                receipt.write(json.dumps(summary) + "\n")
                receipt.flush()
                print(json.dumps(summary), flush=True)
                if not args.keep_clips:
                    clip.unlink()


if __name__ == "__main__":
    main()
