from pathlib import Path
import os
import subprocess
import textwrap
import unittest


ROOT = Path(__file__).resolve().parents[1]
ARTIFACT = ROOT / "tests" / "artifacts" / "architecture_10_20260726" / "codex_wait_migration" / "job_runtime"


class JobSystemContractTests(unittest.TestCase):
    def read(self, relative: str) -> str:
        return (ROOT / relative).read_text(encoding="utf-8")

    def test_job_contract_covers_lifecycle_and_errors(self):
        header = self.read("src/common/jobs/JobSystem.h")
        for state in ("Pending", "Running", "Succeeded", "Failed", "Canceled", "TimedOut"):
            self.assertIn(state, header)
        self.assertIn("errorCode", header)
        self.assertIn("errorMessage", header)
        self.assertIn("progressChanged", header)
        self.assertIn("void cancel()", header)

    def test_runner_guards_owner_lifetime(self):
        source = self.read("src/common/jobs/JobSystem.cpp")
        self.assertIn("QPointer<JobHandle>", source)
        self.assertIn("Qt::QueuedConnection", source)
        self.assertIn("new JobHandle(id, owner)", source)
        self.assertIn("context->isCancellationRequested()", source)
        self.assertIn("context->hasTimedOut()", source)

    def test_process_waits_are_bounded_and_cancelable(self):
        source = self.read("src/common/jobs/JobSystem.cpp")
        self.assertIn("process.terminate()", source)
        self.assertIn("process.kill()", source)
        self.assertIn("operationTimeoutMs", source)
        self.assertIn("terminationGraceMs", source)
        self.assertIn("QProcess::FailedToStart", source)

        playback = self.read("src/core/playback/PlaybackController.cpp")
        probe = self.read("src/services/media/MediaProbe.cpp")
        thumbnails = self.read("src/services/media/ThumbnailService.cpp")
        self.assertNotIn("waitForFinished(-1)", playback)
        self.assertIn("JobRunner::start", playback)
        self.assertIn("context.waitForProcess", probe)
        self.assertIn("context.waitForProcess", thumbnails)

    def test_playback_transcode_is_generation_guarded(self):
        source = self.read("src/core/playback/PlaybackController.cpp")
        self.assertIn("openGeneration", source)
        self.assertIn("generation != _p->openGeneration", source)
        self.assertIn("_p->transcodeJob->cancel()", source)
        self.assertIn("30 * 60 * 1000", source)


class JobSystemRuntimeContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        ARTIFACT.mkdir(parents=True, exist_ok=True)
        source_dir = ARTIFACT / "contract_source"
        build_dir = ARTIFACT / "contract_build"
        source_dir.mkdir(exist_ok=True)
        runner = source_dir / "job_system_contract_runner.cpp"
        cmake = source_dir / "CMakeLists.txt"
        runner.write_text(
            textwrap.dedent(
                r'''
                #include "common/jobs/JobSystem.h"
                #include <QCoreApplication>
                #include <QElapsedTimer>
                #include <QProcess>

                using namespace cgplay;
                #define REQUIRE(x) do { if (!(x)) return __LINE__; } while (false)

                ProcessOutcome runCmd(const QString& command, int timeoutMs = 5000) {
                    QProcess process;
                    process.setProcessChannelMode(QProcess::SeparateChannels);
                    process.start("cmd.exe", {"/d", "/s", "/c", command});
                    JobContext context(timeoutMs);
                    return context.waitForProcess(process, 10, timeoutMs, 50);
                }

                int main(int argc, char** argv) {
                    QCoreApplication app(argc, argv);

                    const ProcessOutcome success = runCmd("echo stdout-text & echo stderr-text 1>&2");
                    REQUIRE(success.succeeded());
                    REQUIRE(success.standardOutput.contains("stdout-text"));
                    REQUIRE(success.standardError.contains("stderr-text"));

                    const ProcessOutcome nonzero = runCmd("exit /b 7");
                    REQUIRE(nonzero.state == JobState::Failed);
                    REQUIRE(nonzero.exitCode == 7);

                    QProcess missing;
                    missing.start("Z:/cgplay-missing/codex-process.exe", {});
                    JobContext missingContext(1000);
                    const ProcessOutcome missingResult = missingContext.waitForProcess(missing, 10, 1000, 50);
                    REQUIRE(missingResult.state == JobState::Failed);
                    REQUIRE(missingResult.exitCode == -1);
                    REQUIRE(!missingResult.standardError.isEmpty());

                    QProcess slow;
                    slow.start("powershell.exe", {"-NoProfile", "-Command", "Start-Sleep -Seconds 10"});
                    JobContext timeoutContext(100);
                    QElapsedTimer elapsed;
                    elapsed.start();
                    const ProcessOutcome timeoutResult = timeoutContext.waitForProcess(slow, 10, 100, 50);
                    REQUIRE(timeoutResult.state == JobState::TimedOut);
                    REQUIRE(elapsed.elapsed() < 500);
                    REQUIRE(slow.state() == QProcess::NotRunning);
                    return 0;
                }
                '''
            ),
            encoding="utf-8",
        )
        job_source = ROOT / "src" / "common" / "jobs" / "JobSystem.cpp"
        job_header = ROOT / "src" / "common" / "jobs" / "JobSystem.h"
        cmake.write_text(
            "cmake_minimum_required(VERSION 3.21)\n"
            "project(CGPlayJobSystemContracts LANGUAGES CXX)\n"
            "set(CMAKE_CXX_STANDARD 17)\n"
            "set(CMAKE_AUTOMOC ON)\n"
            "find_package(Qt6 REQUIRED COMPONENTS Core Concurrent)\n"
            f'add_executable(job_system_contract_runner job_system_contract_runner.cpp "{job_source.as_posix()}" "{job_header.as_posix()}")\n'
            f'target_include_directories(job_system_contract_runner PRIVATE "{(ROOT / "src").as_posix()}")\n'
            "target_link_libraries(job_system_contract_runner PRIVATE Qt6::Core Qt6::Concurrent)\n",
            encoding="utf-8",
        )
        configure = subprocess.run(
            ["cmake", "-S", str(source_dir), "-B", str(build_dir),
             "-G", "Visual Studio 17 2022", "-A", "x64",
             "-DCMAKE_PREFIX_PATH=C:/QtClean/6.5.3/msvc2019_64"],
            cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace",
        )
        (ARTIFACT / "configure.log").write_text(configure.stdout + configure.stderr, encoding="utf-8")
        if configure.returncode != 0:
            raise AssertionError(f"JobSystem contract configure failed: {configure.stderr[-2000:]}")
        build = subprocess.run(
            ["cmake", "--build", str(build_dir), "--config", "Release"],
            cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace",
        )
        (ARTIFACT / "build.log").write_text(build.stdout + build.stderr, encoding="utf-8")
        if build.returncode != 0:
            raise AssertionError(f"JobSystem contract build failed: {build.stderr[-2000:]}")
        cls.runner = build_dir / "Release" / "job_system_contract_runner.exe"

    def test_process_outcomes_and_timeout_budget(self):
        environment = os.environ.copy()
        environment["PATH"] = "C:/QtClean/6.5.3/msvc2019_64/bin;" + environment.get("PATH", "")
        result = subprocess.run(
            [str(self.runner)], cwd=ARTIFACT, capture_output=True, text=True,
            encoding="utf-8", errors="replace", env=environment, timeout=15,
        )
        (ARTIFACT / "runner.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
