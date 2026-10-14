#!/usr/bin/env python3
"""Inventory a committed base..head diff; never infer execution from text matches.

Coverage inputs must come from the reported head and intended test-only run. JSON
exports do not establish either fact themselves. --coverage-root remaps an absolute
checkout path embedded by another machine. Percentages use only measured executable
changed lines, not all added lines. Ranges in output are inclusive [start, end].
--verify-checkout compares measured inventory files in --root byte-for-byte with
--head; run against the instrumented checkout before further source edits. This
checks current source identity, not profile freshness or build provenance.
"""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys


HEADERS = {".h", ".hh", ".hpp", ".hxx", ".inl", ".inc"}
SOURCES = {".c", ".cc", ".cpp", ".cxx", ".m", ".mm", ".metal", ".cu",
           ".rs", ".go", ".java", ".swift", ".incl_cpp", ".s"}
SCRIPTS = {".py", ".sh", ".bash", ".zsh", ".pl", ".rb", ".js", ".ts",
           ".jsx", ".tsx", ".ps1", ".cmake", ".bat"}


def git(root, *args):
    return subprocess.check_output(["git", "-C", str(root), *args])


def snapshot(root, ref):
    """Read committed blobs in one process, ignoring concurrent worktree edits."""
    entries = git(root, "ls-tree", "-rz", ref).split(b"\0")
    paths, oids = [], []
    for entry in filter(None, entries):
        meta, path = entry.split(b"\t", 1)
        mode, kind, oid = meta.split()
        name = os.fsdecode(path)
        text_file = (file_kind(name) is not None or Path(name).suffix.lower() in
                     {".md", ".txt", ".json", ".yml", ".yaml"} or
                     Path(name).name in {".gitignore", ".gitattributes"} or
                     not Path(name).suffix)
        if kind == b"blob" and mode != b"120000" and text_file:
            paths.append(os.fsdecode(path))
            oids.append(oid)
    raw = subprocess.check_output(["git", "-C", str(root), "cat-file", "--batch"],
                                  input=b"\n".join(oids) + b"\n")
    result, offset = {}, 0
    for path in paths:
        end = raw.index(b"\n", offset)
        size = int(raw[offset:end].split()[-1])
        blob = raw[end + 1:end + 1 + size]
        if b"\0" not in blob:
            result[path] = blob.decode("utf-8", errors="replace")
        offset = end + size + 2
    return result


def file_kind(path, content=""):
    p = Path(path)
    if p.suffix.lower() in HEADERS:
        return "header"
    if p.suffix.lower() in SOURCES:
        return "implementation"
    if (p.suffix.lower() in SCRIPTS or p.name in {"CMakeLists.txt", "Makefile", "Dockerfile"}
            or content.startswith("#!")):
        return "script"
    return None


def is_test(path):
    p = Path(path)
    return (bool({"test", "tests"} & set(p.parts)) or p.name.startswith("test_")
            or p.stem.endswith("_test"))


def feature(path):
    # Ownership-oriented heuristic, deliberately not a product support declaration.
    if is_test(path):
        return "validation"
    parts = Path(path).parts
    if len(parts) >= 3 and parts[0] == "source":
        return "/".join(parts[:3])
    if len(parts) >= 3 and parts[0] == "tools":
        return "/".join(parts[:2])
    return str(Path(path).with_suffix(""))


def ranges(lines):
    result = []
    for line in sorted(lines):
        if result and line == result[-1][1] + 1:
            result[-1][1] = line
        else:
            result.append([line, line])
    return result


def changes(root, base, head):
    fields = git(root, "-c", "diff.renameLimit=0", "diff", "--no-ext-diff", "--name-status", "-z", "--find-renames=50%",
                 base, head, "--").split(b"\0")
    i = 0
    while i < len(fields) and fields[i]:
        status, path = fields[i].decode(), os.fsdecode(fields[i + 1])
        i += 2
        old = path
        if status.startswith(("R", "C")):
            path = os.fsdecode(fields[i])
            i += 1
        patch = git(root, "--literal-pathspecs", "-c", "diff.renameLimit=0", "diff", "--no-ext-diff", "--no-textconv",
                    "--unified=0", "--find-renames=50%", "--diff-algorithm=myers",
                    "--no-indent-heuristic", base, head, "--", old, path).decode(
                        "utf-8", errors="replace")
        lines = set()
        removed = 0
        for match in re.finditer(r"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@", patch, re.M):
            start, count = int(match[3]), int(match[4] or 1)
            lines.update(range(start, start + count))
            removed += int(match[2] or 1)
        yield path, old, status, lines, removed


