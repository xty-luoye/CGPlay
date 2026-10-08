"""Contracts for the in-app file association editor and Explorer thumbnails."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class FileAssociationThumbnailContracts(unittest.TestCase):
    def read(self, relative: str) -> str:
        return (ROOT / relative).read_text(encoding="utf-8")

    def test_settings_exposes_select_all_apply_and_default_apps(self) -> None:
        source = self.read("src/ui/app/ApplicationSettings.cpp")
        for name in (
            "SettingsFileAssociationSelectAll",
            "SettingsFileAssociationClearAll",
            "SettingsFileAssociationApply",
            "SettingsFileAssociationOpenDefaultApps",
            "SettingsFileAssociationDeployDeviceDefaults",
            "SettingsFileAssociationRemoveDeviceDefaults",
            "SettingsFileAssociationCount",
        ):
            self.assertIn(name, source)
        self.assertIn("WindowsFileAssociations::supportedVideoExtensions()", source)
        self.assertIn("WindowsFileAssociations::applyVideoExtensions", source)
        self.assertIn("ms-settings:defaultapps", source)
        self.assertIn("ms-settings:defaultapps?registeredAppUser=CGPlay", source)
        self.assertIn("打开 CGPlay 默认应用页", source)
        self.assertIn("管理员部署全部默认", source)
        self.assertIn("撤销设备级策略", source)
        self.assertIn("注销/登录", source)
        self.assertIn("已登记", source)

    def test_settings_automation_audits_new_controls(self) -> None:
        source = self.read("src/ui/app/MainWindowAutomationSettings.cpp")
        self.assertIn("Settings file association controls", source)
        self.assertIn("supportedVideoExtensions", source)
        self.assertIn("SettingsFileAssociationApply", source)
        self.assertIn("默认播放器", source)

    def test_registry_helper_preserves_userchoice_and_notifies_shell(self) -> None:
        source = self.read("src/services/platform/WindowsFileAssociations.cpp")
        self.assertIn("HKEY_CURRENT_USER", source)
        self.assertIn("HKEY_LOCAL_MACHINE", source)
        self.assertIn("CGPlay.Video", source)
        self.assertIn("OpenWithProgids", source)
        self.assertIn("SHChangeNotify(SHCNE_ASSOCCHANGED", source)
        # UserChoice is read-only here: Windows owns the protected hash and
        # CGPlay must never fabricate or delete it.
        self.assertIn("UserChoice", source)
        self.assertIn("hasExplicitUserChoice", source)
        self.assertIn("pruneStaleOpenWithProgIds", source)
        self.assertIn("CGPlayThumbnailProvider.dll", source)

    def test_device_policy_worker_uses_supported_extensions_and_runas(self) -> None:
        source = self.read("src/services/platform/WindowsFileAssociations.cpp")
        main = self.read("src/main.cpp")
        self.assertIn("DefaultAssociationsConfiguration", source)
        self.assertIn('Suggested=\\\"false\\\"', source)
        self.assertIn("defaultAssociationsXml", source)
        self.assertIn("ShellExecuteExW", source)
        self.assertIn('lpVerb = L"runas"', source)
        self.assertIn("--cgplay-deploy-default-associations", source)
        self.assertIn("--cgplay-remove-default-associations", main)

    def test_thumbnail_provider_implements_file_initialized_com_contract(self) -> None:
        source = self.read("src/shell/CGPlayThumbnailProvider.cpp")
        self.assertIn("IThumbnailProvider", source)
        self.assertIn("IInitializeWithFile", source)
        self.assertIn("DllGetClassObject", source)
        self.assertIn("DllCanUnloadNow", source)
        self.assertIn("CreateProcessW", source)
        self.assertIn("ffmpeg.exe", source)
        self.assertIn("CreateDIBSection", source)
        self.assertIn("WICPixelFormat32bppBGRA", source)

    def test_thumbnail_provider_has_stable_exports(self) -> None:
        definition = self.read("src/shell/CGPlayThumbnailProvider.def")
        self.assertIn("DllGetClassObject", definition)
        self.assertIn("DllCanUnloadNow", definition)
        cmake = self.read("src/CMakeLists.txt")
        self.assertIn("add_library(CGPlayThumbnailProvider SHARED", cmake)
        self.assertIn("CGPlayThumbnailProvider.def", cmake)
        self.assertIn("windowscodecs", cmake)

    def test_file_initialized_provider_registers_compatible_shell_loading(self) -> None:
        source = self.read("src/services/platform/WindowsFileAssociations.cpp")
        installer = self.read("installer/CGPlay_Installer.nsi")
        inno = self.read("scripts/installer/CGPlay.iss")
        self.assertIn('L"DisableProcessIsolation", 0, REG_DWORD', source)
        self.assertIn("RegWriteDWordValue(HKCU, 'Software\\Classes\\CLSID\\' + CGPlayThumbnailClsid, 'DisableProcessIsolation', 1);", inno)
        self.assertIn("registerFileThumbnailInitialization(HKEY_LOCAL_MACHINE, clsid)", source)
        self.assertIn(
            "registerFileThumbnailInitialization(HKEY_CURRENT_USER, WindowsFileAssociations::thumbnailProviderClsid())",
            source,
        )
        self.assertIn(
            'WriteRegDWORD HKCU "Software\\Classes\\CLSID\\${CGPLAY_THUMBNAIL_CLSID}" "DisableProcessIsolation" 1',
            installer,
        )
        for function in ("Function .onInit", "Function un.onInit"):
            body = installer.split(function, 1)[1].split("FunctionEnd", 1)[0]
            self.assertIn("SetRegView 64", body)

    def test_installer_registers_provider_for_each_video_extension(self) -> None:
        installer = self.read("installer/CGPlay_Installer.nsi")
        self.assertIn("CGPLAY_THUMBNAIL_CLSID", installer)
        self.assertIn("CGPLAY_THUMBNAIL_SHELLEX", installer)
        self.assertIn("CGPlayThumbnailProvider.dll", installer)
        self.assertIn("ShellEx\\${CGPLAY_THUMBNAIL_SHELLEX}", installer)
        self.assertIn("DeleteRegKey HKCU \"Software\\Classes\\CLSID\\${CGPLAY_THUMBNAIL_CLSID}\"", installer)

    def test_package_validators_require_thumbnail_provider(self) -> None:
        packager = self.read("tools/package_installer.ps1")
        payload = self.read("tools/validate_package_payload.ps1")
        self.assertIn("CGPlayThumbnailProvider.dll", packager)
        self.assertIn("CGPlayThumbnailProvider.dll", payload)
        self.assertIn("--target CGPlay CGPlayQuickLook CGPlayThumbnailProvider", packager)


if __name__ == "__main__":
    unittest.main()
