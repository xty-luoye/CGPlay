"""Contracts for non-blocking media-open dispatch and stale-result protection."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class MediaOpenAsyncContractTests(unittest.TestCase):
    def test_open_file_dispatches_probe_to_job_runner(self):
        source = (ROOT / "src/ui/app/ApplicationMedia.cpp").read_text(encoding="utf-8")
        wrapper = source[source.index("void MainWindow::openFile"):source.index("void MainWindow::_openFileWithMediaInfo")]
        self.assertIn('JobRunner::start(', wrapper)
        self.assertIn('QStringLiteral("media.probe")', wrapper)
        self.assertNotIn("_p->mediaService->probe", wrapper)
        self.assertNotIn("MediaProbe::probe(path, sequenceFpsOverride);", wrapper)

    def test_probe_results_are_generation_guarded_and_cancelable(self):
        source = (ROOT / "src/ui/app/ApplicationMedia.cpp").read_text(encoding="utf-8")
        self.assertIn("++_p->mediaProbeGeneration", source)
        self.assertIn("generation != _p->mediaProbeGeneration", source)
        self.assertIn("_p->mediaProbeJob->cancel()", source)
        self.assertIn("_openFileWithMediaInfo(path, sequenceFpsOverride, *mediaInfo)", source)

    def test_resolved_open_path_keeps_existing_compatibility_dispatch(self):
        source = (ROOT / "src/ui/app/ApplicationMedia.cpp").read_text(encoding="utf-8")
        resolved = source[source.index("void MainWindow::_openFileWithMediaInfo"):]
        self.assertIn("playback->openFile(path, sequenceFpsOverride, mediaInfo.codecName)", resolved)
        self.assertIn("_p->timeline->setMediaPath(path)", resolved)
        self.assertIn("mediaInfo.durationSeconds", resolved)


if __name__ == "__main__":
    unittest.main()