def static_refs(path, files):
    tokens = {path, Path(path).name}
    if Path(path).suffix == ".py":
        tokens.add(path[:-3].replace("/", "."))
        tokens.add(Path(path).stem)
    pattern = re.compile(r"(?<![\w./-])(?:" + "|".join(re.escape(t) for t in sorted(tokens))
                         + r")(?![\w./-])")
    return [{"path": name, "lines": ranges({i for i, line in enumerate(text.splitlines(), 1)
                                            if pattern.search(line)})}
            for name, text in sorted(files.items())
            if name != path and any(token in text for token in tokens) and pattern.search(text)]


def coverage_path(name, root, coverage_root, cwd=None):
    path = Path(name)
    if not path.is_absolute():
        path = Path(cwd or coverage_root or root) / path
    path = Path(os.path.normpath(path))
    for prefix in (coverage_root, root):
        if prefix:
            try:
                return path.relative_to(prefix).as_posix()
            except ValueError:
                pass
    return None  # Never guess by basename: duplicate headers are common.


def read_coverage(inputs, root, coverage_root):
    measured, executed, evidence = {}, {}, []
    for kind, filename in inputs:
        raw = Path(filename).read_bytes()
        data = json.loads(gzip.decompress(raw) if raw.startswith(b"\x1f\x8b") else raw)
        records = []
        if kind == "python":
            for name, info in data["files"].items():
                hits = set(info["executed_lines"])
                records.append((name, hits | set(info["missing_lines"]), hits, None))
        elif kind == "gcov":
            for info in data["files"]:
                rows = [r for r in info["lines"] if not r.get("gcovr/excluded", False)]
                records.append((info["file"], {r["line_number"] for r in rows},
                                {r["line_number"] for r in rows if r["count"] > 0},
                                data.get("current_working_directory")))
        else:
            if data["type"] != "llvm.coverage.json.export":
                raise ValueError("expected llvm-cov export JSON")
            for unit in data["data"]:
                for info in unit["files"]:
                    known, hits = set(), set()
                    segments = info["segments"]
                    for seg, end in zip(segments, segments[1:]):
                        line, column, count, has_count = seg[:4]
                        if not has_count or (len(seg) > 5 and seg[5]):
                            continue
                        # A segment ending at column 1 does not cover that line.
                        last = end[0] - (end[1] == 1)
                        span = set(range(line, last + 1))
                        known.update(span)
                        if count > 0:
                            hits.update(span)
                    records.append((info["filename"], known, hits, None))
        matched, unmatched = set(), set()
        for name, known, hits, cwd in records:
            if any(type(n) is not int or n < 1 for n in known):
                raise ValueError("coverage line numbers must be positive integers")
            path = coverage_path(name, root, coverage_root, cwd)
            if path is None:
                unmatched.add(name)
                continue
            matched.add(path)
            measured.setdefault(path, set()).update(known)
            executed.setdefault(path, set()).update(hits)
        evidence.append({"format": kind, "sha256": hashlib.sha256(raw).hexdigest(),
                         "mapped_files": len(matched), "unmapped_files": sorted(unmatched)})
    return measured, executed, evidence


def verify_source(root, head, path):
    source = root / path
    if source.is_symlink() or not source.is_file():
        raise ValueError(f"checkout verification failed for {path}: missing or non-regular source")
    if source.read_bytes() != git(root, "cat-file", "blob", head + ":" + path):
        raise ValueError(f"checkout verification failed for {path}: source differs from --head {head}")


