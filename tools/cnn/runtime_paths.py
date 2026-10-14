"""Portable defaults for CNN data and generated artifacts."""

import os
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


def external_path(relative: str = "") -> Path:
    root = Path(os.environ.get("GPR_EXTERNAL_ROOT", REPO)).expanduser()
    if not root.is_absolute():
        root = REPO / root
    return root / relative
