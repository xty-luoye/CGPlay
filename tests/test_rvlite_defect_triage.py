"""Offline unit tests for the standalone RVLite defect triage tool."""

from __future__ import annotations

import base64
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

from rvlite_defect_triage import rvlite_triage as triage  # noqa: E402


ARTIFACT_DIR = ROOT / "tests" / "artifacts" / "rvlite_defect_triage"


class RvliteDefectTriageTests(unittest.TestCase):
    def test_systemone_payload_uses_raw_base64_and_typed_questions(self) -> None:
        payload = triage.build_systemone_payload("播放卡顿", base64.b64encode(b"png").decode("ascii"))
        self.assertEqual(payload["model"], "clef-flash")
        self.assertEqual(payload["images"], [base64.b64encode(b"png").decode("ascii")])
        self.assertFalse(payload["images"][0].startswith("data:"))
        self.assertEqual(payload["questions"]["module"]["type"], "choice")
        self.assertEqual(payload["questions"]["severity"]["type"], "score")
        self.assertEqual(
            triage.endpoint_url("http://127.0.0.1:11434/v1", "/v1/systemone"),
            "http://127.0.0.1:11434/v1/systemone",
        )

    def test_ollama_result_is_normalized_and_saved_without_image_bytes(self) -> None:
        ARTIFACT_DIR.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=ARTIFACT_DIR) as temp_dir:
            screenshot = Path(temp_dir) / "screen.png"
            screenshot.write_bytes(b"fake-png")
            output = Path(temp_dir) / "result.json"
            captured: dict[str, object] = {}

            def fake_request(url, *, timeout, payload=None):
                if url.endswith("/api/version"):
                    return {"version": "0.35.1"}
                if url.endswith("/api/tags"):
                    return {"models": [{"model": "clef-flash:9b"}]}
                captured.update(payload or {})
                return {
                    "model": "clef-flash",
                    "answers": {
                        "module": {
                            "type": "choice",
                            "choice": "video_export",
                            "probabilities": {
                                "video_export": 0.91,
                                "playback": 0.04,
                                "exr_color": 0.03,
                                "qt_ui": 0.01,
                                "other": 0.01,
                            },
                            "confidence": 0.88,
                        },
                        "severity": {
                            "type": "score",
                            "score": 2.2,
                            "probabilities": {"0": 0.01, "1": 0.05, "2": 0.64, "3": 0.30},
                            "confidence": 0.62,
                        },
                    },
                }

            with patch.object(triage, "request_json", side_effect=fake_request):
                record = triage.triage_defect(
                    "导出 H.264 后画面偏粉",
                    screenshot_path=screenshot,
                    ollama_url="http://127.0.0.1:11434",
                )
                triage.write_json_atomic(record, output)

            self.assertEqual(record["model"]["mode"], "ollama")
            self.assertEqual(record["triage"]["module"], "video_export")
            self.assertEqual(record["triage"]["severity"], "high")
            self.assertTrue(record["triage"]["human_confirmation_required"])
            self.assertIn("low_confidence", record["triage"]["human_confirmation_reasons"])
            self.assertEqual(captured["images"], [base64.b64encode(b"fake-png").decode("ascii")])
            saved = json.loads(output.read_text(encoding="utf-8"))
            self.assertNotIn(base64.b64encode(b"fake-png").decode("ascii"), output.read_text(encoding="utf-8"))
            self.assertEqual(saved["input"]["screenshot_sha256"], record["input"]["screenshot_sha256"])

    def test_unavailable_ollama_uses_deterministic_fallback(self) -> None:
        def unavailable(*args, **kwargs):
            raise triage.TriageFailure("ollama_unavailable", "connection refused")

        with patch.object(triage, "request_json", side_effect=unavailable):
            record = triage.triage_defect("Qt 菜单快捷键失效")

        self.assertEqual(record["model"]["mode"], "heuristic")
        self.assertEqual(record["triage"]["module"], "qt_ui")
        self.assertEqual(record["triage"]["severity"], "medium")
        self.assertTrue(record["triage"]["human_confirmation_required"])
        self.assertIn("ollama_unavailable", record["triage"]["human_confirmation_reasons"])
        self.assertEqual(record["errors"][0]["code"], "ollama_unavailable")

    def test_old_ollama_version_falls_back_and_records_reason(self) -> None:
        def old_version(url, *, timeout, payload=None):
            self.assertTrue(url.endswith("/api/version"))
            return {"version": "0.35.0"}

        with patch.object(triage, "request_json", side_effect=old_version):
            record = triage.triage_defect("打开 8K AV1 文件时程序崩溃")

        self.assertEqual(record["model"]["mode"], "heuristic")
        self.assertEqual(record["triage"]["module"], "playback")
        self.assertEqual(record["triage"]["severity"], "blocker")
        self.assertIn("ollama_version_too_old", record["triage"]["human_confirmation_reasons"])

    def test_force_heuristic_does_not_call_ollama(self) -> None:
        with patch.object(triage, "request_json") as request:
            record = triage.triage_defect("OCIO Gamma 不一致", force_heuristic=True)
        request.assert_not_called()
        self.assertEqual(record["model"]["mode"], "heuristic")
        self.assertEqual(record["triage"]["module"], "exr_color")
        self.assertIn("forced_heuristic", [item["code"] for item in record["errors"]])

    def test_invalid_model_response_falls_back_instead_of_raising(self) -> None:
        def invalid_response(url, *, timeout, payload=None):
            if url.endswith("/api/version"):
                return {"version": "0.35.1"}
            if url.endswith("/api/tags"):
                return {"models": [{"name": "clef-flash"}]}
            return {"answers": {"module": {"choice": "not-a-module"}}}

        with patch.object(triage, "request_json", side_effect=invalid_response):
            record = triage.triage_defect("播放器出现错误")

        self.assertEqual(record["model"]["mode"], "heuristic")
        self.assertEqual(record["errors"][0]["code"], "invalid_model_response")
        self.assertIn("invalid_model_response", record["triage"]["human_confirmation_reasons"])

    def test_missing_screenshot_keeps_text_triage_and_records_image_error(self) -> None:
        with patch.object(
            triage,
            "request_json",
            side_effect=triage.TriageFailure("ollama_unavailable", "offline"),
        ):
            record = triage.triage_defect("见截图：EXR 画面偏色", screenshot_path="missing.png")

        self.assertEqual(record["triage"]["module"], "exr_color")
        self.assertIn("image_read_error", record["triage"]["human_confirmation_reasons"])
        self.assertIn("missing_screenshot_for_visual_claim", record["triage"]["human_confirmation_reasons"])


if __name__ == "__main__":
    unittest.main()
