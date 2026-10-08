import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class PluginLifecycleContractTests(unittest.TestCase):
    def read(self, relative: str) -> str:
        return (ROOT / relative).read_text(encoding="utf-8")

    def function_body(self, source: str, signature: str) -> str:
        start = source.index(signature)
        brace = source.index("{", start)
        depth = 0
        for index in range(brace, len(source)):
            if source[index] == "{":
                depth += 1
            elif source[index] == "}":
                depth -= 1
                if depth == 0:
                    return source[brace + 1:index]
        self.fail(f"unterminated function: {signature}")

    def test_manager_separates_deactivation_from_binary_unload(self) -> None:
        header = self.read("src/plugins/PluginManager.h")
        implementation = self.read("src/plugins/PluginManager.cpp")

        for contract in (
            "bool deactivatePlugin(const QString& pluginId);",
            "bool reactivatePlugin(const QString& pluginId);",
            "bool unloadPluginBinary(const QString& pluginId);",
            "bool canUnloadPluginBinary(const QString& pluginId) const;",
            "LifecycleState pluginLifecycleState(const QString& pluginId) const;",
            "bool isPluginInitialized(const QString& pluginId) const;",
        ):
            self.assertIn(contract, header)

        legacy_unload = self.function_body(
            implementation, "bool PluginManager::unloadPlugin(const QString& pluginId)"
        )
        legacy_reload = self.function_body(
            implementation, "bool PluginManager::reloadPlugin(const QString& pluginId)"
        )
        binary_unload = self.function_body(
            implementation, "bool PluginManager::unloadPluginBinary(const QString& pluginId)"
        )
        self.assertIn("return deactivatePlugin(pluginId);", legacy_unload)
        self.assertIn("return reactivatePlugin(pluginId);", legacy_reload)
        self.assertIn("binaryUnloadRefused", binary_unload)
        self.assertNotIn("loader->unload()", binary_unload)

    def test_manager_tracks_initialization_per_plugin(self) -> None:
        header = self.read("src/plugins/PluginManager.h")
        implementation = self.read("src/plugins/PluginManager.cpp")
        self.assertRegex(header, r"bool\s+initialized\s*=\s*false;")
        self.assertIn("!entry.initialized && entry.plugin", implementation)
        self.assertIn("entry.initialized = true;", implementation)
        self.assertGreaterEqual(
            implementation.count("registeredEntry.initialized = true;"), 2
        )
        self.assertIn("it->initialized && it->plugin", implementation)
        self.assertIn("it->initialized = false;", implementation)
        self.assertIn("entry->initialized = false;", implementation)

    def test_builtin_plugin_lifecycle_methods_are_idempotent(self) -> None:
        plugins = {
            "AnnotationPlugin": "src/plugins/annotation/AnnotationPlugin.cpp",
            "OcioPlugin": "src/plugins/ocio/OcioPlugin.cpp",
            "QuickLookPlugin": "src/plugins/quicklook/QuickLookPlugin.cpp",
            "CodexPlugin": "src/plugins/codex/CodexPlugin.cpp",
        }
        for class_name, relative in plugins.items():
            with self.subTest(plugin=class_name):
                source = self.read(relative)
                initialize = self.function_body(source, f"void {class_name}::initialize()")
                shutdown = self.function_body(source, f"void {class_name}::shutdown()")
                self.assertRegex(initialize, r"if\s*\(_initialized\)\s*(?:\{|return)")
                self.assertIn("_initialized = true;", initialize)
                self.assertRegex(shutdown, r"if\s*\(!_initialized\)\s*(?:\{|return)")
                self.assertIn("_initialized = false;", shutdown)

    def test_codex_deactivation_suspends_background_resources(self) -> None:
        source = self.read("src/plugins/codex/CodexPlugin.cpp")
        constructor = self.function_body(source, "CodexPlugin::CodexPlugin(QObject* parent)")
        initialize = self.function_body(source, "void CodexPlugin::initialize()")
        shutdown = self.function_body(source, "void CodexPlugin::shutdown()")

        self.assertNotIn("_localTaskTimer->start();", constructor)
        self.assertIn("_localTaskTimer->start();", initialize)
        self.assertIn("_localTaskTimer->stop();", shutdown)
        self.assertIn("_terminalTimeoutTimer->stop();", shutdown)
        self.assertIn("_localFileWatcher->removePaths", shutdown)
        self.assertIn("_restartAfterStop = false;", shutdown)

    def test_plugin_interface_keeps_existing_virtual_abi_surface(self) -> None:
        interface = self.read("src/plugins/api/IPlugin.h")
        virtual_methods = re.findall(r"virtual\s+[^;]+;", interface)
        self.assertEqual(virtual_methods, [
            "virtual ~IPlugin() = default;",
            "virtual QString id() const = 0;",
            "virtual void initialize() = 0;",
            "virtual void shutdown() = 0;",
        ])
        self.assertIn("shutdown() deactivates runtime", interface)


if __name__ == "__main__":
    unittest.main()
