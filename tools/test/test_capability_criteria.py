"""Coverage instrumentation must not turn performance omissions into passes."""
import unittest

import test_capabilities as capabilities


class CriteriaTest(unittest.TestCase):
    def test_instrumented_only_skips_performance(self):
        cap = {"criteria": {"encode_ms": {"max": 10}, "peak_rss_mb": {"max": 20},
                            "compress_ratio": {"max": 0.5}, "psnr_db": {"min": 40}}}
        measured = {"encode_ms": 100000, "peak_rss_mb": 100000,
                    "compress_ratio": 0.2, "psnr_db": 50}
        verdict, rows = capabilities.check_cap(cap, measured, instrumented=True)
        self.assertEqual(verdict, "PARTIAL")
        self.assertEqual([row[2] for row in rows], ["SKIPPED", "SKIPPED", "MET", "MET"])
        self.assertEqual(capabilities.check_cap(cap, measured, instrumented=False)[0], "FAILED")
        for metric, value in (("psnr_db", 30), ("compress_ratio", 0.9)):
            with self.subTest(metric=metric):
                self.assertEqual(capabilities.check_cap(
                    cap, dict(measured, **{metric: value}), instrumented=True)[0], "FAILED")
        self.assertEqual(cap["criteria"]["encode_ms"]["max"], 10)


if __name__ == "__main__":
    unittest.main()
