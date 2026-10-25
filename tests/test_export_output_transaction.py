"""Focused input-integrity and atomic export regression fixtures.

Run through the repository's background test harness. These tests never create
Qt/application windows and write fixtures only beneath tests/artifacts.
"""
import hashlib
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

import av
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
TOOLS = Path(os.environ.get("CGPLAY_TOOLS_ROOT", ROOT / "tools"))
ARTIFACTS = Path(os.environ.get(
    "CGPLAY_EXPORT_TEST_ARTIFACTS", ROOT / "tests/artifacts/export_output_transaction"))
sys.path.insert(0, str(TOOLS))

from cgplay.export.export_engine import _parse_optional_arguments, _write_error_log
from cgplay.export.export_job import export_frames
from cgplay.export.models.export_settings import ExportSettings
from cgplay.export.utils.image_reader import ImageReader
from cgplay.export.utils.output_transaction import OutputTransaction


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write_video(path, count):
    with av.open(str(path), "w") as container:
        stream = container.add_stream("libx264", rate=24)
        stream.width, stream.height, stream.pix_fmt = 96, 64, "yuv420p"
        for index in range(count):
            pixels = np.full((64, 96, 3), index * 3 % 255, dtype=np.uint8)
            frame = av.VideoFrame.from_ndarray(pixels, format="bgr24")
            frame.pts = index
            for packet in stream.encode(frame):
                container.mux(packet)
        for packet in stream.encode():
            container.mux(packet)


def count_frames(path):
    with av.open(str(path)) as container:
        return sum(1 for _ in container.decode(video=0))


