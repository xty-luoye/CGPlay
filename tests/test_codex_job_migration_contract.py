from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE_FILES = sorted((ROOT / "src/plugins/codex").glob("CodexPlugin*.cpp"))
HEADER = ROOT / "src/plugins/codex/CodexPlugin.h"
ORCHESTRATOR = ROOT / "src/plugins/codex/LocalCodexOrchestrator.cpp"
APP_SERVER_SESSION = ROOT / "src/services/agent/CodexAppServerSession.cpp"
JOB_SYSTEM = ROOT / "src/common/jobs/JobSystem.cpp"


class CodexJobMigrationContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = "\n".join(path.read_text(encoding="utf-8") for path in SOURCE_FILES)
        cls.header = HEADER.read_text(encoding="utf-8")
        cls.orchestrator = ORCHESTRATOR.read_text(encoding="utf-8")
        cls.app_server_session = APP_SERVER_SESSION.read_text(encoding="utf-8")
        cls.job_system = JOB_SYSTEM.read_text(encoding="utf-8")

    def test_process_tools_use_job_runner_and_cancelable_wait(self):
        self.assertIn('#include "common/jobs/JobSystem.h"', self.source)
        self.assertIn("JobRunner::start", self.source)
        self.assertIn("context.waitForProcess(process, 25)", self.source)
        for job_id in (
            "codex.workspace-archive",
            "codex.export-media",
            "codex.review-pack",
            "codex.selection-frame",
            "codex.selection-audio",
        ):
            self.assertIn(job_id, self.source)

    def test_async_process_jobs_have_real_lifecycle_handles(self):
        self.assertIn("QHash<QString, JobHandle*> _asyncToolJobs", self.header)
        self.assertIn("job->cancel()", self.source)
        self.assertNotIn("_asyncToolWatchers", self.source + self.header)
        self.assertNotIn("_asyncCancelFlags", self.source + self.header)

    def test_network_operations_share_bounded_job_wait(self):
        self.assertIn("NetworkJobResult waitForNetworkJob", self.source)
        self.assertIn("context.shouldStop()", self.source)
        self.assertIn("reply->abort()", self.source)
        self.assertIn("QSet<JobContext*> _activeNetworkJobs", self.header)
        self.assertIn("job->cancel()", self.source)
        self.assertNotIn("timeout.start(180000)", self.source)
        self.assertNotIn("downloadTimeout.start", self.source)
        self.assertNotRegex(
            self.source,
            re.compile(r"imageReply\s*=.*?loop\.exec\(\)", re.DOTALL),
        )

    def test_media_polling_is_bounded_and_nonblocking(self):
        self.assertIn("pollElapsed.elapsed() < 10 * 60 * 1000", self.source)
        self.assertIn("waitForJobDelay(mediaJob, 5000)", self.source)
        self.assertNotIn("QThread::msleep(5000)", self.source)

    def test_executable_probe_uses_bounded_job_context(self):
        probe = self.source[
            self.source.index("bool isRunnableCodexExecutable"):
            self.source.index("QString discoverCodexExecutable")
        ]
        self.assertIn("JobContext context(3000)", probe)
        self.assertIn("context.waitForProcess(probe, 25, 3000)", probe)
        self.assertIn("result.succeeded()", probe)
        self.assertNotIn("waitForFinished(", probe)

    def test_orchestrator_git_commands_share_bounded_job_wait(self):
        self.assertIn('#include "common/jobs/JobSystem.h"', self.orchestrator)
        self.assertIn("ProcessOutcome waitForGitProcess", self.orchestrator)
        self.assertIn("context.waitForProcess(process, 25, timeoutMs)", self.orchestrator)
        self.assertIn("process.error() == QProcess::FailedToStart", self.job_system)
        self.assertIn("result.exitCode = -1", self.job_system)
        self.assertIn("QString gitProcessError", self.orchestrator)
        self.assertIn("process.errorString().trimmed()", self.orchestrator)
        self.assertIn("Git process failed with exit code %1", self.orchestrator)
        self.assertGreaterEqual(self.orchestrator.count("waitForGitProcess("), 7)
        self.assertNotIn("waitForFinished(", self.orchestrator)

    def test_orchestrator_rejects_every_failed_diff_before_merge(self):
        merge = self.orchestrator[
            self.orchestrator.index("bool LocalCodexOrchestrator::mergeTask"):
        ]
        self.assertIn("if (!diffResult.succeeded())", merge)
        failure_guard = merge.index("if (!diffResult.succeeded())")
        patch_read = merge.index("const QByteArray patch = diffResult.standardOutput")
        branch_merge = merge.index('QStringLiteral("merge")')
        self.assertLess(failure_guard, patch_read)
        self.assertLess(failure_guard, branch_merge)

    def test_app_server_destructor_reaps_through_job_context(self):
        self.assertIn('#include "common/jobs/JobSystem.h"', self.app_server_session)
        self.assertIn("JobContext shutdownContext(400)", self.app_server_session)
        self.assertIn("shutdownContext.waitForProcess(_process, 25, 400, 50)", self.app_server_session)
        self.assertNotIn("waitForFinished(", self.app_server_session)

    def test_codex_sources_have_no_direct_process_waits(self):
        for source in (self.source, self.orchestrator, self.app_server_session):
            self.assertNotIn("waitForFinished(", source)


if __name__ == "__main__":
    unittest.main()
