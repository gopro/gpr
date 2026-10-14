"""Synthetic runtime contracts only; these tests make no trained-quality claim."""
from pathlib import Path
import sys
import tempfile
import unittest

import numpy as np
import torch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "cnn"))

from mission1_sr_models import RAW_SCALE, make_model_from_config
from mission1_sr_runtime import (
    coord_channels,
    deinterleave,
    load_model,
    reinterleave_to_path,
    run_tiles,
)


class Mission1SRRuntimeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.previous_threads = torch.get_num_threads()
        torch.set_num_threads(1)
        cls.addClassCleanup(torch.set_num_threads, cls.previous_threads)
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.config = {
            "architecture": "coord_detail_preclean_adapter_pixelshuffle",
            "width": 4,
            "depth": 3,
            "residual_scale": 0.1,
        }
        # Nonzero synthetic heads exercise the network, not only its base resize.
        with torch.random.fork_rng(devices=[]):
            torch.manual_seed(17)
            original = make_model_from_config(cls.config)
            with torch.no_grad():
                for parameter in original.parameters():
                    parameter.uniform_(-0.05, 0.05)
        cls.state = original.state_dict()
        checkpoint = Path(cls.temp.name) / "synthetic.pt"
        torch.save({"config": cls.config, "model": cls.state}, checkpoint)
        cls.model, cls.loaded_config = load_model(checkpoint, torch.device("cpu"))

    def planes(self, height=10, width=12):
        return np.random.default_rng(23).integers(
            0, int(RAW_SCALE) + 1, size=(4, height, width), dtype=np.uint16
        )

    def run_runtime(self, planes, **changes):
        options = dict(
            model=self.model, planes=planes, device=torch.device("cpu"),
            tile=6, overlap=2, write_output=True,
            high_width=48, high_height=40, coordinate_channels=True,
        )
        options.update(changes)
        return run_tiles(**options)

    def test_checkpoint_load_is_strict_and_preserves_config(self):
        self.assertEqual(self.loaded_config, self.config)
        self.assertFalse(self.model.training)
        self.assertEqual(set(self.model.state_dict()), set(self.state))
        for name, value in self.model.state_dict().items():
            self.assertTrue(torch.equal(value, self.state[name]), name)
        incomplete = dict(self.state)
        incomplete.pop(next(iter(incomplete)))
        checkpoint = Path(self.temp.name) / "incomplete.pt"
        torch.save({"config": self.config, "model": incomplete}, checkpoint)
        with self.assertRaises(RuntimeError):
            load_model(checkpoint, torch.device("cpu"))

    def test_bayer_roundtrip_exact(self):
        raw = np.random.default_rng(5).integers(0, 65536, (20, 24), dtype=np.uint16)
        planes = deinterleave(raw)
        self.assertEqual(planes.shape, (4, 10, 12))
        for channel, (row, col) in enumerate(((0, 0), (0, 1), (1, 0), (1, 1))):
            np.testing.assert_array_equal(planes[channel], raw[row::2, col::2])
        path = Path(self.temp.name) / "roundtrip.raw"
        reinterleave_to_path(path, planes)
        np.testing.assert_array_equal(np.fromfile(path, dtype="<u2").reshape(raw.shape), raw)

    def test_coordinate_global_alignment(self):
        left = coord_channels(x0=0, y0=0, tile=6, low_plane_w=12, low_plane_h=10)
        right = coord_channels(x0=4, y0=4, tile=6, low_plane_w=12, low_plane_h=10)
        np.testing.assert_array_equal(left[:, 4:6, 4:6], right[:, :2, :2])
        self.assertEqual(left[0, 0, 0], -1.0)
        self.assertEqual(left[1, 0, 0], -1.0)
        edge = coord_channels(x0=6, y0=4, tile=6, low_plane_w=12, low_plane_h=10)
        self.assertEqual(edge[0, -1, -1], 1.0)
        self.assertEqual(edge[1, -1, -1], 1.0)

    def test_tiled_inference_finite_deterministic_and_globally_conditioned(self):
        planes = self.planes()
        inputs, predictions = [], []
        def observe(module, args, output):
            inputs.append(args[0].detach().clone())
            predictions.append(output.detach().clone())
        hook = self.model.register_forward_hook(observe)
        try:
            out, stats = self.run_runtime(planes)
        finally:
            hook.remove()
        self.assertEqual(out.shape, (4, 20, 24))
        self.assertEqual(out.dtype, np.uint16)
        self.assertEqual(stats["tile_count"], 6)
        self.assertTrue(all(np.isfinite(value) and value >= 0 for value in stats.values()))
        origins = [(x, y) for y in (0, 4) for x in (0, 4, 6)]
        self.assertEqual(len(inputs), len(origins))
        for tensor, prediction, (x, y) in zip(inputs, predictions, origins):
            self.assertEqual(tuple(prediction.shape), (1, 4, 12, 12))
            self.assertTrue(torch.isfinite(prediction).all().item())
            expected_raw = planes[:, y:y + 6, x:x + 6].astype(np.float32)
            expected_raw *= 1.0 / RAW_SCALE
            np.testing.assert_array_equal(tensor[0, :4].numpy(), expected_raw)
            coords = coord_channels(x0=x, y0=y, tile=6, low_plane_w=12, low_plane_h=10)
            np.testing.assert_array_equal(tensor[0, 4:].numpy(), coords)
        repeated, _ = self.run_runtime(planes)
        np.testing.assert_array_equal(out, repeated)
        no_output, timed = self.run_runtime(planes, write_output=False)
        self.assertIsNone(no_output)
        self.assertEqual(timed["tile_count"], stats["tile_count"])

    def test_single_tile_matches_direct_model_and_quantization(self):
        planes = self.planes(6, 6)
        patch = planes.astype(np.float32)
        patch *= 1.0 / RAW_SCALE
        coords = coord_channels(x0=0, y0=0, tile=6, low_plane_w=6, low_plane_h=6)
        with torch.inference_mode():
            prediction = self.model(torch.from_numpy(np.concatenate([patch, coords])[None]))
        expected = np.clip(prediction[0].numpy() * RAW_SCALE + 0.5, 0, 65535).astype(np.uint16)
        actual, stats = self.run_runtime(planes, high_width=24, high_height=24, overlap=0)
        np.testing.assert_array_equal(actual, expected)
        self.assertEqual(stats["tile_count"], 1)

    def test_invalid_inputs_fail_before_model_execution(self):
        cases = [
            {"planes": np.zeros((10, 12))},
            {"planes": np.zeros((3, 10, 12))},
            {"planes": np.zeros((4, 10, 12, 1))},
            {"planes": []},
            {"planes": np.zeros((4, 0, 12))},
            {"planes": np.zeros((4, 10, 0))},
            {"planes": self.planes(5, 12), "high_height": 20},
            {"planes": self.planes(10, 5), "high_width": 20},
            {"tile": 0}, {"tile": -1}, {"tile": 11},
            {"tile": 6.0}, {"tile": True},
            {"overlap": -1}, {"overlap": 6}, {"overlap": 7},
            {"overlap": 1.5}, {"overlap": False},
            {"high_width": 49}, {"high_height": 41},
            {"high_width": 44}, {"high_height": 36},
            {"high_width": 0}, {"high_height": -4},
            {"high_width": 48.0}, {"high_height": True},
        ]
        calls = []
        hook = self.model.register_forward_pre_hook(lambda module, args: calls.append(True))
        try:
            for write_output in (True, False):
                for changes in cases:
                    with self.subTest(changes=changes, write_output=write_output):
                        options = dict(planes=self.planes(), write_output=write_output)
                        options.update(changes)
                        with self.assertRaises(ValueError):
                            self.run_runtime(**options)
        finally:
            hook.remove()
        self.assertEqual(calls, [])


if __name__ == "__main__":
    unittest.main()
