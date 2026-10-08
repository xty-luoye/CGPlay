import json
import os
import shutil
import subprocess
import sys
import textwrap
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
ARTIFACTS = ROOT / "tests" / "artifacts" / "p1_export_credential_fix_20260727"
FFPROBE = ROOT / "build_win_full" / "bin" / "Release" / "ffprobe.exe"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

from cgplay.export.export_engine import run_export


class ReleaseExportSecurityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        from PIL import Image
        import numpy as np

        cls.output_dir = ARTIFACTS / "codec_outputs"
        cls.sequence_dir = ARTIFACTS / "rgba_sequence"
        cls.output_dir.mkdir(parents=True, exist_ok=True)
        cls.sequence_dir.mkdir(parents=True, exist_ok=True)
        width, height = 96, 64
        for frame_number in range(1, 4):
            x = np.linspace(0, 255, width, dtype=np.uint8)
            alpha = np.tile(x, (height, 1))
            rgba = np.zeros((height, width, 4), dtype=np.uint8)
            rgba[:, :, 0] = 40 * frame_number
            rgba[:, :, 1] = np.tile(x, (height, 1))
            rgba[:, :, 2] = 255 - rgba[:, :, 1]
            rgba[:, :, 3] = alpha
            Image.fromarray(rgba, "RGBA").save(cls.sequence_dir / f"frame.{frame_number:04d}.png")
        cls.source = cls.sequence_dir / "frame.0001.png"

    def _probe(self, path: Path) -> dict:
        completed = subprocess.run(
            [
                str(FFPROBE), "-v", "error", "-select_streams", "v:0",
                "-show_entries", (
                    "stream=codec_name,profile,pix_fmt,width,height,color_range,"
                    "color_space,color_transfer,color_primaries"
                ),
                "-of", "json", str(path),
            ],
            check=True,
            capture_output=True,
            text=True,
            encoding="utf-8",
        )
        return json.loads(completed.stdout)["streams"][0]

    def test_real_codec_outputs_and_alpha(self) -> None:
        import av
        import numpy as np

        cases = {
            "h264": ("h264.mp4", "h264"),
            "h265": ("h265.mp4", "hevc"),
            "prores_hq": ("prores_422_hq.mov", "prores"),
            "prores_4444": ("prores_4444.mov", "prores"),
        }
        report = {}
        for codec, (filename, expected_codec) in cases.items():
            output = self.output_dir / filename
            output.unlink(missing_ok=True)
            self.assertEqual(0, run_export(str(self.source), str(output), 24.0, 1, 3, codec=codec))
            self.assertTrue(output.is_file())
            stream = self._probe(output)
            self.assertEqual(expected_codec, stream["codec_name"])
            self.assertEqual("tv", stream.get("color_range"), stream)
            self.assertEqual("bt709", stream.get("color_space"), stream)
            self.assertEqual("bt709", stream.get("color_transfer"), stream)
            self.assertEqual("bt709", stream.get("color_primaries"), stream)
            report[codec] = stream

        prores_path = self.output_dir / "prores_4444.mov"
        prores_hq = report["prores_hq"]
        self.assertIn("HQ", prores_hq.get("profile", "").upper())
        self.assertEqual("yuv422p10le", prores_hq.get("pix_fmt"))
        prores = report["prores_4444"]
        self.assertIn("4444", prores.get("profile", ""))
        self.assertTrue(prores.get("pix_fmt", "").startswith("yuva444"), prores)
        with av.open(str(prores_path)) as container:
            frame = next(container.decode(video=0))
            decoded = frame.to_ndarray(format="rgba")
        alpha = decoded[:, :, 3]
        self.assertLess(int(alpha.min()), 32)
        self.assertGreater(int(alpha.max()), 220)
        self.assertGreater(float(np.std(alpha)), 30.0)
        prores["decoded_alpha"] = {
            "minimum": int(alpha.min()),
            "maximum": int(alpha.max()),
            "standardDeviation": float(np.std(alpha)),
        }

        (ARTIFACTS / "codec_probe.json").write_text(
            json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
        )

    def test_unknown_codec_and_missing_annotations_fail_closed(self) -> None:
        unknown_output = self.output_dir / "unknown.mp4"
        unknown_output.unlink(missing_ok=True)
        self.assertEqual(1, run_export(str(self.source), str(unknown_output), 24.0, 1, 1, codec="unknown"))
        self.assertFalse(unknown_output.exists())

        missing_output = self.output_dir / "missing_annotations.mp4"
        missing_output.unlink(missing_ok=True)
        self.assertEqual(
            1,
            run_export(
                str(self.source), str(missing_output), 24.0, 1, 1,
                codec="h264", annotations_json=str(ARTIFACTS / "missing.annotations.json"),
            ),
        )
        self.assertFalse(missing_output.exists())

        invalid_cases = {
            "one_point_rectangle": [{
                "frame": 1,
                "type": "rectangle",
                "points": [{"x": 1, "y": 1}],
            }],
            "unknown_type": [{
                "frame": 1,
                "type": "not-a-real-tool",
                "points": [{"x": 1, "y": 1}, {"x": 10, "y": 10}],
            }],
        }
        for name, payload in invalid_cases.items():
            with self.subTest(name=name):
                sidecar = ARTIFACTS / f"{name}.annotations.json"
                output = self.output_dir / f"{name}.mp4"
                sidecar.write_text(json.dumps(payload), encoding="utf-8")
                output.unlink(missing_ok=True)
                self.assertEqual(
                    1,
                    run_export(
                        str(self.source), str(output), 24.0, 1, 1,
                        codec="h264", annotations_json=str(sidecar),
                    ),
                )
                self.assertFalse(output.exists())

        out_of_range = ARTIFACTS / "out_of_range.annotations.json"
        out_of_range.write_text(
            json.dumps([{
                "frame": 999,
                "type": "rectangle",
                "points": [{"x": 1, "y": 1}, {"x": 10, "y": 10}],
            }]),
            encoding="utf-8",
        )
        self.assertEqual(
            1,
            run_export(
                str(self.source), str(missing_output), 24.0, 1, 1,
                codec="h264", annotations_json=str(out_of_range),
            ),
        )
        self.assertFalse(missing_output.exists())

        malformed = ARTIFACTS / "malformed.annotations.json"
        malformed.write_text(json.dumps([{"frame": 1, "points": []}]), encoding="utf-8")
        self.assertEqual(
            1,
            run_export(
                str(self.source), str(missing_output), 24.0, 1, 1,
                codec="h264", annotations_json=str(malformed),
            ),
        )
        self.assertFalse(missing_output.exists())

        empty_sidecar = ARTIFACTS / "empty.annotations.json"
        empty_sidecar.write_text("[]\n", encoding="utf-8")
        self.assertEqual(
            1,
            run_export(
                str(self.source), str(missing_output), 24.0, 1, 1,
                codec="h264", annotations_json=str(empty_sidecar),
            ),
        )
        self.assertFalse(missing_output.exists())

    def test_nonempty_annotation_sidecar_is_rendered(self) -> None:
        import av
        import numpy as np
        from PIL import Image

        sidecar = ARTIFACTS / "valid.annotations.json"
        sidecar.write_text(
            json.dumps([
                {
                    "frame": 1,
                    "type": "rectangle",
                    "color": "#ff0000",
                    "author": "test",
                    "comment": "visible annotation",
                    "points": [{"x": 8, "y": 8}, {"x": 70, "y": 48}],
                }
            ]),
            encoding="utf-8",
        )
        output = self.output_dir / "annotated.mp4"
        clean_output = self.output_dir / "annotation_clean_reference.mp4"
        output.unlink(missing_ok=True)
        clean_output.unlink(missing_ok=True)
        self.assertEqual(0, run_export(str(self.source), str(clean_output), 24.0, 1, 1, codec="h264"))
        self.assertEqual(
            0,
            run_export(
                str(self.source), str(output), 24.0, 1, 1,
                codec="h264", annotations_json=str(sidecar),
            ),
        )
        self.assertEqual("h264", self._probe(output)["codec_name"])
        with av.open(str(output)) as container:
            decoded = next(container.decode(video=0)).to_ndarray(format="rgb24")
        with av.open(str(clean_output)) as container:
            clean_decoded = next(container.decode(video=0)).to_ndarray(format="rgb24")
        self.assertGreater(float(np.mean(np.abs(decoded.astype(float) - clean_decoded.astype(float)))), 3.0)

        alpha_output = self.output_dir / "annotated_prores_4444.mov"
        alpha_output.unlink(missing_ok=True)
        self.assertEqual(
            0,
            run_export(
                str(self.source), str(alpha_output), 24.0, 1, 1,
                codec="prores_4444", annotations_json=str(sidecar),
            ),
        )
        with av.open(str(alpha_output)) as container:
            annotated_rgba = next(container.decode(video=0)).to_ndarray(format="rgba")
        source_alpha = np.asarray(Image.open(self.source).convert("RGBA"))[:, :, 3]
        self.assertGreater(int(annotated_rgba[8, 8, 3]), int(source_alpha[8, 8]) + 100)

    def test_sparse_sequence_uses_real_frame_numbers(self) -> None:
        import av
        import numpy as np
        from PIL import Image

        sequence_dir = ARTIFACTS / "sparse_sequence"
        sequence_dir.mkdir(parents=True, exist_ok=True)
        for frame_number, color in ((1, (20, 40, 60, 255)), (3, (60, 40, 20, 255))):
            Image.new("RGBA", (96, 64), color).save(sequence_dir / f"sparse.{frame_number:04d}.png")
        source = sequence_dir / "sparse.0001.png"
        sidecar = ARTIFACTS / "sparse.annotations.json"
        sidecar.write_text(json.dumps([{
            "frame": 3,
            "type": "rectangle",
            "color": "#ff0000",
            "points": [{"x": 8, "y": 8}, {"x": 70, "y": 48}],
        }]), encoding="utf-8")
        clean = self.output_dir / "sparse_clean.mp4"
        annotated = self.output_dir / "sparse_annotated.mp4"
        clean.unlink(missing_ok=True)
        annotated.unlink(missing_ok=True)
        self.assertEqual(0, run_export(str(source), str(clean), 24.0, 1, 3, codec="h264"))
        self.assertEqual(
            0,
            run_export(
                str(source), str(annotated), 24.0, 1, 3,
                codec="h264", annotations_json=str(sidecar),
            ),
        )
        with av.open(str(clean)) as container:
            clean_frames = [frame.to_ndarray(format="rgb24") for frame in container.decode(video=0)]
        with av.open(str(annotated)) as container:
            annotated_frames = [frame.to_ndarray(format="rgb24") for frame in container.decode(video=0)]
        self.assertEqual(2, len(annotated_frames))
        self.assertGreater(
            float(np.mean(np.abs(annotated_frames[1].astype(float) - clean_frames[1].astype(float)))),
            3.0,
        )

    def test_uint16_fallback_preserves_alpha_scale(self) -> None:
        import numpy as np
        from cgplay.export.utils.image_reader import _process_pixels

        rgba16 = np.zeros((1, 3, 4), dtype=np.uint16)
        rgba16[:, :, :3] = 32768
        rgba16[0, :, 3] = [0, 32768, 65535]
        converted = _process_pixels(rgba16)
        self.assertEqual([0, 127, 255], converted[0, :, 3].tolist())

        rgba8 = np.zeros((1, 3, 4), dtype=np.uint8)
        rgba8[0, :, 3] = [0, 128, 255]
        self.assertEqual([0, 128, 255], _process_pixels(rgba8)[0, :, 3].tolist())

        rgba_float = np.zeros((1, 3, 4), dtype=np.float32)
        rgba_float[0, :, 3] = [0.0, 1.0, 2.0]
        self.assertEqual([0, 255, 255], _process_pixels(rgba_float)[0, :, 3].tolist())

    def test_video_reader_estimates_missing_frame_count_and_streams_lazily(self) -> None:
        from fractions import Fraction
        from types import SimpleNamespace
        import numpy as np
        from cgplay.export.utils.video_reader import VideoReader

        decode_calls = []

        class FakeFrame:
            format = SimpleNamespace(name="yuv420p")

            def __init__(self, value):
                self.value = value

            def to_ndarray(self, format):
                self.requested_format = format
                return np.full((2, 3, 3), self.value, dtype=np.uint8)

        class FakePacket:
            def decode(self):
                decode_calls.append("decode")
                return iter((FakeFrame(10), FakeFrame(20), FakeFrame(30)))

        class FakeContainer:
            duration = 85_101_000

            def seek(self, offset):
                self.seek_offset = offset

            def demux(self, stream):
                return iter((FakePacket(),))

        reader = VideoReader()
        reader._container = FakeContainer()
        reader._stream = SimpleNamespace(
            frames=0,
            average_rate=Fraction(25, 1),
            width=3840,
            height=2160,
            color_range=1,
            colorspace=1,
            color_primaries=1,
            color_trc=1,
        )

        self.assertEqual(2128, reader.frame_count)
        self.assertEqual(
            {
                "color_range": 1,
                "color_space": 1,
                "color_primaries": 1,
                "color_transfer": 1,
            },
            reader.color_metadata,
        )
        frames = reader.read_frames(start_frame=1, end_frame=1)
        self.assertEqual([], decode_calls)
        decoded = list(frames)
        self.assertEqual(["decode"], decode_calls)
        self.assertEqual(1, len(decoded))
        self.assertEqual(20, int(decoded[0][0, 0, 0]))

    def test_finalize_failure_removes_partial_output(self) -> None:
        import logging
        from unittest.mock import patch
        from cgplay.export.export_job import _export_image_sequence
        from cgplay.export.models.export_settings import ExportSettings

        output = self.output_dir / "partial_finalize_failure.mp4"
        output.unlink(missing_ok=True)

        class FailingExporter:
            def write_frame(self, _frame):
                output.write_bytes(b"partial")

            def close(self):
                raise RuntimeError("simulated flush failure")

        settings = ExportSettings(
            fps=24.0,
            codec="h264",
            output_path=str(output),
            output_format="mp4",
        )
        with patch("cgplay.export.export_job._open_exporter", return_value=FailingExporter()):
            with self.assertRaisesRegex(RuntimeError, "finalize"):
                _export_image_sequence(
                    str(self.source), settings, logging.getLogger("test"), None, 1, 1, None
                )
        self.assertFalse(output.exists())

    def test_cpp_paths_are_strict_and_plaintext_reads_are_removed(self) -> None:
        review = (ROOT / "src/features/annotation/ReviewExport.cpp").read_text(encoding="utf-8")
        app = (ROOT / "src/ui/app/Application.cpp").read_text(encoding="utf-8")
        runtime = (ROOT / "src/ui/app/ApplicationRuntime.cpp").read_text(encoding="utf-8")
        self.assertIn("QTemporaryFile annotationFile", review)
        self.assertIn("QSaveFile file(annoJson)", review)
        self.assertIn("批注数据文件不存在", review)
        self.assertIn("codecId", review)
        self.assertIn("selectVideoExportCodec", app)
        self.assertIn("VideoExportCodec::ProRes4444", app)
        self.assertIn("migrateLegacySubtitleCredentials", runtime)

        legacy_keys = (
            "ai/subtitles/asr/apiKey",
            "ai/subtitles/mimo/apiKey",
            "ai/subtitles/qwen/apiKey",
            "ai/subtitles/translation/apiKey",
            "ai/subtitles/online/apiKey",
        )
        violations = []
        migration = ROOT / "src/services/ai/SubtitleCredentialMigration.cpp"
        for path in (ROOT / "src").rglob("*.cpp"):
            if path == migration:
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            for key in legacy_keys:
                if key in text:
                    violations.append(f"{path.relative_to(ROOT)}:{key}")
        self.assertEqual([], violations)

    def test_cpp_dpapi_migration_runner(self) -> None:
        build_dir = ARTIFACTS / "credential_migration_build"
        source_dir = ARTIFACTS / "credential_migration_runner"
        source_dir.mkdir(parents=True, exist_ok=True)
        runner = source_dir / "main.cpp"
        cmake = source_dir / "CMakeLists.txt"
        runner.write_text(textwrap.dedent(r'''
            #include "services/ai/SubtitleCredentialMigration.h"
            #include "services/ai/WindowsDpapiCredentialStore.h"
            #include "services/ai/api/IAICredentialStore.h"
            #include "settings/api/ISettingsService.h"
            #include <QCoreApplication>
            #include <QHash>
            #include <QSettings>
            #include <QStandardPaths>

            using namespace cgplay;

            class MemorySettings final : public ISettingsService {
            public:
                QVariant value(const QString& key, const QVariant& fallback = {}) const override { return values.value(key, fallback); }
                void setValue(const QString& key, const QVariant& value) override { values.insert(key, value); }
                bool contains(const QString& key) const override { return values.contains(key); }
                QStringList allKeys() const override { return values.keys(); }
                void remove(const QString& key) override { values.remove(key); }
                void sync() override {}
                QString organization() const override { return QStringLiteral("CGPlayTests"); }
                QString application() const override { return QStringLiteral("CredentialMigration"); }
                QHash<QString, QVariant> values;
            };

            class FailingStore final : public IAICredentialStore {
            public:
                bool storeSecret(const QString&, const QByteArray&, QString* error) override { if (error) *error = QStringLiteral("expected failure"); return false; }
                QByteArray loadSecret(const QString&, QString* = nullptr) const override { return {}; }
                bool removeSecret(const QString&, QString* = nullptr) override { return true; }
                bool hasSecret(const QString&) const override { return false; }
            };

            class CorruptStore final : public IAICredentialStore {
            public:
                bool storeSecret(const QString&, const QByteArray& value, QString* = nullptr) override { stored = value; return true; }
                QByteArray loadSecret(const QString&, QString* error = nullptr) const override { if (error) *error = QStringLiteral("corrupt DPAPI value"); return {}; }
                bool removeSecret(const QString&, QString* = nullptr) override { return true; }
                bool hasSecret(const QString&) const override { return true; }
                QByteArray stored;
            };

            int main(int argc, char** argv) {
                QCoreApplication app(argc, argv);
                app.setOrganizationName(QStringLiteral("CGPlayTests"));
                app.setApplicationName(QStringLiteral("CredentialMigration"));
                QStandardPaths::setTestModeEnabled(true);
                WindowsDpapiCredentialStore store;
                const QStringList ids = {
                    QStringLiteral("subtitles/asrApiKey"), QStringLiteral("mimo/apiKey"),
                    QStringLiteral("qwen/apiKey"), QStringLiteral("subtitles/translationApiKey"),
                    QStringLiteral("subtitles/onlineApiKey")
                };
                for (const QString& id : ids) store.removeSecret(id);
                if (!store.storeSecret(QStringLiteral("qwen/apiKey"), QByteArray("existing-safe"))) return 10;

                MemorySettings settings;
                settings.setValue(QStringLiteral("ai/subtitles/asr/apiKey"), QStringLiteral("asr-secret"));
                settings.setValue(QStringLiteral("ai/subtitles/mimo/apiKey"), QStringLiteral("mimo-secret"));
                settings.setValue(QStringLiteral("ai/subtitles/qwen/apiKey"), QStringLiteral("must-not-overwrite"));
                settings.setValue(QStringLiteral("ai/subtitles/translation/apiKey"), QStringLiteral("translate-secret"));
                settings.setValue(QStringLiteral("ai/subtitles/online/apiKey"), QStringLiteral("online-secret"));
                const auto result = migrateLegacySubtitleCredentials(&settings, &store);
                if (!result.success || result.migratedCount != 4 || result.removedPlaintextCount != 5) return 11;
                if (!settings.allKeys().isEmpty()) return 12;
                if (store.loadSecret(QStringLiteral("subtitles/asrApiKey")) != QByteArray("asr-secret")) return 13;
                if (store.loadSecret(QStringLiteral("qwen/apiKey")) != QByteArray("existing-safe")) return 14;
                if (store.loadSecret(QStringLiteral("subtitles/translationApiKey")) != QByteArray("translate-secret")) return 15;
                if (store.loadSecret(QStringLiteral("subtitles/onlineApiKey")) != QByteArray("online-secret")) return 16;
                for (const QString& id : ids) store.removeSecret(id);

                MemorySettings failingSettings;
                failingSettings.setValue(QStringLiteral("ai/subtitles/asr/apiKey"), QStringLiteral("retain-on-failure"));
                FailingStore failingStore;
                const auto failed = migrateLegacySubtitleCredentials(&failingSettings, &failingStore);
                if (failed.success || !failingSettings.contains(QStringLiteral("ai/subtitles/asr/apiKey"))) return 17;

                MemorySettings corruptSettings;
                corruptSettings.setValue(QStringLiteral("ai/subtitles/asr/apiKey"), QStringLiteral("repair-corrupt-secret"));
                CorruptStore corruptStore;
                const auto repaired = migrateLegacySubtitleCredentials(&corruptSettings, &corruptStore);
                if (!repaired.success || corruptSettings.contains(QStringLiteral("ai/subtitles/asr/apiKey"))) return 18;
                if (corruptStore.stored != QByteArray("repair-corrupt-secret")) return 19;
                return 0;
            }
        '''), encoding="utf-8")
        cmake.write_text(textwrap.dedent(f'''
            cmake_minimum_required(VERSION 3.21)
            project(CredentialMigrationRunner LANGUAGES CXX)
            set(CMAKE_CXX_STANDARD 17)
            find_package(Qt6 REQUIRED COMPONENTS Core)
            add_executable(credential_migration_runner
                main.cpp
                "{(ROOT / 'src/services/ai/SubtitleCredentialMigration.cpp').as_posix()}"
                "{(ROOT / 'src/services/ai/WindowsDpapiCredentialStore.cpp').as_posix()}"
            )
            target_include_directories(credential_migration_runner PRIVATE
                "{(ROOT / 'src').as_posix()}"
                "{(ROOT / 'src/services').as_posix()}"
                "{(ROOT / 'src/common').as_posix()}"
            )
            target_link_libraries(credential_migration_runner PRIVATE Qt6::Core crypt32)
        '''), encoding="utf-8")
        if build_dir.exists():
            shutil.rmtree(build_dir)
        subprocess.run(
            ["cmake", "-S", str(source_dir), "-B", str(build_dir), "-DCMAKE_PREFIX_PATH=C:/QtClean/6.5.3/msvc2019_64"],
            check=True,
        )
        subprocess.run(["cmake", "--build", str(build_dir), "--config", "Release"], check=True)
        executable = build_dir / "Release" / "credential_migration_runner.exe"
        env = os.environ.copy()
        env["PATH"] = "C:/QtClean/6.5.3/msvc2019_64/bin" + os.pathsep + env.get("PATH", "")
        completed = subprocess.run([str(executable)], check=False, env=env)
        self.assertEqual(0, completed.returncode)


if __name__ == "__main__":
    unittest.main()
