"""Registry consistency check.

This catches registry mistakes that would make a quality-gate run ambiguous:
unknown references, missing required fields, impossible ship classes, and
checkpoint metadata drift. Artifact completeness can be promoted from warnings
to errors with --strict-artifacts when preparing a production claim.

Run from CI or manually:
    python3 tests/quality_gates/check_registry_consistency.py
    python3 tests/quality_gates/check_registry_consistency.py --strict-artifacts
"""
import argparse
import hashlib
import json
import os
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))
from verify_production_artifacts import artifact_specs, selected_cnns
from validation_registry import load_validation_registry

REG = load_validation_registry()
GATES = json.loads((REPO / "tests/quality_gates/gates.json").read_text())

warnings = []
errors = []


def is_meta_key(name):
    return str(name).startswith("$")


def is_tbd(value):
    return isinstance(value, str) and "TBD" in value.upper()


def artifact_candidates(path_value):
    if not isinstance(path_value, str):
        return []
    path = Path(path_value)
    if path.is_absolute():
        return [path]

    roots: list[tuple[Path, bool]] = [(REPO, False)]
    for key in ("GPR_MODEL_ROOT", "GPR_CHECKPOINT_ROOT"):
        for item in os.environ.get(key, "").split(os.pathsep):
            if item:
                roots.append((Path(item), True))
    external_root = Path(os.environ.get("GPR_EXTERNAL_ROOT", REPO))
    roots.extend([
        (external_root, False),
        (external_root / "models", True),
        (external_root / "checkpoints", True),
    ])

    candidates = []
    for root, prefer_stripped in roots:
        stripped = None
        if path.parts and path.parts[0] in {"models", "checkpoints"}:
            stripped = root / Path(*path.parts[1:])
        if prefer_stripped and stripped is not None:
            candidates.append(stripped)
            candidates.append(root / path)
        else:
            candidates.append(root / path)
            if stripped is not None:
                candidates.append(stripped)
    unique = []
    seen = set()
    for candidate in candidates:
        key = str(candidate)
        if key not in seen:
            seen.add(key)
            unique.append(candidate)
    return unique


def resolve_artifact_path(path_value):
    candidates = artifact_candidates(path_value)
    for path in candidates:
        if path.exists():
            return path
    return candidates[0] if candidates else None


