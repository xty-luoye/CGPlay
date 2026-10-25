"""Native update routing and old-cache compatibility, without network requests."""
import json
import os
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
OUT = Path(os.environ.get("CGPLAY_MANIFEST_POLICY_ARTIFACTS",
    ROOT / "tests/artifacts/component_manifest_policy_1_0_8"))
QT = Path(os.environ.get("CGPLAY_TEST_QT_ROOT", "E:/rjkf/CGPlay-deps/QtClean/6.5.3/msvc2019_64"))


class ManifestPolicyTests(unittest.TestCase):
    def test_shipped_sources_and_cached_component_merge(self):
        OUT.mkdir(parents=True, exist_ok=True)
        build = OUT / "build"
        environment = os.environ.copy()
        environment["PATH"] = str(QT / "bin") + os.pathsep + environment.get("PATH", "")
        commands = [
            ["cmake", "-S", ROOT / "tests/component_install_contracts", "-B", build,
             "-G", "Visual Studio 17 2022", "-A", "x64", f"-DCMAKE_PREFIX_PATH={QT}",
             f"-DCGPLAY_SOURCE_ROOT={ROOT}", f"-DCGPLAY_TEST_EXTRACTED_DIR={OUT}"],
            ["cmake", "--build", build, "--config", "Release", "--target", "manifest_policy_probe", "--parallel", "2"],
        ]
        with (OUT / "commands.log").open("wb") as log:
            for command in commands:
                subprocess.run([str(v) for v in command], check=True, cwd=ROOT, env=environment,
                    creationflags=subprocess.CREATE_NO_WINDOW, stdout=log, stderr=log)
        result = subprocess.run([str(build / "Release/manifest_policy_probe.exe"),
            str(ROOT / "resources/component_manifest.json")], check=True, cwd=ROOT, env=environment,
            creationflags=subprocess.CREATE_NO_WINDOW, capture_output=True)
        report = json.loads(result.stdout)
        (OUT / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        self.assertTrue(report["passed"], report)
        self.assertEqual(0, report["nativeVisibleWindows"])
        self.assertGreaterEqual(len(report["checks"]), 10)
        manager = (ROOT / "src/services/component/ComponentManager.cpp").read_text(encoding="utf-8-sig")
        self.assertIn("component_manifest_policy::sourceUrls(_bundledManifest, _manifestUrl)", manager)
        self.assertIn("component_manifest_policy::mergePayload(doc.object(), _bundledManifest)", manager)


if __name__ == "__main__":
    unittest.main()
