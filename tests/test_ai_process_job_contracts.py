from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class AiProcessJobContractTests(unittest.TestCase):
    def read(self, relative: str) -> str:
        return (ROOT / relative).read_text(encoding="utf-8")

    def test_subtitle_generation_routes_process_waits_through_jobs(self):
        owner = self.read("src/services/ai/SubtitleGenerationService.cpp")
        support = self.read("src/services/ai/SubtitleGenerationSupport.cpp")
        source = owner + "\n" + support

        self.assertIn('#include "common/jobs/JobSystem.h"', source)
        self.assertGreaterEqual(source.count("job.waitForProcess(process, 25"), 3)
        self.assertGreaterEqual(source.count("ProcessOutcome"), 3)
        self.assertNotIn("waitForFinished(", source)

        # Protect the tuned quick-subtitle path while process ownership changes.
        self.assertIn("constexpr int kChunkSeconds = 4;", owner)
        self.assertIn("constexpr int kDefaultAsrConcurrency = 8;", owner)
        self.assertIn("constexpr int kDefaultTranslationConcurrency = 8;", owner)
        self.assertIn("constexpr qint64 kForwardPartialCoalesceMs = 650;", owner)

    def test_enhancement_workers_preserve_cancel_and_use_process_outcomes(self):
        source = "\n".join((
            self.read("src/services/ai/TranslationEnhancementScheduler.cpp"),
            self.read("src/services/ai/TranslationEnhancementSupport.cpp"),
        ))

        self.assertIn('#include "common/jobs/JobSystem.h"', source)
        self.assertGreaterEqual(source.count("waitForProcess(process, 25"), 3)
        self.assertGreaterEqual(source.count("waitForWorkerProcess(&process"), 2)
        self.assertIn("context.cancel()", source)
        self.assertIn("catch (const std::system_error&)", source)
        self.assertIn("monitorUnavailable", source)
        self.assertIn("JobState::Canceled", source)
        self.assertIn("JobState::TimedOut", source)
        self.assertIn("processOutcome.standardOutput", source)
        self.assertGreaterEqual(source.count("processOutcome.exitCode"), 4)
        self.assertNotIn("waitForFinished(", source)


if __name__ == "__main__":
    unittest.main()
