import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class CommandDispatcherContractTests(unittest.TestCase):
    def read(self, relative: str) -> str:
        return (ROOT / relative).read_text(encoding="utf-8")

    def test_registry_only_owns_metadata(self) -> None:
        header = self.read("src/plugins/CommandRegistry.h")
        implementation = self.read("src/plugins/CommandRegistry.cpp")
        self.assertNotIn("FallbackExecutor", header)
        self.assertNotIn("setFallbackExecutor", header + implementation)
        self.assertNotRegex(header, r"\bexecute\s*\(")

    def test_dispatcher_rejects_unknown_and_disabled_commands(self) -> None:
        implementation = self.read("src/ui/app/CommandDispatcher.cpp")
        self.assertIn("const CommandDescriptor* command = _registry->find(commandId);", implementation)
        self.assertIn("if (!command) return false;", implementation)
        self.assertIn("if (command->isEnabled && !command->isEnabled()) return false;", implementation)
        self.assertIn("!_registry->contains(id)", implementation)

    def test_plugin_descriptor_trigger_precedes_registered_handler(self) -> None:
        implementation = self.read("src/ui/app/CommandDispatcher.cpp")
        trigger = implementation.index("if (command->trigger)")
        handler = implementation.index("_handlers.constFind")
        self.assertLess(trigger, handler)

    def test_main_window_has_no_manual_command_fallback(self) -> None:
        header = self.read("src/ui/app/Application.h")
        implementation = self.read("src/ui/app/Application.cpp")
        self.assertNotIn("_executeCommandFallback", header + implementation)
        dispatch_method = implementation.split("bool MainWindow::_executeCommandId", 1)[1]
        dispatch_method = dispatch_method.split("void MainWindow::_rebuildCustomToolbar", 1)[0]
        self.assertIn("_p->commandDispatcher->dispatch(commandId, checked)", dispatch_method)
        self.assertNotIn("QStringLiteral(\"playback.", dispatch_method)

    def test_every_builtin_descriptor_has_one_registered_handler(self) -> None:
        implementation = self.read("src/ui/app/Application.cpp")
        load = implementation.split("void MainWindow::_loadHostExtensionContributions()", 1)[1]
        load = load.split("void MainWindow::_applyMenuContributions", 1)[0]
        ids = re.findall(r'addBuiltIn\(\{QStringLiteral\("([^"]+)"\)', load)
        expected = {
            "playback.toggle", "playback.previousFrame", "playback.nextFrame",
            "playback.gotoStart", "playback.gotoEnd", "playback.reverse",
            "playback.stop", "playback.forward", "playback.seekBackward10",
            "playback.seekForward10", "playback.setInPoint", "playback.setOutPoint",
            "playback.clearInPoint", "playback.clearOutPoint", "playback.setSpeed",
            "playback.loop", "audio.toggleMute", "audio.openVolume",
            "view.fitToWindow", "view.zoom1to1", "view.fullscreen",
            "translation.toggle", "translation.mode", "annotation.create",
            "annotation.delete", "annotation.undo", "annotation.redo",
            "compare.toggleB", "compare.autoClearB", "codex.captureFrame",
            "codex.browserSnapshot", "export.render", "workspace.save",
            "workspace.reset",
        }
        self.assertEqual(len(ids), len(set(ids)))
        self.assertTrue(expected.issubset(set(ids)))
        self.assertIn("for (const auto& [descriptor, tool] : annotationTools)", load)
        self.assertIn("for (const auto& [descriptor, mode] : compareModes)", load)

    def test_builtins_survive_without_plugin_manager(self) -> None:
        implementation = self.read("src/ui/app/Application.cpp")
        load = implementation.split("void MainWindow::_loadHostExtensionContributions()", 1)[1]
        load = load.split("void MainWindow::_applyMenuContributions", 1)[0]
        first_builtin = load.index("addBuiltIn(")
        plugin_commands = load.index("pluginManager\n        ? pluginManager->commandDescriptors()")
        self.assertLess(first_builtin, plugin_commands)
        self.assertNotIn("if (!pluginManager) {", load[:first_builtin])

    def test_core_menu_and_transport_controls_dispatch_by_id(self) -> None:
        implementation = self.read("src/ui/app/Application.cpp")
        for command_id in (
            "playback.toggle", "playback.previousFrame", "playback.nextFrame",
            "playback.gotoStart", "playback.gotoEnd", "audio.toggleMute",
            "view.fitToWindow", "view.zoom1to1", "view.fullscreen",
            "export.render", "workspace.save", "workspace.reset",
        ):
            self.assertIn(f'_executeCommandId(QStringLiteral("{command_id}"))', implementation)
        self.assertIn("QObject::disconnect(button, nullptr, _p->playbackBar, nullptr);", implementation)
        self.assertIn("_executeCommandId(commandId);", implementation)


if __name__ == "__main__":
    unittest.main()
