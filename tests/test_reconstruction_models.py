"""Inference architecture smoke tests, not trained-model quality claims."""
import sys
import unittest
from pathlib import Path

import torch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools/cnn"))
import train_bayer_rgb_target_cleanup as cleanup
import mission1_sr_models as sr


class ReconstructionModels(unittest.TestCase):
    def test_cleanup_roundtrip(self):
        config = {"width": 8, "depth": 3, "residual_scale": 0.04}
        model = cleanup.make_model(config).eval()
        restored = cleanup.make_model(config).eval()
        restored.load_state_dict(model.state_dict(), strict=True)
        source = torch.rand(1, 4, 16, 16)
        with torch.inference_mode():
            expected, actual = model(source), restored(source)
        self.assertEqual(tuple(actual.shape), tuple(source.shape))
        torch.testing.assert_close(actual, expected, rtol=0, atol=0)
        self.assertTrue(torch.isfinite(actual).all())

    def test_sr_architectures(self):
        for architecture in (
            "residual_highres", "lowres_pixelshuffle", "resblock_pixelshuffle",
            "edge_pixelshuffle", "adapter_pixelshuffle",
            "green_detail_adapter_pixelshuffle", "preclean_adapter_pixelshuffle",
            "coord_preclean_adapter_pixelshuffle", "coord_detail_preclean_adapter_pixelshuffle",
            "coord_deep_preclean_adapter_pixelshuffle",
        ):
            with self.subTest(architecture=architecture):
                config = {"architecture": architecture, "width": 8, "depth": 3,
                          "residual_scale": 0.1}
                model = sr.make_model_from_config(config).eval()
                restored = sr.make_model_from_config(config).eval()
                restored.load_state_dict(model.state_dict(), strict=True)
                channels = 6 if sr.architecture_uses_coords(architecture) else 4
                source = torch.rand(1, channels, 16, 16)
                with torch.inference_mode():
                    expected, actual = model(source), restored(source)
                self.assertEqual(tuple(actual.shape), (1, 4, 32, 32))
                self.assertTrue(torch.isfinite(actual).all())
                torch.testing.assert_close(actual, expected, rtol=0, atol=0)


if __name__ == "__main__":
    torch.set_num_threads(1)
    unittest.main()
