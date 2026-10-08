"""Build exact update GUI method extraction and run it offscreen with fake IO."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--qt", required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    source = (root / "src/ui/app/Application.cpp").read_text(encoding="utf-8")
    start = source.index("bool MainWindow::_downloadAndLaunchInstaller(")
    end = source.index("void MainWindow::_setUpdateStatusBadge(", start)
    methods = source[start:end]
    assert "void MainWindow::_offerDownloadedUpdate(" in methods
    (out / "update_methods.inc").write_text(methods, encoding="utf-8")
    (out / "extraction.json").write_text(json.dumps({
        "source": "src/ui/app/Application.cpp",
        "sourceSha256": hashlib.sha256(source.encode()).hexdigest(),
        "extractedSha256": hashlib.sha256(methods.encode()).hexdigest(),
        "methods": ["_downloadAndLaunchInstaller", "_offerDownloadedUpdate"],
        "networkAndInstaller": "stubbed; no external network or executable launch",
    }, indent=2), encoding="utf-8")
    build = out / "build"
    env = os.environ.copy()
    env["PATH"] = str(args.qt / "bin") + os.pathsep + env.get("PATH", "")
    env["QT_QPA_PLATFORM_PLUGIN_PATH"] = str(args.qt / "plugins/platforms")
    commands = [
        ["cmake", "-S", str(Path(__file__).parent), "-B", str(build),
         "-G", "Visual Studio 17 2022", "-A", "x64",
         "-DQt6_DIR=" + str(args.qt / "lib/cmake/Qt6"), "-DEXTRACTED_METHODS_DIR=" + str(out)],
        ["cmake", "--build", str(build), "--config", "Release", "--parallel", "4"],
        [str(build / "Release/update_ui_cache_contracts.exe"), str(out)],
    ]
    for index, command in enumerate(commands):
        result = subprocess.run(command, env=env, capture_output=True, timeout=120,
                                creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        (out / f"step{index}.log").write_bytes(result.stdout + result.stderr)
        if result.returncode:
            print(f"Step {index} failed: {result.returncode}")
            return result.returncode
    report = json.loads((out / "report.json").read_text(encoding="utf-8"))
    print(json.dumps({k: report[k] for k in ("background", "passed", "failed", "platform_visible_top_level_windows")}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
