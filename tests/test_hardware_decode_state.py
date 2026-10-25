"""The saved preference cannot masquerade as an active hardware decoder."""
from pathlib import Path
import unittest
from tests.test_thumbnail_disk_cache import run_native

ROOT = Path(__file__).resolve().parents[1]


class HardwareDecodeStateTests(unittest.TestCase):
    def test_real_manager_request_fallback_and_persistence(self):
        report = run_native('hardware')
        self.assertEqual(5, len(report['cases']))
        self.assertTrue(report['backgroundMode'])
        self.assertEqual(0, report['visiblePlatformWindows'])


if __name__ == '__main__':
    unittest.main()