def audit(args):
    root = Path(args.root).resolve()
    refs = {key: git(root, "rev-parse", "--verify", value + "^{commit}").decode().strip()
            for key, value in (("base", args.base), ("head", args.head), ("tests", args.test_ref))}
    head = snapshot(root, refs["head"])
    base = snapshot(root, refs["base"])
    tests = {p: t for p, t in snapshot(root, refs["tests"]).items() if is_test(p) and file_kind(p, t)}
    build_import = {p: t for p, t in head.items() if not is_test(p) and file_kind(p, t)}
    inputs = [(kind, p) for kind in ("llvm", "gcov", "python") for p in getattr(args, kind + "_json")]
    measured, executed, evidence = read_coverage(
        inputs, root, Path(args.coverage_root).resolve() if args.coverage_root else None)
    rows, verified = [], []
    for path, old, status, changed, removed in changes(root, refs["base"], refs["head"]):
        kind = file_kind(path, head.get(path, base.get(old, "")))
        if not kind:
            continue
        known = measured.get(path, set()) if status != "D" else set()
        if args.verify_checkout and known:
            verify_source(root, refs["head"], path)
            verified.append(path)
        hits = executed.get(path, set()) & known
        changed_known, changed_hits = changed & known, changed & hits
        state = "EXECUTED" if changed_hits else "NOT_EXECUTED" if changed_known else "UNKNOWN"
        rows.append({"path": path, "previous_path": old if old != path else None,
                     "change": status, "kind": kind, "feature": feature(path),
                     "changed_lines": ranges(changed), "removed_line_count": removed,
                     "direct_test_references_static_hints": static_refs(path, tests),
                     "build_import_references_static_hints": static_refs(path, build_import),
                     "build_or_import": "INSTRUMENTED" if known else "UNKNOWN",
                     "execution": {"status": "EXECUTED" if hits else "NOT_EXECUTED" if known else "UNKNOWN",
                                   "executed_lines": ranges(hits)},
                     "changed_line_coverage": {
                         "status": state, "measured_lines": len(changed_known),
                         "executed_lines": len(changed_hits),
                         "unmeasured_lines": len(changed - known),
                         "percent": round(100 * len(changed_hits) / len(changed_known), 2)
                         if changed_known else None},
                     "retention_review": "REVIEW_DELETION" if status == "D" else
                     "RETAIN_CANDIDATE" if changed_hits else "REVIEW_TEST_GAP" if changed_known else
                     "REVIEW_UNMEASURED"})
    result = {"schema_version": 1, "refs": refs, "comparison": "base..head (not merge-base)",
            "semantics": {
                "feature": "path-based ownership heuristic, not a support declaration",
                "static_hints": "literal path/basename/module references; may be comments; not execution evidence",
                "instrumented": "report contains measured lines; not proof of import or execution",
                "execution": "union of supplied reports; caller must ensure head and test-run provenance",
                "coverage": "percent of measured executable changed lines; unmeasured may be non-executable or uninstrumented",
                "retention": "review suggestions only; execution does not prove assertions or product necessity"},
            "coverage_inputs": evidence, "files": sorted(rows, key=lambda row: row["path"])}
    if args.verify_checkout:
        result["checkout_verification"] = {
            "status": "VERIFIED" if verified else "NO_MEASURED_FILES",
            "method": "worktree bytes equal head Git blob",
            "scope": "measured, non-deleted changed-runtime inventory files only",
            "verified_files": sorted(verified),
            "limitation": "current source identity only; profile freshness and build provenance are caller responsibilities"}
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=str(Path(__file__).resolve().parents[2]))
    parser.add_argument("--base", required=True, help="exact base commit/ref (use merge-base explicitly for PR scope)")
    parser.add_argument("--head", default="review/product-runtime")
    parser.add_argument("--test-ref", default="HEAD", help="committed test inventory ref, default validation HEAD")
    parser.add_argument("--coverage-root", help="original checkout root in coverage exports")
    parser.add_argument("--verify-checkout", action="store_true",
                        help="require measured inventory source bytes in --root to match --head")
    for kind in ("llvm", "gcov", "python"):
        parser.add_argument("--" + kind + "-json", action="append", default=[], help="repeatable coverage JSON (or gzip)")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    try:
        result = json.dumps(audit(args), sort_keys=True, separators=(",", ":")) + "\n"
        if args.output:
            args.output.write_text(result, encoding="utf-8")
        else:
            sys.stdout.write(result)
    except (OSError, ValueError, KeyError, TypeError, subprocess.CalledProcessError) as exc:
        parser.exit(2, "audit_feature_coverage: " + str(exc) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
