from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


def without_disabled_blocks(source: str) -> str:
    return re.sub(r"#if\s+0\b.*?#endif", "", source, flags=re.DOTALL)


class JobMigrationContractTests(unittest.TestCase):
    def read(self, relative: str) -> str:
        return (ROOT / relative).read_text(encoding="utf-8")

    def test_review_export_preserves_cancel_and_progress(self):
        source = self.read("src/features/annotation/ReviewExport.cpp")
        header = self.read("src/features/annotation/ReviewExport.h")

        self.assertIn("const std::atomic_bool* cancel", header)
        self.assertIn("cancel->load(std::memory_order_relaxed)", source)
        self.assertIn("context.cancel()", source)
        self.assertIn("context.reportProgress", source)
        self.assertIn("ProcessOutcome", source)
        self.assertIn("JobState::Canceled", source)
        self.assertIn("JobState::TimedOut", source)
        self.assertNotIn("waitForFinished(", source)

    def test_review_download_uses_bounded_download_service(self):
        source = self.read("src/features/annotation/ReviewExport.cpp")

        self.assertIn("DownloadService::instance().downloadFileFromUrls", source)
        self.assertIn("kExtractTimeoutMs", source)
        self.assertNotIn("QNetworkAccessManager", source)
        self.assertIn("waitForInteractiveProcess", source)
        self.assertIn("QProgressDialog::canceled", source)
        self.assertIn("context.cancel()", source)

    def test_download_processes_use_job_context(self):
        source = self.read("src/services/component/DownloadService.cpp")

        self.assertIn("context.waitForProcess", source)
        self.assertGreaterEqual(source.count("waitForDownloadProcess(process, context)"), 3)
        self.assertIn("ProcessOutcome", source)
        self.assertIn("kDownloadTotalTimeoutMs", source)
        self.assertIn("kDownloadStallTimeoutMs", source)
        self.assertIn("context.shouldStop()", source)
        self.assertIn("&JobHandle::cancel", source)
        self.assertIn("context.reportProgress", source)
        self.assertNotIn("waitForFinished(", source)
        self.assertNotRegex(source, r"waitForFinished\s*\(\s*30\s*\*\s*60")

    def test_network_loops_have_stall_total_and_cancel_guards(self):
        source = self.read("src/services/component/DownloadService.cpp")

        self.assertGreaterEqual(source.count("QEventLoop loop"), 3)
        self.assertGreaterEqual(source.count("QTimer stallTimer"), 2)
        self.assertGreaterEqual(source.count("QTimer totalTimer"), 2)
        self.assertGreaterEqual(source.count("QTimer cancelPoll"), 2)
        self.assertGreaterEqual(source.count("reply->abort()"), 6)
        self.assertIn("JobRunner::start", source)
        self.assertIn("QProgressDialog::canceled", source)
        self.assertIn("&JobHandle::cancel", source)
        self.assertGreaterEqual(source.count("setTransferTimeout(kDownloadStallTimeoutMs)"), 2)

    def test_component_manager_has_one_active_download_owner(self):
        source = self.read("src/services/component/ComponentManager.cpp")
        active = without_disabled_blocks(source)

        self.assertIn("DownloadService::instance().downloadBytesFromUrls", active)
        self.assertIn("DownloadService::instance().downloadFileFromUrls", active)
        self.assertNotIn("QNetworkAccessManager", active)
        self.assertNotIn("waitForFinished(", active)
        self.assertIn("context.waitForProcess", active)
        self.assertIn("QProgressDialog::canceled", active)
        self.assertIn("context.cancel()", active)
        self.assertIn("QTimer stopPoll", active)
        self.assertIn("kExtractTimeoutMs", active)


if __name__ == "__main__":
    unittest.main()
