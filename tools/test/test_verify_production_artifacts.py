#!/usr/bin/env python3
"""Smoke-test production artifact verification for checkpoints and SR pairs."""
from __future__ import annotations

import contextlib
import hashlib
import importlib.util
import io
import json
import os
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "verify_production_artifacts.py"


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def import_tool():
    spec = importlib.util.spec_from_file_location("verify_production_artifacts_smoke", TOOL)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


@contextlib.contextmanager
def patched_env(**updates: str):
    old = {key: os.environ.get(key) for key in updates}
    try:
        os.environ.update(updates)
        yield
    finally:
        for key, value in old.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value


def run_tool(module, registry: Path, *flags: str) -> tuple[int, dict]:
    old_registry = module.REGISTRY
    old_argv = sys.argv[:]
    stdout = io.StringIO()
    try:
        module.REGISTRY = registry
        sys.argv = ["verify_production_artifacts.py", "--strict", "--json", *flags]
        with contextlib.redirect_stdout(stdout):
            code = module.main()
    finally:
        module.REGISTRY = old_registry
        sys.argv = old_argv
    return code, json.loads(stdout.getvalue())


def main() -> int:
    work_parent = Path(os.environ.get("GPR_TMPDIR", os.environ.get("RUNNER_TEMP", tempfile.gettempdir())))
    work_parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="verify_production_artifacts_", dir=work_parent) as td:
        root = Path(td)
        external = root / "external"
        artifact_dir = external / "artifacts" / "sr"
        artifact_dir.mkdir(parents=True)

        checkpoint = artifact_dir / "model.pt"
        checkpoint.write_bytes(b"checkpoint fixture\n" * 8)
        pairs = artifact_dir / "pairs.npz"
        pairs.write_bytes(b"training pair fixture\n" * 8)

        registry = root / "registry.json"
        registry.write_text(json.dumps({
            "pipelines": {"fixture": {"cnn": "sr_fixture", "release_selection": "production"}},
            "cnns": {
                "sr_fixture": {
                    "ckpt_path": "artifacts/sr/model.pt",
                    "ckpt_sha256": sha256_file(checkpoint),
                    "training_pairs_path": "artifacts/sr/pairs.npz",
                    "training_pairs_sha256": sha256_file(pairs),
                }
            }
        }), encoding="utf-8")

        module = import_tool()
        with patched_env(GPR_EXTERNAL_ROOT=str(external)):
            code, payload = run_tool(module, registry, "--include-training-data")
            if code != 0 or payload.get("failures") != 0:
                print(f"expected verifier success, got code={code} payload={payload}", file=sys.stderr)
                return 1
            rows = payload.get("artifacts") or []
            if sum(1 for row in rows if row.get("path_field") == "training_pairs_path") != 1:
                print("expected one training_pairs_path artifact row", file=sys.stderr)
                return 1

            bad_registry = root / "bad_registry.json"
            bad = json.loads(registry.read_text())
            bad["cnns"]["sr_fixture"]["training_pairs_sha256"] = "0" * 64
            bad_registry.write_text(json.dumps(bad), encoding="utf-8")
            bad_code, bad_payload = run_tool(module, bad_registry, "--include-training-data")
            bad_rows = [
                row for row in (bad_payload.get("artifacts") or [])
                if row.get("path_field") == "training_pairs_path"
            ]
            if bad_code == 0 or not bad_rows or bad_rows[0].get("status") != "sha_mismatch":
                print(
                    "expected training_pairs_path hash mismatch to fail: "
                    f"code={bad_code} rows={bad_rows}",
                    file=sys.stderr,
                )
                return 1

            code, payload = run_tool(module, bad_registry)
            assert code == 0 and len(payload["artifacts"]) == 1, payload
            data = json.loads(registry.read_text())
            data["cnns"]["historical"] = {"ckpt_path": "missing-history.pt", "ckpt_sha256": "0" * 64}
            data["pipelines"]["old"] = {"cnn": "historical", "release_selection": "historical"}
            registry.write_text(json.dumps(data))
            assert run_tool(module, registry)[0] == 0
            code, payload = run_tool(module, registry, "--all")
            assert code == 1 and payload["artifacts"][-1]["status"] == "missing"
            checkpoint.write_bytes(b"corrupt")
            code, payload = run_tool(module, registry)
            assert code == 1 and payload["artifacts"][0]["status"] == "sha_mismatch"
            checkpoint.unlink()
            code, payload = run_tool(module, registry)
            assert code == 1 and payload["artifacts"][0]["status"] == "missing"

            for selection in ("ship_root", "existing_offline", "production", "review_only", "historical"):
                assert module.release_required({"release_selection": selection}) == (
                    selection in {"ship_root", "existing_offline", "production"})
            assert module.release_required({"$role": "ship-still"})
            assert module.release_required({"$role": "UPRESABLE production chain legacy"})
            assert module.release_required({"production_scope": "offline_production"})
            assert not module.release_required({"$role": "ship-old", "release_selection": "historical"})
            specs = module.artifact_specs({
                "router_sidecar_path": "router.json", "router_sidecar_sha256": "r",
                "expert_checkpoints": {"one": {"path": "expert.pt", "sha256": "e"}},
                "ckpt_rgb_detail": "detail.pt", "ckpt_rgb_detail_sha256": "d",
            })
            assert {s[0] for s in specs} == {"router_sidecar_path", "expert_checkpoints.one.path", "ckpt_rgb_detail"}

    print("test_verify_production_artifacts: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
