"""Real OIIO/PyAV coverage for player-relative export ranges and EXR channels."""
import json
import os
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import patch

import av
import numpy as np
import OpenImageIO as oiio

ROOT = Path(__file__).resolve().parents[1]
TOOLS = Path(os.environ.get("CGPLAY_TOOLS_ROOT", os.environ.get("CGPLAY_EXPORT_TOOLS_DIR", ROOT / "tools")))
ARTIFACTS = Path(os.environ.get(
    "CGPLAY_EXPORT_TEST_ARTIFACTS", ROOT / "tests/artifacts/export_sequence_contract"))
sys.path.insert(0, str(TOOLS))

from cgplay.export.export_engine import run_export
from cgplay.export.export_job import export_frames
from cgplay.export.models.export_settings import ExportSettings
from cgplay.export.utils import image_reader
from cgplay.export.utils.image_reader import ImageReader


def write_exr(path, names, values):
    data = np.empty((64, 96, len(names)), dtype=np.float32)
    data[:] = values
    spec = oiio.ImageSpec(96, 64, len(names), oiio.FLOAT)
    spec.channelnames = names
    output = oiio.ImageOutput.create(str(path))
    assert output.open(str(path), spec), output.geterror()
    try:
        assert output.write_image(data), output.geterror()
    finally:
        output.close()


def decode(path):
    with av.open(str(path)) as container:
        return [frame.to_ndarray(format="bgra") for frame in container.decode(video=0)]


class ExportSequenceContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        ARTIFACTS.mkdir(parents=True, exist_ok=True)
        cls.sequence = ARTIFACTS / "sequence"
        cls.sequence.mkdir(exist_ok=True)
        for number, value in [(1001, .05), (1002, .2), (1003, .8)]:
            write_exr(cls.sequence / f"render.{number}.exr", ["R", "G", "B"], [value] * 3)
        cls.source = cls.sequence / "render.1001.exr"
        (ARTIFACTS / "runtime.json").write_text(json.dumps({
            "python": sys.executable, "av": av.__version__, "oiio": oiio.__version__,
            "tools": str(TOOLS), "loadedImageReader": image_reader.__file__, "backgroundMode": True,
        }, indent=2), encoding="utf-8")
        assert Path(image_reader.__file__).resolve().is_relative_to(TOOLS.resolve())

    def setUp(self):
        self.output_dir = ARTIFACTS / self._testMethodName
        self.output_dir.mkdir(exist_ok=True)

    def export(self, source=None, start=0, end=2, numbering="relative", annotations=None, name="out.mov"):
        output = self.output_dir / name
        output.unlink(missing_ok=True)
        sidecar = None
        if annotations is not None:
            sidecar = self.output_dir / (name + ".annotations.json")
            sidecar.write_text(json.dumps(annotations), encoding="utf-8")
        result = run_export(str(source or self.source), str(output), 24, start, end,
                            codec="prores_4444", annotations_json=str(sidecar) if sidecar else None,
                            frame_numbering=numbering)
        return result, output

    @staticmethod
    def annotation(frame):
        return {"frame": frame, "type": "rectangle", "color": "#ff0000",
                "points": [{"x": 8, "y": 8}, {"x": 72, "y": 48}]}

    def test_relative_single_frame_matches_absolute_source(self):
        for relative, absolute in [(0, 1001), (1, 1002), (2, 1003)]:
            with self.subTest(relative=relative):
                code, output = self.export(start=relative, end=relative, name=f"relative{relative}.mov")
                source_code, reference = self.export(start=absolute, end=absolute, numbering="source",
                                                     name=f"source{absolute}.mov")
                self.assertEqual((0, 0), (code, source_code))
                frames = decode(output)
                self.assertEqual(1, len(frames))
                np.testing.assert_array_equal(frames[0], decode(reference)[0])

    def test_relative_annotations_keep_source_position_after_trim(self):
        code, clean = self.export(start=1, end=2, name="clean.mov")
        annotated_code, annotated = self.export(start=1, end=2, annotations=[self.annotation(2)])
        self.assertEqual((0, 0), (code, annotated_code))
        clean_frames, frames = decode(clean), decode(annotated)
        self.assertEqual(2, len(frames))
        np.testing.assert_array_equal(clean_frames[0], frames[0])
        self.assertGreater(np.abs(clean_frames[1].astype(float) - frames[1]).mean(), 1)

    def test_annotation_on_unexported_position_fails_before_output(self):
        code, output = self.export(start=1, end=1, annotations=[self.annotation(0)])
        self.assertEqual(1, code)
        self.assertFalse(output.exists())

    def test_sparse_relative_range_preserves_number_gaps(self):
        source = self.output_dir / "sparse.1001.exr"
        write_exr(source, ["R", "G", "B"], [.2] * 3)
        write_exr(self.output_dir / "sparse.1003.exr", ["R", "G", "B"], [.4] * 3)
        code, clean = self.export(source=source, name="clean.mov")
        annotated_code, annotated = self.export(source=source, annotations=[self.annotation(2)])
        self.assertEqual((0, 0), (code, annotated_code))
        clean_frames, frames = decode(clean), decode(annotated)
        self.assertEqual(2, len(frames))
        np.testing.assert_array_equal(clean_frames[0], frames[0])
        self.assertGreater(np.abs(clean_frames[1].astype(float) - frames[1]).mean(), 1)
        hole_code, hole = self.export(source=source, start=1, end=1, name="hole.mov")
        self.assertEqual(1, hole_code)
        self.assertFalse(hole.exists())

    def test_source_numbering_default_retains_legacy_sparse_annotations(self):
        source = self.output_dir / "sparse.0001.exr"
        write_exr(source, ["R", "G", "B"], [.2] * 3)
        write_exr(self.output_dir / "sparse.0003.exr", ["R", "G", "B"], [.4] * 3)
        output = self.output_dir / "legacy.mov"
        settings = ExportSettings(output_path=str(output), codec="prores_4444")
        export_frames(str(source), settings, start_frame=1, end_frame=3,
                      annotations=[self.annotation(3)])
        self.assertEqual(2, len(decode(output)))

    def test_range_defaults_and_invalid_ranges(self):
        frames = {1001: "first", 1003: "last"}
        self.assertEqual((0, 2, [(0, "first"), (2, "last")]),
                         ImageReader.get_sequence_range(frames, frame_numbering="relative"))
        for start, end, numbering in [(4, 5, "relative"), (1, 1, "relative"),
                                      (2, 1, "relative"), (-1, 1, "relative"),
                                      (1, 3, "source"), (1002, 1002, "source")]:
            with self.subTest(start=start, end=end, numbering=numbering):
                with self.assertRaises(ValueError):
                    ImageReader.get_sequence_range(frames, start, end, frame_numbering=numbering)
        with self.assertRaises(ValueError):
            ImageReader.get_sequence_range({})
        with self.assertRaises(ValueError):
            ImageReader.get_sequence_range(frames, frame_numbering="guess")

    def test_cli_relative_switch_and_legacy_default(self):
        env = dict(os.environ, PYTHONPATH=str(TOOLS))
        sidecar = self.output_dir / "relative.annotations.json"
        sidecar.write_text(json.dumps([self.annotation(1)]), encoding="utf-8")
        for arguments, expected in [(["1", "1", "20M", "prores_4444", "--relative-frames"], 0),
                                    (["1", "1", "20M", "prores_4444", "--relative-frames", "--annotations", str(sidecar)], 0),
                                    (["1002", "1002", "20M", "prores_4444"], 0),
                                    (["1", "1", "20M", "prores_4444"], 1)]:
            output = self.output_dir / f"cli_{len(list(self.output_dir.glob('*.log')))}.mov"
            process = subprocess.run([sys.executable, "-m", "cgplay.export.export_engine",
                                      str(self.source), str(output), "24", *arguments],
                                     cwd=TOOLS, env=env, capture_output=True, text=True,
                                     encoding="utf-8", errors="replace", timeout=30,
                                     creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            self.assertEqual(expected, process.returncode, process.stdout + process.stderr)
            if expected == 0:
                self.assertEqual(1, len(decode(output)))
            else:
                self.assertFalse(output.exists())

    def test_video_relative_flag_keeps_zero_based_frame_range(self):
        code, source = self.export(name="source_video.mov")
        self.assertEqual(0, code)
        relative_code, relative = self.export(source=source, start=1, end=1, name="relative.mov")
        source_code, absolute = self.export(source=source, start=1, end=1,
                                            numbering="source", name="source_numbering.mov")
        self.assertEqual((0, 0), (relative_code, source_code))
        frames = decode(relative)
        self.assertEqual(1, len(frames))
        np.testing.assert_array_equal(frames[0], decode(absolute)[0])

    def test_rgbz_exports_opaque_without_alpha(self):
        for index, z in enumerate([0, .5, 10]):
            source = self.output_dir / f"rgbzCase{index}.exr"
            write_exr(source, ["R", "G", "B", "Z"], [.2, .4, .6, z])
            code, output = self.export(source=source, start=0, end=0, name=f"z{z}.mov")
            self.assertEqual(0, code)
            frame = decode(output)[0]
            self.assertTrue(np.all(frame[:, :, 3] == 255))
            self.assertGreater(frame[:, :, :3].mean(), 1)

    def test_named_alpha_and_reordered_channels_are_preserved(self):
        for index, alpha in enumerate([0, .25, 1]):
            source = self.output_dir / f"rgbaCase{index}.exr"
            write_exr(source, ["Z", "A", "B", "G", "R"], [10, alpha, .6, .4, .2])
            code, output = self.export(source=source, start=0, end=0, name=f"alpha{alpha}.mov")
            self.assertEqual(0, code)
            frame = decode(output)[0]
            self.assertLessEqual(abs(float(frame[:, :, 3].mean()) - int(alpha * 255)), 1)
            expected = image_reader._process_pixels(np.full((64, 96, 3), [.2, .4, .6], dtype=np.float32))
            self.assertLess(np.abs(frame[:, :, :3].astype(float) - expected).mean(), 3)

    def test_single_color_layer_uses_only_its_own_alpha(self):
        source = self.output_dir / "layered.exr"
        write_exr(source, ["beauty.R", "beauty.G", "beauty.B", "other.A", "Z"],
                  [.2, .4, .6, 0, 0])
        code, output = self.export(source=source, start=0, end=0)
        self.assertEqual(0, code)
        self.assertTrue(np.all(decode(output)[0][:, :, 3] == 255))
        write_exr(source, ["beauty.R", "beauty.G", "beauty.B", "beauty.A", "Z"],
                  [.2, .4, .6, .5, 0])
        code, output = self.export(source=source, start=0, end=0, name="with_alpha.mov")
        self.assertEqual(0, code)
        self.assertLessEqual(abs(float(decode(output)[0][:, :, 3].mean()) - 127), 1)

    def test_unsupported_exr_channels_fail_without_metadata_losing_fallback(self):
        for index, names in enumerate([["Z"], ["N.X", "N.Y", "N.Z"],
                                      ["R", "G", "B", "beauty.R", "beauty.G", "beauty.B"],
                                      ["beauty.R", "beauty.G", "beauty.B", "beauty.A", "beauty.Alpha"]]):
            source = self.output_dir / f"unsupported{index}.exr"
            write_exr(source, names, [.5] * len(names))
            with patch.object(image_reader, "_read_imageio") as fallback:
                code, output = self.export(source=source, start=0, end=0, name=f"failed{index}.mov")
                self.assertEqual(1, code)
                self.assertFalse(output.exists())
                fallback.assert_not_called()

    def test_unrelated_aov_aliases_do_not_reject_the_color_layer(self):
        for has_alpha in (False, True):
            names = ["beauty.R", "beauty.G", "beauty.B"]
            values = [.2, .4, .6]
            if has_alpha:
                names.append("beauty.A")
                values.append(.5)
            source = self.output_dir / f"aovCase{has_alpha}.exr"
            write_exr(source, names + ["mask.A", "mask.Alpha", "mask.R", "mask.r"],
                      values + [0, 1, .1, .9])
            code, output = self.export(source=source, start=0, end=0, name=f"aov{has_alpha}.mov")
            self.assertEqual(0, code)
            reference = self.output_dir / f"referenceCase{has_alpha}.exr"
            write_exr(reference, names, values)
            reference_code, reference_output = self.export(
                source=reference, start=0, end=0, name=f"reference{has_alpha}.mov")
            self.assertEqual(0, reference_code)
            np.testing.assert_array_equal(decode(output)[0], decode(reference_output)[0])

    def test_multi_part_exr_fails_explicitly(self):
        source = self.output_dir / "multipart.exr"
        specs = [oiio.ImageSpec(96, 64, 3, oiio.FLOAT) for _ in range(2)]
        for index, spec in enumerate(specs):
            spec.channelnames = ["R", "G", "B"]
            spec.attribute("oiio:subimagename", f"part{index}")
        output = oiio.ImageOutput.create(str(source))
        self.assertTrue(output.open(str(source), specs), output.geterror())
        try:
            self.assertTrue(output.write_image(np.full((64, 96, 3), .2, dtype=np.float32)), output.geterror())
            self.assertTrue(output.open(str(source), specs[1], "AppendSubimage"), output.geterror())
            self.assertTrue(output.write_image(np.full((64, 96, 3), .4, dtype=np.float32)), output.geterror())
        finally:
            output.close()
        code, destination = self.export(source=source, start=0, end=0)
        self.assertEqual(1, code)
        self.assertFalse(destination.exists())

    def test_exr_requires_oiio_but_png_fallback_dispatch_stays_available(self):
        from PIL import Image
        png = self.output_dir / "rgba.png"
        Image.new("RGBA", (96, 64), (10, 20, 30, 127)).save(png)
        expected = ImageReader.read_frame(str(png))
        self.assertTrue(np.all(expected[:, :, 3] == 127))
        # The full installer uses OIIO and does not include optional imageio.
        # Exercise the unchanged fallback dispatch without installing a library.
        with patch.object(image_reader, "_HAS_OIIO", False), \
                patch.object(image_reader, "_read_imageio", return_value=expected) as fallback:
            with self.assertRaisesRegex(RuntimeError, "OpenImageIO"):
                ImageReader.read_frame(str(self.source))
            frame = ImageReader.read_frame(str(png))
            np.testing.assert_array_equal(expected, frame)
            fallback.assert_called_once_with(str(png))

    def test_first_frame_is_fully_decoded_only_once(self):
        original = ImageReader.read_frame
        with patch.object(ImageReader, "read_frame", wraps=original) as read:
            code, output = self.export()
        self.assertEqual(0, code)
        self.assertEqual(3, len(decode(output)))
        self.assertEqual([str(self.sequence / f"render.{number}.exr") for number in [1001, 1002, 1003]],
                         [call.args[0] for call in read.call_args_list])

    def test_read_encode_flush_and_callback_interrupt_clean_partial_outputs(self):
        original_read = ImageReader.read_frame
        for failure in ["read", "encode", "flush", "interrupt"]:
            with self.subTest(failure=failure):
                output = self.output_dir / f"{failure}.mov"
                progress = []
                class Exporter:
                    closed = False
                    def write_frame(self, frame):
                        output.write_bytes(b"partial export")
                        if failure == "encode":
                            raise RuntimeError("injected encode failure")
                    def close(self):
                        self.closed = True
                        if failure == "flush":
                            raise RuntimeError("injected flush failure")
                exporter = Exporter()
                def read(path):
                    if failure == "read" and str(path).endswith("1002.exr"):
                        raise IOError("injected decode failure")
                    return original_read(path)
                def notify(percent, message):
                    progress.append(percent)
                    if failure == "interrupt" and percent > 5:
                        raise RuntimeError("interrupted through callback")
                settings = ExportSettings(output_path=str(output), codec="prores_4444")
                with patch("cgplay.export.export_job._open_exporter", return_value=exporter), \
                        patch.object(ImageReader, "read_frame", side_effect=read):
                    with self.assertRaises(RuntimeError):
                        export_frames(str(self.source), settings, progress_callback=notify,
                                      start_frame=0, end_frame=2, frame_numbering="relative")
                self.assertTrue(exporter.closed)
                self.assertFalse(output.exists())
                self.assertNotIn(100, progress)
                self.assertTrue(self.source.exists())


if __name__ == "__main__":
    unittest.main()
