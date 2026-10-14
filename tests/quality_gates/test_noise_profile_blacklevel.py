import sys
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "cnn"))

from analyze_dng_noise_profile import DngMeta, noise_sigma_map


def test_noise_sigma_map_accepts_three_value_black_level() -> None:
    raw = np.array(
        [
            [4100, 4110, 4200, 4210],
            [4120, 4130, 4220, 4230],
            [4300, 4310, 4400, 4410],
            [4320, 4330, 4420, 4430],
        ],
        dtype=np.float32,
    )
    meta = DngMeta(
        image_id="x2d",
        path=Path("x2d.dng"),
        iso=64,
        black=4096.0,
        black_levels=[4096.0, 4100.0, 4104.0],
        white=62914.0,
        white_levels=[62914.0],
        noise_profile=[2.0e-5, 3.0e-9, 2.1e-5, 3.2e-9, 2.2e-5, 3.4e-9],
        cfa_pattern=[0, 1, 1, 2],
        cfa_plane_color=["Red", "Green", "Blue"],
        make="Hasselblad",
        model="X2D 100C",
    )

    sigma = noise_sigma_map(raw, meta)

    assert sigma.shape == raw.shape
    assert np.all(np.isfinite(sigma))
    assert np.all(sigma > 0.0)
    assert sigma[0, 1] != sigma[0, 0]
    assert sigma[1, 1] != sigma[0, 1]