def sha256_file(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def artifact_issue(message, strict):
    if strict:
        errors.append(message)
    else:
        warnings.append(message)


ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument(
    "--strict-artifacts",
    action="store_true",
    help="treat missing checkpoints and TBD hashes as errors",
)
ap.add_argument("--registry", type=Path, help="registry to check (defaults to this worktree)")
args = ap.parse_args()
if args.registry:
    REG = json.loads(args.registry.read_text())
release_cnns = selected_cnns(REG)

known_ship_classes = set(GATES.get("ship_classes", {})) - {"$doc"}
known_codecs = set(REG.get("codecs", {}))
known_cnns = set(REG.get("cnns", {}))
known_demosaicers = set(REG.get("demosaicers", {}))

# Every codec must have use_for
for name, codec in REG.get("codecs", {}).items():
    if is_meta_key(name):
        continue
    if not isinstance(codec, dict):
        errors.append(f"codec {name!r}: entry must be an object")
        continue
    for field in ("binary", "quality"):
        if field not in codec:
            warnings.append(f"codec {name!r} missing {field!r} field")
    if "use_for" not in codec:
        warnings.append(f"codec {name!r} missing 'use_for' field")
    elif codec["use_for"] not in {"still", "video", "experiment", "deprecated"}:
        errors.append(f"codec {name!r} has invalid use_for={codec['use_for']!r}")




for name, cnn in REG.get("cnns", {}).items():
    if is_meta_key(name):
        continue
    if not isinstance(cnn, dict):
        errors.append(f"cnn {name!r}: entry must be an object")
        continue
    if name == "none":
        continue

    arch = cnn.get("cnn_arch_variant")
    if not arch:
        errors.append(f"cnn {name!r}: missing cnn_arch_variant")

    trained_against = cnn.get("trained_against_codec")
    if trained_against is None:
        errors.append(f"cnn {name!r}: missing trained_against_codec")
    elif is_tbd(trained_against):
        artifact_issue(
            f"cnn {name!r}: trained_against_codec is unresolved ({trained_against})",
            args.strict_artifacts,
        )
    elif trained_against not in known_codecs and trained_against != "any":
        errors.append(f"cnn {name!r}: trained_against_codec references unknown codec {trained_against!r}")

    if "raw_norm" not in cnn:
        errors.append(f"cnn {name!r}: missing raw_norm")

    if arch == "ycbcr_decomp":
        required = ("ckpt_y", "ckpt_cb", "ckpt_cr")
        missing = [field for field in required if field not in cnn]
        if missing:
            errors.append(f"cnn {name!r}: ycbcr_decomp missing {', '.join(missing)}")
    elif arch == "lab_chroma_corrector":
        if "ckpt_y" not in cnn:
            errors.append(f"cnn {name!r}: lab_chroma_corrector missing ckpt_y")
        if "ckpt_chroma" not in cnn and "ckpt_path" not in cnn:
            errors.append(f"cnn {name!r}: lab_chroma_corrector missing ckpt_chroma or ckpt_path")
    elif "ckpt_path" not in cnn and "expert_checkpoints" not in cnn:
        errors.append(f"cnn {name!r}: missing ckpt_path")

    for path_field, path_value, sha_field, expected_sha in artifact_specs(cnn):
        if name not in release_cnns:
            continue
        path = resolve_artifact_path(path_value)
        if path is None:
            errors.append(f"cnn {name!r}: {path_field} must be a string")
            continue

        if not path.exists():
            searched = artifact_candidates(path_value)[:4]
            suffix = f" (searched first: {', '.join(str(p) for p in searched)})" if searched else ""
            artifact_issue(
                f"cnn {name!r}: {path_field} does not exist: {path_value}{suffix}",
                args.strict_artifacts,
            )
            continue

        if expected_sha is None:
            artifact_issue(
                f"cnn {name!r}: missing {sha_field} for {path_field}",
                args.strict_artifacts,
            )
            continue
        if is_tbd(expected_sha):
            artifact_issue(
                f"cnn {name!r}: {sha_field} is unresolved ({expected_sha})",
                args.strict_artifacts,
            )
            continue
        if not isinstance(expected_sha, str) or len(expected_sha) != 64:
            artifact_issue(
                f"cnn {name!r}: {sha_field} is not a sha256 hex digest ({expected_sha!r})",
                args.strict_artifacts,
            )
            continue

        if path.is_absolute() and not str(path).startswith(str(REPO)):
            # External artifacts may not be mounted in CI. If present locally,
            # verify the hash; otherwise the missing-file warning above is enough.
            pass

        actual_sha = sha256_file(path)
        if actual_sha != expected_sha:
            errors.append(
                f"cnn {name!r}: {sha_field} mismatch for {path_field} "
                f"(expected {expected_sha}, got {actual_sha})"
            )

# Pipeline-level: still pipelines shouldn't reference video CNNs and vice-versa.
# Heuristic: CNN names containing 'sl_q3' or 'gpr_tools' = still-trained;
# CNN names containing 'ml2' or 'dmsr' or 'bido' = video-trained.
def cnn_intent(cnn_name):
    if cnn_name == "none":
        return "any"
    if "ml2" in cnn_name or "dmsr" in cnn_name or "bido" in cnn_name.lower():
        return "video"
    if "sl_q3" in cnn_name or "gpr_tools" in cnn_name:
        return "still"
    if "hh1x4" in cnn_name or "l1l2x4" in cnn_name:
        return "video"  # historical multi-level CNNs
    return "unknown"

for pname, pipe in REG.get("pipelines", {}).items():
    if is_meta_key(pname):
        continue
    if not isinstance(pipe, dict):
        errors.append(f"pipeline {pname!r}: entry must be an object")
        continue

    for field in ("codec", "cnn", "demosaic", "ship_class"):
        if field not in pipe:
            errors.append(f"pipeline {pname!r}: missing {field!r}")

    codec_name = pipe.get("codec")
    cnn_name = pipe.get("cnn", "none")
    demosaic_name = pipe.get("demosaic")
    ship_class = pipe.get("ship_class")

    if codec_name not in known_codecs:
        errors.append(f"pipeline {pname!r}: unknown codec {codec_name!r}")
        continue
    if cnn_name not in known_cnns:
        errors.append(f"pipeline {pname!r}: unknown cnn {cnn_name!r}")
        continue
    if demosaic_name not in known_demosaicers:
        errors.append(f"pipeline {pname!r}: unknown demosaic {demosaic_name!r}")
    if ship_class not in known_ship_classes:
        errors.append(f"pipeline {pname!r}: unknown ship_class {ship_class!r}")

    expected_name = f"codec={codec_name}+cnn={cnn_name}+demosaic={demosaic_name}"
    if pname != expected_name:
        errors.append(f"pipeline {pname!r}: key does not match canonical name {expected_name!r}")

    production_scope = pipe.get("production_scope")
    selection = pipe.get("release_selection")
    if selection is not None:
        if selection not in {"ship_root", "existing_offline", "production", "review_only", "historical"}:
            errors.append(f"pipeline {pname!r}: unknown release_selection {selection!r}")
        if (selection == "ship_root") != str(pipe.get("$role", "")).startswith("ship-"):
            errors.append(f"pipeline {pname!r}: ship root selection disagrees with legacy role")
        if selection == "production" and production_scope != "offline_production":
            errors.append(f"pipeline {pname!r}: production selection requires offline_production")
        if selection == "review_only" and production_scope != "offline_review_only":
            errors.append(f"pipeline {pname!r}: review selection requires offline_review_only")
    if production_scope is not None and production_scope not in {"offline_review_only", "offline_production"}:
        errors.append(f"pipeline {pname!r}: unknown production_scope {production_scope!r}")

    is_native12_sr = (
        str(codec_name).startswith("mission1_native12")
        and str(cnn_name).startswith("mission1_native12_8k_sr")
    )
    if is_native12_sr:
        doc = str(pipe.get("$doc", ""))
        use_for = str(pipe.get("use_for", ""))
        if production_scope not in {"offline_review_only", "offline_production"}:
            errors.append(
                f"pipeline {pname!r}: Mission 1 native12 8K SR must set "
                "production_scope='offline_review_only' or 'offline_production'"
            )
        if "execution_scope" in pipe and pipe["execution_scope"] != "offline":
            errors.append(f"pipeline {pname!r}: native12 SR execution_scope must be offline")
        if "execution_scope" not in pipe and "OFFLINE" not in use_for:
            errors.append(f"pipeline {pname!r}: Mission 1 native12 8K SR use_for must include OFFLINE")
        if "execution_scope" not in pipe and "not a live-camera path" not in doc:
            errors.append(
                f"pipeline {pname!r}: Mission 1 native12 8K SR doc must state "
                "it is not a live-camera path"
            )

    is_upresable_production = (pipe.get("release_selection") == "existing_offline"
                              if "release_selection" in pipe else
                              str(pipe.get("$role", "")).startswith("UPRESABLE production chain"))
    if is_upresable_production:
        doc = str(pipe.get("$doc", ""))
        if production_scope != "offline_review_only":
            errors.append(
                f"pipeline {pname!r}: UPRESABLE production chain must set "
                "production_scope='offline_review_only'"
            )
        if "execution_scope" in pipe:
            if pipe["execution_scope"] != "offline" or pipe.get("output_kind") != "editable_raw":
                errors.append(f"pipeline {pname!r}: existing offline chain must produce editable_raw offline")
        elif "editable" not in doc or "not a live-camera path" not in doc:
            errors.append(
                f"pipeline {pname!r}: UPRESABLE production doc must describe "
                "editable output and state it is not a live-camera path"
            )

    codec_use = REG["codecs"][codec_name].get("use_for", "unknown")
    cnn_use = cnn_intent(cnn_name)
    # still codec + video CNN, or vice-versa, is suspicious
    if codec_use == "still" and cnn_use == "video":
        warnings.append(f"pipeline {pname!r}: still codec paired with video CNN ({cnn_name})")
    if codec_use == "video" and cnn_use == "still":
        # acceptable as a cross-pair probe but flag it
        warnings.append(f"pipeline {pname!r}: video codec paired with still CNN ({cnn_name}) — cross-pair probe?")
    if codec_use == "deprecated":
        warnings.append(f"pipeline {pname!r}: uses deprecated codec {codec_name!r}")

print(f"=== registry consistency check ===")
print(
    f"codecs: {len(REG.get('codecs', {}))}  "
    f"cnns: {len(REG.get('cnns', {}))}  "
    f"pipelines: {len(REG.get('pipelines', {}))}"
)
print(f"strict_artifacts: {args.strict_artifacts}")
if warnings:
    print(f"\n{len(warnings)} warnings:")
    for w in warnings[:20]:
        print(f"  WARN: {w}")
    if len(warnings) > 20:
        print(f"  ... ({len(warnings) - 20} more)")
if errors:
    print(f"\n{len(errors)} ERRORS:")
    for e in errors:
        print(f"  ERROR: {e}")
    sys.exit(1)
print(f"\nOK" if not errors else "")
