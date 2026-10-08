from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class TranscodeCacheContractTests(unittest.TestCase):
    def test_transcode_intermediate_paths_are_job_unique(self):
        source = (ROOT / "src/core/playback/PlaybackController.cpp").read_text(encoding="utf-8")

        self.assertIn("#include <QUuid>", source)
        self.assertIn("uniqueTranscodeTempPath", source)
        self.assertIn(
            'uniqueTranscodeTempPath(cachePath, QStringLiteral(".tmp.mov"))',
            source,
        )
        self.assertIn(
            'uniqueTranscodeTempPath(cachePath, QStringLiteral(".tmp.mp4"))',
            source,
        )
        self.assertNotIn(
            'cachePath + QStringLiteral(".tmp.mov")',
            source,
        )
        self.assertNotIn(
            'cachePath + QStringLiteral(".tmp.mp4")',
            source,
        )


if __name__ == "__main__":
    unittest.main()
