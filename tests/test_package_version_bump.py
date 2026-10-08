import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class PackageVersionBumpContracts(unittest.TestCase):
    @staticmethod
    def current_version() -> str:
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        match = re.search(r"project\s*\(\s*CGPlay\s+VERSION\s+([0-9.]+)", cmake)
        if not match:
            raise AssertionError("CGPlay version is missing from CMakeLists.txt")
        return match.group(1)

    @staticmethod
    def invoke_version_functions(body: str) -> subprocess.CompletedProcess[str]:
        script_path = ROOT / "tools/package_installer.ps1"
        bootstrap = rf"""
$source = [System.IO.File]::ReadAllText('{script_path}')
$marker = '$versionRollbackSnapshot = $null'
$prefix = $source.Substring(0, $source.IndexOf($marker))
Invoke-Expression $prefix
{body}
"""
        return subprocess.run(
            ["powershell", "-NoProfile", "-NonInteractive", "-Command", bootstrap],
            cwd=ROOT,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            check=False,
        )

    def test_packager_bumps_and_rebuilds_by_default(self) -> None:
        script = (ROOT / "tools/package_installer.ps1").read_text(encoding="utf-8")
        self.assertIn("Get-NextProjectVersion", script)
        self.assertIn("Set-ProjectVersion", script)
        self.assertIn("[switch]$NoVersionBump", script)
        self.assertIn("revisionWidth", script)
        self.assertIn('ToString("D$revisionWidth")', script)
        self.assertIn("cmake --build $buildDir --config Release --target CGPlay CGPlayQuickLook", script)

    def test_version_surfaces_are_updated_together(self) -> None:
        script = (ROOT / "tools/package_installer.ps1").read_text(encoding="utf-8")
        for path in (
            "CMakeLists.txt",
            "installer\\CGPlay_Installer.nsi",
            "resources\\CGPlay.rc",
            "src\\ui\\app\\ApplicationRuntime.cpp",
            "src\\ui\\app\\ApplicationUiSupport.cpp",
        ):
            self.assertIn(path, script)
        self.assertIn("APP_FILE_VERSION", script)
        self.assertIn("FILEVERSION", script)
        self.assertIn("PRODUCTVERSION", script)

    def test_current_display_and_numeric_versions_are_consistent(self) -> None:
        version = self.current_version()
        numeric_parts = [int(part) for part in version.split(".")]
        numeric_parts.extend([0] * (4 - len(numeric_parts)))
        file_version = ".".join(str(part) for part in numeric_parts)
        resource_version = ",".join(str(part) for part in numeric_parts)
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        nsi = (ROOT / "installer/CGPlay_Installer.nsi").read_text(encoding="utf-8")
        rc = (ROOT / "resources/CGPlay.rc").read_text(encoding="utf-8")
        runtime = (ROOT / "src/ui/app/ApplicationRuntime.cpp").read_text(encoding="utf-8")
        about = (ROOT / "src/ui/app/ApplicationUiSupport.cpp").read_text(encoding="utf-8")
        for source in (cmake, nsi, rc, runtime, about):
            self.assertIn(version, source)
        self.assertIn(f'APP_FILE_VERSION "{file_version}"', nsi)
        self.assertIn(f"FILEVERSION {resource_version}", rc)
        self.assertIn(f"PRODUCTVERSION {resource_version}", rc)

    def test_next_version_and_replacements_execute_without_group_ambiguity(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            fixture = Path(temp_dir)
            for relative in (
                "CMakeLists.txt",
                "installer/CGPlay_Installer.nsi",
                "resources/CGPlay.rc",
                "src/ui/app/ApplicationRuntime.cpp",
                "src/ui/app/ApplicationUiSupport.cpp",
            ):
                target = fixture / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(ROOT / relative, target)

            escaped_fixture = str(fixture).replace("'", "''")
            result = self.invoke_version_functions(
                rf"""
$next = Get-NextProjectVersion '1.0.7.09'
if ($next -ne '1.0.7.10') {{ throw "Unexpected next version: $next" }}
Set-ProjectVersion -Root '{escaped_fixture}' -Version '11.2.3.04'
Get-ProjectVersion -Root '{escaped_fixture}'
"""
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("11.2.3.04", result.stdout)
            cmake = (fixture / "CMakeLists.txt").read_text(encoding="utf-8")
            self.assertIn("VERSION 11.2.3.04", cmake)
            self.assertNotIn("$11", cmake)

    def test_explicit_and_no_bump_branches_do_not_write_version_sources(self) -> None:
        script = (ROOT / "tools/package_installer.ps1").read_text(encoding="utf-8")
        selection = script[script.index("$previousVersion ="):script.index("if ($versionBumped) {")]
        self.assertNotIn("Set-ProjectVersion", selection)
        self.assertIn("elseif ($NoVersionBump)", selection)
        self.assertIn("$Version.Trim()", selection)

    def test_packager_rolls_back_and_validates_binary_versions(self) -> None:
        script = (ROOT / "tools/package_installer.ps1").read_text(encoding="utf-8")
        self.assertIn("Save-ProjectVersionFiles", script)
        self.assertIn("Restore-ProjectVersionFiles", script)
        self.assertIn("Packaging failed; restoring version source files.", script)
        self.assertIn("Assert-ReleaseBinaryVersion -Directory $ReleaseDir", script)


if __name__ == "__main__":
    unittest.main()