class ExportOutputTransactionTests(unittest.TestCase):
    def setUp(self):
        ARTIFACTS.mkdir(parents=True, exist_ok=True)
        self.directory = Path(tempfile.mkdtemp(prefix=self._testMethodName + "-", dir=ARTIFACTS))
        self.source = self.directory / "source.mp4"
        self.source.write_bytes(b"original input bytes")
        self.output = self.directory / "output.mp4"
        self.original = b"existing target bytes must survive"
        self.output.write_bytes(self.original)

    def test_same_file_and_hard_link_are_rejected_before_encoder_open(self):
        link = self.directory / "hard-link.mp4"
        os.link(self.source, link)
        for target in (self.source, link, self.directory / "." / self.source.name):
            with self.subTest(target=target), patch("cgplay.export.export_job._open_exporter") as encoder:
                with self.assertRaisesRegex(ValueError, "overwrite an input"):
                    export_frames(str(self.source), ExportSettings(output_path=str(target)))
                encoder.assert_not_called()
                self.assertEqual(b"original input bytes", self.source.read_bytes())

    def test_sequence_members_outside_range_and_sidecar_are_protected(self):
        first = self.directory / "render.1001.png"
        later = self.directory / "render.1002.png"
        first.write_bytes(b"first source image")
        later.write_bytes(b"unselected source image")
        with patch("cgplay.export.export_job._open_exporter") as encoder:
            with self.assertRaisesRegex(ValueError, "overwrite an input"):
                export_frames(str(first), ExportSettings(output_path=str(later)),
                              start_frame=1001, end_frame=1001)
            encoder.assert_not_called()
        sidecar = self.directory / "review.json"
        sidecar.write_bytes(b"[]")
        with self.assertRaisesRegex(ValueError, "overwrite an input"):
            export_frames(str(self.source), ExportSettings(output_path=str(sidecar)),
                          protected_paths=[str(sidecar)])
        self.assertEqual(b"unselected source image", later.read_bytes())
        self.assertEqual(b"[]", sidecar.read_bytes())

    def test_log_alias_cannot_mutate_input_or_target(self):
        for log_target in (self.source, self.output):
            with self.subTest(log_target=log_target):
                before = digest(log_target)
                with self.assertRaisesRegex(ValueError, "overwrite an input"):
                    export_frames(str(self.source), ExportSettings(
                        output_path=str(self.output), log_file=str(log_target)))
                self.assertEqual(before, digest(log_target))
        log = self.directory / "export_error.log"
        os.link(self.source, log)
        _write_error_log("injected failure", str(self.output), [str(self.source)])
        self.assertEqual(b"original input bytes", self.source.read_bytes())

    def test_failure_and_cancel_leave_existing_target_and_inputs_unchanged(self):
        first = self.directory / "render.1001.png"
        second = self.directory / "render.1002.png"
        first.write_bytes(b"first source image")
        second.write_bytes(b"second source image")
        expected = {path: digest(path) for path in (first, second, self.output)}
        for failure in ("read", "encode", "flush", "callback"):
            with self.subTest(failure=failure):
                staged = []
                progress = []

                class Exporter:
                    def write_frame(inner, frame):
                        staged[0].write_bytes(b"incomplete export")
                        if failure == "encode":
                            raise RuntimeError("injected encode failure")

                    def close(inner):
                        if failure == "flush":
                            raise RuntimeError("injected flush failure")

                def open_exporter(settings, logger):
                    staged.append(Path(settings.output_path))
                    self.assertNotEqual(self.output, staged[0])
                    return Exporter()

                def read(path):
                    if failure == "read" and Path(path) == second:
                        raise RuntimeError("injected read failure")
                    return np.zeros((64, 96, 3), dtype=np.uint8)

                def notify(percent, message):
                    progress.append(percent)
                    if failure == "callback" and percent > 5:
                        raise RuntimeError("injected cancellation")

                with patch("cgplay.export.export_job._open_exporter", side_effect=open_exporter), \
                        patch.object(ImageReader, "read_frame", side_effect=read):
                    with self.assertRaises(RuntimeError):
                        export_frames(str(first), ExportSettings(output_path=str(self.output)),
                                      progress_callback=notify)
                self.assertNotIn(100, progress)
                self.assertFalse(staged[0].exists())
                self.assertFalse(staged[0].parent.exists())
                self.assertEqual(expected, {path: digest(path) for path in expected})

    def test_engine_error_log_never_overwrites_unselected_sequence_hard_link(self):
        import contextlib
        import io
        from cgplay.export.export_engine import run_export

        # Dependency/codec errors happen before scan_sequence, so protecting
        # only paths collected by export_frames is insufficient for this log.
        for failure in ("dependency", "codec", "selected_decode"):
            with self.subTest(failure=failure):
                directory = self.directory / failure
                directory.mkdir()
                first = directory / "render.1001.png"
                broken = directory / "render.1002.png"
                unselected = directory / "render.1003.png"
                first.write_bytes(b"first synthetic image")
                broken.write_bytes(b"broken synthetic image")
                unselected.write_bytes(b"unselected source must never be a log")
                legacy_log = directory / "export_error.log"
                os.link(unselected, legacy_log)
                output = directory / "result.mp4"
                output.write_bytes(self.original)
                expected = {path: digest(path) for path in (first, broken, unselected, output)}
                stdout, stderr = io.StringIO(), io.StringIO()

                def read(path, **kwargs):
                    if Path(path) == broken:
                        raise RuntimeError("injected selected-frame decode failure")
                    return np.zeros((64, 96, 3), dtype=np.uint8)

                missing = ["injected dependency"] if failure == "dependency" else []
                codec = "invalid-fixture-codec" if failure == "codec" else "h264"
                with patch("cgplay.export.export_engine._check_dependencies", return_value=("fixture", missing)), \
                        patch.object(ImageReader, "read_frame", side_effect=read), \
                        contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                    code = run_export(str(first), str(output), 24, 0, 1,
                                      codec=codec, frame_numbering="relative")

                self.assertEqual(1, code)
                self.assertEqual(expected, {path: digest(path) for path in expected})
                self.assertTrue(os.path.samefile(unselected, legacy_log))
                self.assertEqual(b"unselected source must never be a log", legacy_log.read_bytes())
                logs = list(directory.glob(".cgplay-export-error-*.log"))
                self.assertEqual(1, len(logs), stderr.getvalue())
                self.assertFalse(os.path.samefile(unselected, logs[0]))
                self.assertIn("ERROR_LOG: " + str(logs[0].resolve()), stderr.getvalue())
                expected_message = {"dependency": "Missing dependencies",
                                    "codec": "Unsupported export codec",
                                    "selected_decode": "injected selected-frame decode failure"}[failure]
                self.assertIn(expected_message, logs[0].read_text(encoding="utf-8"))

    def test_replace_failure_keeps_old_target_and_cleans_only_owned_staging(self):
        supplied_staging = self.directory / "supervisor-staging"
        supplied_staging.mkdir()
        unrelated = supplied_staging / "owned-by-supervisor.txt"
        unrelated.write_bytes(b"keep")
        with self.assertRaisesRegex(OSError, "injected replace"):
            with OutputTransaction(str(self.output), [str(self.source)], str(supplied_staging)) as job:
                staged = job.staged_path
                staged.write_bytes(b"completed encoded output")
                with patch("cgplay.export.utils.output_transaction.os.replace",
                           side_effect=OSError("injected replace failure")):
                    job.commit()
        self.assertEqual(self.original, self.output.read_bytes())
        self.assertFalse(staged.parent.exists())
        self.assertTrue(supplied_staging.exists())
        self.assertEqual(b"keep", unrelated.read_bytes())

    def test_destination_identity_is_checked_again_before_commit(self):
        with OutputTransaction(str(self.output), [str(self.source)]) as job:
            job.staged_path.write_bytes(b"completed encoded output")
            self.output.unlink()
            os.link(self.source, self.output)
            with self.assertRaisesRegex(ValueError, "overwrite an input"):
                job.commit()
        self.assertEqual(b"original input bytes", self.source.read_bytes())
        self.assertEqual(b"original input bytes", self.output.read_bytes())

    def test_real_video_success_replaces_only_after_finalize_and_reports_done(self):
        write_video(self.source, 90)
        write_video(self.output, 12)
        source_before, old_target = digest(self.source), digest(self.output)
        progress = []

        def notify(percent, message):
            progress.append(percent)
            if percent < 100:
                self.assertEqual(old_target, digest(self.output))
            else:
                self.assertEqual(90, count_frames(self.output))

        result = export_frames(str(self.source), ExportSettings(output_path=str(self.output)),
                               progress_callback=notify)
        self.assertEqual(str(self.output), result)
        self.assertEqual(source_before, digest(self.source))
        self.assertEqual(90, count_frames(self.output))
        self.assertEqual(100, progress[-1])
        self.assertEqual(1, progress.count(100))

    def test_successful_commit_is_not_rolled_back_by_completion_observer(self):
        with patch("cgplay.export.export_job._export_video_file") as encode:
            def complete(input_path, settings, *arguments):
                Path(settings.output_path).write_bytes(b"completed encoded output")
            encode.side_effect = complete

            def notify(percent, message):
                if percent == 100:
                    raise RuntimeError("completion observer failed")

            result = export_frames(str(self.source), ExportSettings(output_path=str(self.output)),
                                   progress_callback=notify)
        self.assertEqual(str(self.output), result)
        self.assertEqual(b"completed encoded output", self.output.read_bytes())

    def test_legacy_cli_options_and_optional_supervisor_staging(self):
        self.assertEqual(("20M", "h264", None, "source", None, None), _parse_optional_arguments([]))
        self.assertEqual(("8M", "prores_4444", "a.json", "relative", "staging", None),
                         _parse_optional_arguments(["8M", "prores_4444", "--annotations", "a.json",
                                                    "--relative-frames", "--staging-dir", "staging"]))
        for invalid in (["--staging-dir"], ["--annotations", "--relative-frames"], ["--unknown"]):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                _parse_optional_arguments(invalid)


if __name__ == "__main__":
    unittest.main()
