#!/usr/bin/env python3
"""Guard project-owned release docs/code against prohibited sensitive content.

This is intentionally narrower than a repository-wide word ban. Vendored SDKs
and licenses can contain rights-related text, and project docs legitimately use
phrases like "runtime-legal source". The guard targets the specific restricted
claims and product-comparison language that must not appear in committed GPR
release materials.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path
from typing import Iterable


ROOT = Path(__file__).resolve().parents[2]

DEFAULT_INCLUDE = (
    "README.md",
    ".github",
    "docs",
    "pipelines",
    "tests/quality_gates",
    "tools/cnn",
    "tools/gvid_mov",
    "tools/gpr2prores",
    "tools/gvid_metadata.py",
    "tools/gvid_pack.py",
    "tools/test",
    "source/app",
    "source/lib/vc5_common",
    "source/lib/vc5_decoder",
    "source/lib/vc5_encoder",
)

EXCLUDE_PARTS = {
    ".git",
    "__pycache__",
    "build",
    "build-local",
    "source/app/common",
    "tools/test/check_sensitive_content.py",
}

TEXT_SUFFIXES = {
    "",
    ".c",
    ".cc",
    ".cpp",
    ".h",
    ".hpp",
    ".json",
    ".m",
    ".md",
    ".mm",
    ".patch",
    ".py",
    ".sh",
    ".txt",
    ".yml",
    ".yaml",
}

RESTRICTED_RIGHTS_WORD = r"\bpa" r"tents?\b"

PATTERNS = (
    (re.compile(RESTRICTED_RIGHTS_WORD, re.IGNORECASE), "restricted rights wording"),
    (re.compile(r"\bRED\s*[`'\"\u2018\u2019]?\s*384\b", re.IGNORECASE), "third-party still-image reference"),
    (re.compile(r"\bRED\s*[`'\"\u2018\u2019]?\s*967\b", re.IGNORECASE), "third-party raw-video reference"),
    (re.compile(r"\b384\b.{0,80}\b2034\b", re.IGNORECASE), "384/2034 expiry discussion"),
    (re.compile(r"\b967\b.{0,80}\b2028\b", re.IGNORECASE), "967/2028 expiry discussion"),
    (re.compile(r"\bpriority\s+20\d{2}\b", re.IGNORECASE), "restricted-priority discussion"),
    (re.compile(r"\bexpiry\s+(Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec)\b", re.IGNORECASE), "restricted-expiry discussion"),
    (re.compile(r"\bREDCODE\b", re.IGNORECASE), "product-comparison wording"),
    (re.compile(r"\bBRAW\b", re.IGNORECASE), "product-comparison wording"),
    (re.compile(r"\bProRes\s+RAW\b", re.IGNORECASE), "product-comparison wording"),
)

HISTORY_PATTERNS = PATTERNS[:7]
HISTORY_GREP_PATTERNS = (
    r"(^|[^[:alnum:]_])pa[t]ents?([^[:alnum:]_]|$)",
    r"(^|[^[:alnum:]_])RED[`'\"[:space:]]*384([^[:alnum:]_]|$)",
    r"(^|[^[:alnum:]_])RED[`'\"[:space:]]*967([^[:alnum:]_]|$)",
    r"(^|[^[:alnum:]_])384([^[:alnum:]_].{0,80})2034([^[:alnum:]_]|$)",
    r"(^|[^[:alnum:]_])967([^[:alnum:]_].{0,80})2028([^[:alnum:]_]|$)",
    r"(^|[^[:alnum:]_])priority[[:space:]]+20[0-9][0-9]([^[:alnum:]_]|$)",
    r"(^|[^[:alnum:]_])expiry[[:space:]]+(Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec)([^[:alnum:]_]|$)",
)

HISTORY_DIFF_GREP_PATTERN = "|".join(f"({pattern})" for pattern in HISTORY_GREP_PATTERNS)
HISTORY_GREP_BATCH_SIZE = 64


def is_excluded(path: Path) -> bool:
    rel = path.relative_to(ROOT).as_posix()
    return is_excluded_rel(rel)


def is_excluded_rel(rel: str) -> bool:
    parts = set(rel.split("/"))
    return bool(parts & EXCLUDE_PARTS) or any(rel == item or rel.startswith(item + "/") for item in EXCLUDE_PARTS)


def has_text_suffix(rel: str) -> bool:
    return Path(rel).suffix in TEXT_SUFFIXES


def iter_files(paths: list[Path]):
    for base in paths:
        if not base.exists():
            continue
        if base.is_file():
            if not is_excluded(base) and base.suffix in TEXT_SUFFIXES:
                yield base
            continue
        for path in base.rglob("*"):
            if path.is_file() and not is_excluded(path) and path.suffix in TEXT_SUFFIXES:
                yield path


def check_file(path: Path) -> list[str]:
    try:
        text = path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        text = path.read_text(encoding="utf-8", errors="ignore")

    findings: list[str] = []
    for line_no, line in enumerate(text.splitlines(), 1):
        for pattern, label in PATTERNS:
            if pattern.search(line):
                findings.append(f"{path.relative_to(ROOT)}:{line_no}: {label}: {line.strip()}")
    return findings


def history_pathspec(paths: Iterable[str]) -> list[str]:
    exclude_pathspec = [f":(exclude){item}" for item in EXCLUDE_PARTS]
    return [*paths, *exclude_pathspec]


def history_revs() -> list[str]:
    result = subprocess.run(
        ["git", "rev-list", "--all"],
        cwd=ROOT,
        check=True,
        stdout=subprocess.PIPE,
        text=True,
    )
    return [line.strip() for line in result.stdout.splitlines() if line.strip()]


def history_candidate_revs(paths: list[str]) -> list[str]:
    """Return commits whose diffs mention restricted text in project paths.

    Grepping every reachable tree is accurate but too slow for hosted CI on this
    repository. `git log -G` narrows the scan to commits that add/remove a
    matching line, then `check_history` verifies the resulting commit trees with
    the same line-level rules used by the old all-tree scan.
    """

    cmd = [
        "git",
        "log",
        "--all",
        "--no-ext-diff",
        "--regexp-ignore-case",
        "-G",
        HISTORY_DIFF_GREP_PATTERN,
        "--pretty=format:%H",
        "--",
        *history_pathspec(paths),
    ]
    result = subprocess.run(
        cmd,
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if result.returncode not in (0, 1):
        raise RuntimeError(result.stderr.strip() or "git log history scan failed")

    seen: set[str] = set()
    revs: list[str] = []
    for line in result.stdout.splitlines():
        rev = line.strip()
        if rev and rev not in seen:
            seen.add(rev)
            revs.append(rev)
    return revs


def check_history(paths: list[str]) -> list[str]:
    try:
        revs = history_candidate_revs(paths)
    except RuntimeError as exc:
        return [f"history scan failed: {exc}"]
    if not revs:
        return []

    findings: list[str] = []
    reported: set[tuple[str, str, str, str]] = set()
    for start in range(0, len(revs), HISTORY_GREP_BATCH_SIZE):
        batch = revs[start:start + HISTORY_GREP_BATCH_SIZE]
        cmd = ["git", "grep", "-n", "-I", "-i", "-E"]
        for pattern in HISTORY_GREP_PATTERNS:
            cmd.extend(["-e", pattern])
        cmd.extend(batch)
        cmd.extend(["--", *history_pathspec(paths)])

        result = subprocess.run(
            cmd,
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if result.returncode not in (0, 1):
            return [f"history scan failed: {result.stderr.strip() or 'git grep error'}"]

        for raw in result.stdout.splitlines():
            rev, sep, rest = raw.partition(":")
            if not sep:
                continue
            rel, sep, rest = rest.partition(":")
            if not sep or is_excluded_rel(rel) or not has_text_suffix(rel):
                continue
            line_no, sep, line = rest.partition(":")
            if not sep:
                continue
            for pattern, label in HISTORY_PATTERNS:
                if pattern.search(line):
                    key = (rev, rel, line_no, label)
                    if key in reported:
                        continue
                    reported.add(key)
                    findings.append(f"{rev[:12]}:{rel}:{line_no}: {label}: {line.strip()}")

    return findings


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--history",
        action="store_true",
        help="scan reachable project-owned history for restricted legal-risk wording",
    )
    parser.add_argument("paths", nargs="*", help="optional repo-relative paths to scan")
    args = parser.parse_args(argv)

    findings: list[str] = []
    if args.history:
        findings.extend(check_history(args.paths or list(DEFAULT_INCLUDE)))
    else:
        targets = [ROOT / path for path in args.paths] if args.paths else [ROOT / path for path in DEFAULT_INCLUDE]
        for path in sorted(set(iter_files(targets))):
            findings.extend(check_file(path))

    if findings:
        print("Sensitive-content guard failed:", file=sys.stderr)
        for item in findings:
            print(f"  {item}", file=sys.stderr)
        return 1

    mode = "history " if args.history else ""
    print(f"OK - {mode}sensitive-content guard passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
