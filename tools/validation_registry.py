"""Explicit validation-only union of supported and research pipeline records."""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def load_validation_registry(production=None, research=None):
    production = Path(production or ROOT / "pipelines/registry.json")
    research = Path(research or ROOT / "pipelines/research_registry.json")
    result = json.loads(production.read_text())
    extra = json.loads(research.read_text())
    for section in ("codecs", "cnns", "demosaicers", "pipelines"):
        target = result.setdefault(section, {})
        for name, value in extra.get(section, {}).items():
            if name in target:
                raise ValueError(f"duplicate {section} entry across registries: {name}")
            target[name] = value
    return result
