#!/usr/bin/env python3
"""Check portable documentation links and registered runtime entrypoints."""
from __future__ import annotations

import json
import fnmatch
import re
import subprocess
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]
PERSONAL_PATH = re.compile(r"/(?:Volumes|Users)/|/home/(?:pi)(?:/|\b)|192\.168\.\d+\.\d+")
LINK = re.compile(r"!?\[[^\]]*\]\((<[^>]+>|[^\s)]+)(?:\s+[^)]*)?\)")
MEDIA = {".ckpt", ".dng", ".gpr", ".gvid", ".html", ".jpeg", ".jpg",
         ".mlmodel", ".mov", ".mp4", ".npy", ".npz", ".png", ".pt", ".pth",
         ".raw", ".tif", ".tiff", ".onnx"}
ASSETS = ("docs/img/*", "data/readmegfx/*.png", "data/samples/*/*.GPR",
          "source/app/fuzz_decoder/corpus/*.gvid",
          "tests/conformance/inputs/*.raw")


class HTMLLinks(HTMLParser):
    def __init__(self):
        super().__init__()
        self.links = []

    def handle_starttag(self, tag, attrs):
        for key, value in attrs:
            if key in {"src", "href", "poster"} and value:
                self.links.append(value)


def local_target(document: Path, target: str):
    parsed = urlsplit(target.strip("<>"))
    if parsed.scheme or parsed.netloc or not parsed.path:
        return None
    return document.parent / unquote(parsed.path)


def inspect(root: Path, paths: list[str]) -> list[str]:
    errors = []
    for name in paths:
        path = root / name
        if not path.is_file():
            continue
        asset = any(fnmatch.fnmatchcase(name, pattern) for pattern in ASSETS)
        if path.suffix.lower() in MEDIA and not asset:
            errors.append(f"{name}: generated media/model outside fixture or README asset paths")
        if path.stat().st_size > (10_000_000 if asset else 1_000_000):
            errors.append(f"{name}: exceeds repository file-size budget")
        if set(Path(name).parts) & {"artifacts", "outputs", "tmp", "scratch"}:
            errors.append(f"{name}: generated output directory does not belong in source")
        try:
            text = path.read_text(encoding="utf-8")
        except (UnicodeError, OSError):
            continue
        for line, value in enumerate(text.splitlines(), 1):
            if PERSONAL_PATH.search(value):
                errors.append(f"{name}:{line}: machine-specific path or host")
        if path.suffix != ".md":
            continue
        prose = re.sub(r"```.*?```", "", text, flags=re.S)
        prose = re.sub(r"<!--.*?-->", "", prose, flags=re.S)
        prose = re.sub(r"`[^`]*`", "", prose)
        links = [m.group(1) for m in LINK.finditer(prose)]
        html = HTMLLinks()
        html.feed(prose)
        links.extend(html.links)
        for link in links:
            target = local_target(path, link)
            if target is not None and not target.exists():
                errors.append(f"{name}: missing link target {link}")

    registry_path = root / "pipelines/registry.json"
    if registry_path.exists():
        registry = json.loads(registry_path.read_text())

        def check_entries(value):
            if isinstance(value, dict):
                for key, child in value.items():
                    if key == "runtime_entrypoint" and isinstance(child, str):
                        if not (root / child).is_file():
                            errors.append(f"registry: missing runtime entrypoint {child}")
                    check_entries(child)
            elif isinstance(value, list):
                for child in value:
                    check_entries(child)

        check_entries(registry)
    return errors


def main():
    paths = subprocess.check_output(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
        cwd=ROOT,
    ).decode().split("\0")
    errors = inspect(ROOT, [p for p in paths if p])
    for error in errors:
        print(error)
    print(f"Repository portability: {len(errors)} error(s)")
    return bool(errors)


if __name__ == "__main__":
    raise SystemExit(main())
