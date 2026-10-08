import sys
import unittest
from pathlib import Path
from types import SimpleNamespace

import numpy as np


TOOLS = Path(__file__).resolve().parents[1] / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

from cgplay.export.exporters.ffmpeg_exporter import FFmpegExporter
from cgplay.export.models.export_settings import ExportSettings


class FFmpegExporterColorRangeTests(unittest.TestCase):
    def _write_frame(self, color_range: int):
        settings = ExportSettings(
            output_path="unused.mp4",
            width=2,
            height=2,
            color_range=color_range,
            color_space=1,
            color_primaries=1,
            color_transfer=1,
        )
        exporter = FFmpegExporter(settings)
        stream = SimpleNamespace(pix_fmt="yuv420p", frames=[])

        def encode(frame=None):
            if frame is not None:
                stream.frames.append(frame)
            return ()

        stream.encode = encode
        exporter._stream = stream
        exporter._container = SimpleNamespace(mux=lambda packet: None)
        exporter.write_frame(np.zeros((2, 2, 3), dtype=np.uint8))
        return stream.frames[0]

    def test_unspecified_range_remains_unspecified(self):
        from av.video.reformatter import ColorRange

        frame = self._write_frame(int(ColorRange.UNSPECIFIED))

        self.assertEqual(int(ColorRange.UNSPECIFIED), frame.color_range)

    def test_explicit_ranges_are_preserved(self):
        from av.video.reformatter import ColorRange

        for color_range in (ColorRange.MPEG, ColorRange.JPEG):
            with self.subTest(color_range=color_range):
                frame = self._write_frame(int(color_range))
                self.assertEqual(int(color_range), frame.color_range)


if __name__ == "__main__":
    unittest.main()
