#!/usr/bin/env python3
"""Verify fused encoder and decoder quality preset tables stay in sync."""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
ENCODER = ROOT / "source/lib/vc5_encoder/fused_encode.c"
DECODER = ROOT / "source/lib/vc5_decoder/fused_decode.c"


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//.*", "", text)
    return text


def extract_initializer(text: str, name: str) -> str:
    marker = f"{name}[12][10]"
    start = text.find(marker)
    if start < 0:
        raise ValueError(f"missing table {name}")
    brace = text.find("{", start)
    if brace < 0:
        raise ValueError(f"missing initializer for {name}")

    depth = 0
    for idx in range(brace, len(text)):
        ch = text[idx]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return text[brace : idx + 1]
    raise ValueError(f"unterminated initializer for {name}")


def parse_rows(path: Path, table_name: str) -> list[list[int]]:
    text = strip_comments(path.read_text(encoding="utf-8"))
    init = extract_initializer(text, table_name)
    rows: list[list[int]] = []
    for row_text in re.findall(r"\{([^{}]+)\}", init):
        values = [int(value) for value in re.findall(r"-?\d+", row_text)]
        if values:
            rows.append(values)
    return rows


def main() -> int:
    encoder_rows = parse_rows(ENCODER, "quality_tables")
    decoder_rows = parse_rows(DECODER, "FUSED_QUALITY_TABLES")

    failures: list[str] = []
    if len(encoder_rows) != 12:
        failures.append(f"encoder has {len(encoder_rows)} quality rows, expected 12")
    if len(decoder_rows) != 12:
        failures.append(f"decoder has {len(decoder_rows)} quality rows, expected 12")

    for idx, (enc, dec) in enumerate(zip(encoder_rows, decoder_rows)):
        if len(enc) != 10:
            failures.append(f"encoder row {idx} has {len(enc)} values, expected 10")
        if len(dec) != 10:
            failures.append(f"decoder row {idx} has {len(dec)} values, expected 10")
        if enc != dec:
            failures.append(f"quality row {idx} mismatch: encoder={enc} decoder={dec}")

    if failures:
        print("Fused quality table check failed:", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1

    print("OK - fused quality tables match")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
