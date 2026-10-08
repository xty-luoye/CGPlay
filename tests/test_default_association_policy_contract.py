"""Contracts for the elevated Windows default-association policy workflow."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class DefaultAssociationPolicyContracts(unittest.TestCase):
    def read(self, relative: str) -> str:
        return (ROOT / relative).read_text(encoding="utf-8")

    def test_policy_is_device_scoped_and_reapplies_at_sign_in(self) -> None:
        source = self.read("src/services/platform/WindowsFileAssociations.cpp")
        self.assertIn("Software\\\\Policies\\\\Microsoft\\\\Windows\\\\System", source)
        self.assertIn("DefaultAssociationsConfiguration", source)
        self.assertIn("FOLDERID_ProgramData", source)
        self.assertIn("QStandardPaths::GenericDataLocation", source)
        self.assertIn("SetNamedSecurityInfoW", source)
        self.assertIn("PROTECTED_DACL_SECURITY_INFORMATION", source)
        self.assertIn('DefaultAssociations Version=\\\"1\\\"', source)
        self.assertIn('Suggested=\\\"false\\\"', source)
        self.assertIn('Identifier=\\\".%1\\\"', source)
        self.assertIn('ProgId=\\\"CGPlay.Video\\\"', source)

    def test_elevation_and_worker_are_explicit(self) -> None:
        source = self.read("src/services/platform/WindowsFileAssociations.cpp")
        main = self.read("src/main.cpp")
        self.assertIn("ShellExecuteExW", source)
        self.assertIn('lpVerb = L"runas"', source)
        self.assertIn("SEE_MASK_NOCLOSEPROCESS", source)
        self.assertIn("WaitForSingleObject", source)
        self.assertIn("--cgplay-deploy-default-associations", main)
        self.assertIn("runDeviceDefaultAssociationsWorker", main)

    def test_policy_has_guarded_rollback(self) -> None:
        source = self.read("src/services/platform/WindowsFileAssociations.cpp")
        self.assertIn("requestRemoveDeviceDefaultAssociations", source)
        self.assertIn("runRemoveDeviceDefaultAssociationsWorker", source)
        self.assertIn("当前设备默认关联策略不是 CGPlay，未删除", source)
        self.assertIn("QFile::remove(xmlPath)", source)

    def test_settings_exposes_deploy_and_remove_actions(self) -> None:
        source = self.read("src/ui/app/ApplicationSettings.cpp")
        automation = self.read("src/ui/app/MainWindowAutomationSettings.cpp")
        for label in ("管理员部署全部默认", "撤销设备级策略"):
            self.assertIn(label, source)
            self.assertIn(label, automation)


if __name__ == "__main__":
    unittest.main()
