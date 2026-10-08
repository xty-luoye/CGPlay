#!/usr/bin/env python3
"""Run CGPlay player function smoke checks and summarize results."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from datetime import datetime
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MEDIA = ROOT / "tests" / "media" / "1080p_h264.mp4"
DEFAULT_EXE = ROOT / "build_win_full" / "package" / "CGPlay" / "CGPlay.exe"


def run_cmd(args: list[str], timeout: int = 180) -> tuple[int, str, str]:
    creationflags = subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0
    proc = subprocess.run(
        args,
        capture_output=True,
        text=True,
        timeout=timeout,
        creationflags=creationflags,
    )
    return proc.returncode, proc.stdout, proc.stderr


def settings_namespace(output: Path, pid: int | None = None) -> str:
    """Return an isolated, deterministic settings store for one smoke run.

    The process id makes concurrent invocations independent while the path
    digest and report stem keep the namespace deterministic and recognizable.
    The application adds its own ``CGPlayAutomation_`` prefix.
    """

    run_pid = os.getpid() if pid is None else pid
    stem = re.sub(r"[^A-Za-z0-9_.-]", "_", output.stem) or "report"
    path_digest = hashlib.sha256(str(output.resolve()).casefold().encode("utf-8")).hexdigest()[:8]
    return f"player_smoke_{run_pid}_{path_digest}_{stem}"[:64]


def main() -> int:
    parser = argparse.ArgumentParser(description="Run CGPlay player smoke checks.")
    parser.add_argument("--exe", type=Path, default=DEFAULT_EXE)
    parser.add_argument("--media", type=Path, default=MEDIA)
    parser.add_argument(
        "--output",
        type=Path,
        default=ROOT / "tests" / "reports" / f"player_smoke_{datetime.now().strftime('%Y%m%d_%H%M%S')}" / "player_smoke_report.json",
    )
    args = parser.parse_args()

    args.output.parent.mkdir(parents=True, exist_ok=True)
    namespace = settings_namespace(args.output)
    cmd = [
        str(args.exe),
        "--automation-background",
        "--automation-settings-namespace",
        namespace,
        "--smoke-player",
        str(args.media),
        "--smoke-output",
        str(args.output),
    ]
    code, stdout, stderr = run_cmd(cmd)
    report = json.loads(args.output.read_text(encoding="utf-8")) if args.output.exists() else {}

    summary = report.get("summary", {})
    automation = report.get("automation", {})
    coverage = report.get("coverage", {})
    background_ok = automation.get("background") is True and automation.get("visiblePlatformWindows") == 0
    strict_ok = summary.get("manual", 0) == 0 and coverage.get("complete") is True
    print(json.dumps({
        "exit_code": code,
        "pass": summary.get("pass", 0),
        "fail": summary.get("fail", 0),
        "skip": summary.get("skip", 0),
        "manual": summary.get("manual", 0),
        "background": automation.get("background", False),
        "visible_windows": automation.get("visiblePlatformWindows", -1),
        "coverage_complete": coverage.get("complete", False),
        "uncovered_commands": coverage.get("uncoveredCommands", []),
        "uncovered_shortcuts": coverage.get("uncoveredShortcuts", []),
        "settings_namespace": namespace,
        "report": str(args.output),
    }, ensure_ascii=False, indent=2))

    if stdout.strip():
        print(stdout.strip())
    if stderr.strip():
        print(stderr.strip(), file=sys.stderr)

    return 1 if code != 0 or summary.get("fail", 0) or not background_ok or not strict_ok else 0


if __name__ == "__main__":
    raise SystemExit(main())
