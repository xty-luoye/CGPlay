import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class ArchitectureContractTests(unittest.TestCase):
    def read(self, relative: str) -> str:
        return (ROOT / relative).read_text(encoding="utf-8")

    def test_read_ahead_cache_does_not_expose_container_pointer(self) -> None:
        header = self.read("src/core/cache/ReadAheadCache.h")
        implementation = self.read("src/core/cache/ReadAheadCache.cpp")
        self.assertIn("QImage getFrame(int frameNumber) const;", header)
        self.assertIn("QImage ReadAheadCache::getFrame", implementation)
        self.assertNotIn("const QImage* getFrame", header)
        self.assertNotIn("return &it.value()", implementation)

    def test_review_export_frame_count_uses_named_ffprobe_fields(self) -> None:
        implementation = self.read("src/features/annotation/ReviewExport.cpp")
        self.assertIn('"-of", "json=compact=1"', implementation)
        self.assertIn('QStringLiteral("nb_frames")', implementation)
        self.assertIn('QStringLiteral("nb_read_frames")', implementation)
        self.assertIn('QStringLiteral("avg_frame_rate")', implementation)
        self.assertNotIn('"-of", "csv=p=0"', implementation)

    def test_read_ahead_cache_replays_latest_request_after_inflight_batch(self) -> None:
        implementation = self.read("src/core/cache/ReadAheadCache.cpp")
        self.assertIn("bool deferredPreload = false;", implementation)
        self.assertIn("_p->deferredFrom = requestFrom;", implementation)
        self.assertIn("requestPreload(deferredFrom, deferredTo);", implementation)

    def test_registered_commands_use_registry_dispatch(self) -> None:
        registry = self.read("src/plugins/CommandRegistry.cpp")
        dispatcher = self.read("src/ui/app/CommandDispatcher.cpp")
        application = self.read("src/ui/app/Application.cpp")
        self.assertNotIn("execute(", registry)
        self.assertIn("const CommandDescriptor* command = _registry->find(commandId);", dispatcher)
        self.assertIn("if (command->trigger)", dispatcher)
        self.assertIn("return _p->commandDispatcher->dispatch(commandId, checked);", application)
        self.assertNotIn("_executeCommandFallback", application)

    def test_input_bindings_delegate_to_typed_profile(self) -> None:
        header = self.read("src/common/input/InputBindingStore.h")
        implementation = self.read("src/common/input/InputBindingStore.cpp")
        self.assertIn("class InputProfile", header)
        for contract in ("defaults()", "migrate(", "fromJson(", "validate(", "toJson()"):
            self.assertIn(contract, header)
        self.assertIn("InputProfile _profile;", header)
        self.assertIn("return _profile.binding(device, commandId);", implementation)
        self.assertIn("return _profile.setBinding(device, commandId, gesture, conflict);", implementation)
        self.assertIn("const QJsonObject profileObject = _profile.toJson();", implementation)


if __name__ == "__main__":
    unittest.main()
