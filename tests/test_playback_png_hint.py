"""Exercise PNG-MOV routing in the built player with real cold/warm media.

Run after building Release. No source extraction or decoder mocks are used.
Every run has its own settings/cache namespace; the warm run reuses it.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import time
import unittest
import uuid

from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
BASE_ARTIFACT = ROOT / "tests/artifacts/player_speed_settings_20260912/png_hint"
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)
KNOWN_ROUTE = "[PlaybackController] Known PNG MOV compatibility path:"
TRANSCODING = "[PlaybackController] Transcoding PNG MOV for playback:"
CACHE_HIT = "[PlaybackController] Using cached PNG MOV transcode:"


class PlaybackPngHint(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.exe = Path(os.environ.get("CGPLAY_PNG_HINT_EXE", str(ROOT / "build_win_full/bin/Release/CGPlay.exe")))
        cls.ffmpeg = ROOT / "build_win_full/runtime/ffmpeg/ffmpeg.exe"
        cls.ffprobe = cls.ffmpeg.with_name("ffprobe.exe")
        for tool in (cls.exe, cls.ffmpeg, cls.ffprobe):
            if not tool.is_file():
                raise AssertionError(f"Required existing executable missing: {tool}")
        cls.run_id = time.strftime("%Y%m%d_%H%M%S_") + uuid.uuid4().hex[:8]
        cls.artifact = BASE_ARTIFACT / cls.run_id
        cls.artifact.mkdir(parents=True)

    def command(self, name, command, timeout=180, require_nonblack=False):
        env = os.environ.copy()
        if require_nonblack:
            env["CGPLAY_SMOKE_REQUIRE_NONBLACK"] = "1"
        completed = subprocess.run(
            [str(value) for value in command], cwd=ROOT,
            capture_output=True, text=True, encoding="utf-8", errors="replace",
            creationflags=NO_WINDOW, timeout=timeout, env=env,
        )
        (self.artifact / f"{name}.stdout.log").write_text(completed.stdout, encoding="utf-8")
        (self.artifact / f"{name}.stderr.log").write_text(completed.stderr, encoding="utf-8")
        self.assertEqual(0, completed.returncode, f"{name}: {completed.stderr[-4000:]}")
        return completed

    def make_media(self, pixel_format, codec="png"):
        case = pixel_format if codec == "png" else codec
        path = self.artifact / f"{case}.mov"
        command = [self.ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
                   "-f", "lavfi", "-i", "testsrc2=size=160x96:rate=12",
                   "-t", "1", "-an"]
        if pixel_format == "rgba":
            command += ["-vf", "format=rgba,colorchannelmixer=aa=0.5"]
        command += ["-c:v", "png" if codec == "png" else "libx264", "-threads", "1",
                    "-pix_fmt", pixel_format, path]
        self.command(f"{case}_generate", command)
        probed = self.command(f"{case}_probe", [
            self.ffprobe, "-v", "error", "-select_streams", "v:0", "-show_entries",
            "stream=codec_name,pix_fmt,width,height,nb_frames", "-of", "json", path,
        ])
        metadata = json.loads(probed.stdout)["streams"][0]
        self.assertEqual(codec, metadata["codec_name"], metadata)
        self.assertEqual(pixel_format, metadata["pix_fmt"], metadata)
        self.assertEqual((160, 96), (metadata["width"], metadata["height"]), metadata)
        self.assertEqual(12, int(metadata["nb_frames"]), metadata)
        return case, path

    def smoke(self, case, path, phase, expect_png):
        report_path = self.artifact / f"{case}_{phase}.json"
        completed = self.command(f"{case}_{phase}", [
            self.exe, "--automation-background", "--automation-settings-namespace",
            f"png_hint_{self.run_id}_{case}",
            "--smoke-player", path, "--smoke-output", report_path,
        ], require_nonblack=True)
        report = json.loads(report_path.read_text(encoding="utf-8-sig"))
        self.assertTrue(report["automation"]["background"], report_path)
        self.assertEqual(0, report["automation"]["visiblePlatformWindows"], report_path)
        self.assertEqual(0, report["summary"]["fail"], report_path)
        self.assertEqual(0, report["summary"].get("skip", 0), report_path)
        self.assertEqual(0, report["summary"].get("manual", 0), report_path)
        self.assertTrue(report["coverage"]["complete"], report_path)
        opened = next(item for item in report["results"] if item["name"] == "open media")
        self.assertEqual("PASS", opened["status"], opened)
        detail = opened["details"]
        self.assertEqual(0, detail["nativeVisibleWindows"], detail)
        self.assertLessEqual(detail["openDispatchMs"], 500, detail)
        self.assertGreaterEqual(detail["playerReadyMs"], 0, detail)
        self.assertGreaterEqual(detail["firstNonBlackFrameMs"], detail["playerReadyMs"], detail)
        self.assertLess(detail["firstNonBlackFrameMs"], 60000, detail)
        self.assertTrue(detail["firstFramePixelPass"], detail)
        capture_path = Path(detail["firstFrameCapture"])
        self.assertTrue(capture_path.is_file(), detail)
        with Image.open(capture_path) as capture:
            pixels = list(capture.convert("RGB").resize((160, 96)).getdata())
        nonblack = sum(max(pixel) > 24 for pixel in pixels) / len(pixels)
        self.assertGreater(nonblack, 0.10, f"First frame is blank: {capture_path}")
        self.assertEqual(12, report["baseline"]["total"], report["baseline"])
        self.assertAlmostEqual(12, report["baseline"]["fps"], places=2)
        log = completed.stdout + completed.stderr
        if expect_png:
            self.assertIn(KNOWN_ROUTE, log)
            self.assertNotIn("Player opened with invalid time range:", log)
            if phase == "cold":
                self.assertIn(TRANSCODING, log)
            else:
                self.assertIn(CACHE_HIT, log)
                self.assertNotIn(TRANSCODING, log)
        else:
            self.assertNotIn(KNOWN_ROUTE, log)
            self.assertNotIn(TRANSCODING, log)
            self.assertNotIn(CACHE_HIT, log)
        evidence = {
            "case": case, "phase": phase, "report": str(report_path),
            "background": True, "visiblePlatformWindows": 0, "nativeVisibleWindows": 0,
            "openDispatchMs": detail["openDispatchMs"], "playerReadyMs": detail["playerReadyMs"],
            "firstNonBlackFrameMs": detail["firstNonBlackFrameMs"],
            "firstFrameCapture": str(capture_path), "nonblackFraction": nonblack,
            "knownPngRoute": KNOWN_ROUTE in log,
            "transcoded": TRANSCODING in log, "cacheHit": CACHE_HIT in log,
        }
        (self.artifact / f"{case}_{phase}_evidence.json").write_text(
            json.dumps(evidence, indent=2) + "\n", encoding="utf-8")

    def check_png(self, pixel_format):
        case, path = self.make_media(pixel_format)
        self.smoke(case, path, "cold", expect_png=True)
        self.smoke(case, path, "warm", expect_png=True)

    def test_rgb24_png_mov_cold_and_warm(self):
        self.check_png("rgb24")

    def test_rgb48be_png_mov_cold_and_warm(self):
        self.check_png("rgb48be")

    def test_rgba_png_mov_cold_and_warm(self):
        self.check_png("rgba")

    def test_h264_mov_keeps_native_route(self):
        case, path = self.make_media("yuv420p", "h264")
        self.smoke(case, path, "cold", expect_png=False)
        self.smoke(case, path, "warm", expect_png=False)


if __name__ == "__main__":
    unittest.main(verbosity=2)
