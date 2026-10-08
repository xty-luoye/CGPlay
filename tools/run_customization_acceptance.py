#!/usr/bin/env python3
"""Serial acceptance matrix for CGPlay customization persistence/runtime behavior.

The app has no headless settings RPC, so this harness drives the same persisted
stores used by the settings UI and verifies the effective runtime dump/capture.
All mutations are serialized and restored in a finally block.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile
from datetime import datetime
from pathlib import Path

try:
    import winreg
except ImportError:  # pragma: no cover - this acceptance runs on Windows.
    winreg = None


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_EXE = ROOT / "build_win_full" / "bin" / "Release" / "CGPlay.exe"
SETTINGS_NAMESPACE = "CustomizationAcceptance"
SETTINGS_APPLICATION = f"CGPlayAutomation_{SETTINGS_NAMESPACE}"
APPDATA = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData/Local")) / "CGPlay" / SETTINGS_APPLICATION
REG_PATH = rf"Software\CGPlay\{SETTINGS_APPLICATION}"


def reg_snapshot() -> dict[str, dict[str, object]]:
    if winreg is None:
        raise RuntimeError("winreg is unavailable")
    snapshot: dict[str, dict[str, object]] = {}

    def walk(key: object, prefix: str) -> None:
        for i in range(winreg.QueryInfoKey(key)[1]):
            name, value, kind = winreg.EnumValue(key, i)
            snapshot[f"{prefix}/{name}"] = {"value": value, "kind": kind}
        for i in range(winreg.QueryInfoKey(key)[0]):
            child = winreg.EnumKey(key, i)
            with winreg.OpenKey(key, child, 0, winreg.KEY_READ) as sub:
                walk(sub, f"{prefix}/{child}")

    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, REG_PATH, 0, winreg.KEY_READ) as root:
            walk(root, REG_PATH)
    except FileNotFoundError:
        pass
    return snapshot


def reg_set(path: str, name: str, value: object, kind: int | None = None) -> None:
    if winreg is None:
        raise RuntimeError("winreg is unavailable")
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, path) as key:
        if kind is None:
            kind = winreg.REG_DWORD if isinstance(value, int) else winreg.REG_SZ
        winreg.SetValueEx(key, name, 0, kind, value)


def reg_delete_tree(path: str) -> None:
    if winreg is None:
        return
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, path, 0, winreg.KEY_READ | winreg.KEY_WRITE) as key:
            children = [winreg.EnumKey(key, i) for i in range(winreg.QueryInfoKey(key)[0])]
        for child in children:
            reg_delete_tree(f"{path}\\{child}")
        winreg.DeleteKey(winreg.HKEY_CURRENT_USER, path)
    except FileNotFoundError:
        pass


def reg_restore(snapshot: dict[str, dict[str, object]]) -> None:
    reg_delete_tree(REG_PATH)
    for full, item in snapshot.items():
        prefix, name = full.rsplit("/", 1)
        reg_set(prefix.replace("/", "\\"), name, item["value"], int(item["kind"]))


def copy_tree_state(root: Path) -> dict[str, bytes | None]:
    state: dict[str, bytes | None] = {}
    for name in ("shortcuts.json", "mouse_bindings.json", "gamepad_bindings.json"):
        path = root / name
        state[name] = path.read_bytes() if path.exists() else None
    workspace = root / "workspaces"
    if workspace.exists():
        for path in workspace.glob("*"):
            if path.is_file():
                state[str(path.relative_to(root))] = path.read_bytes()
    return state


def restore_tree_state(root: Path, state: dict[str, bytes | None]) -> None:
    root.mkdir(parents=True, exist_ok=True)
    # Only restore files owned by this acceptance harness.  AppLocalData also
    # contains Codex's live Git/cache trees and must never be swept here.
    owned = {root / name for name in ("shortcuts.json", "mouse_bindings.json", "gamepad_bindings.json")}
    workspace = root / "workspaces"
    if workspace.exists():
        owned.update(path for path in workspace.glob("*") if path.is_file())
    for path in owned:
        relative = str(path.relative_to(root))
        if relative not in state:
            path.unlink(missing_ok=True)
    for relative, data in state.items():
        path = root / relative
        if data is None:
            path.unlink(missing_ok=True)
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)


def run_process(exe: Path, args: list[str], timeout: int = 30) -> tuple[int, str, str]:
    env = os.environ.copy()
    args = ["--automation-settings-namespace", SETTINGS_NAMESPACE, *args]
    env["QT_IM_MODULE"] = "none"
    proc = subprocess.run(
        [str(exe), *args],
        cwd=str(ROOT),
        env=env,
        capture_output=True,
        timeout=timeout,
        text=False,
    )
    return proc.returncode, proc.stdout.decode("utf-8", "replace"), proc.stderr.decode("utf-8", "replace")


class Acceptance:
    def __init__(self, exe: Path, output: Path) -> None:
        self.exe = exe
        self.output = output
        self.output.mkdir(parents=True, exist_ok=True)
        self.checks: list[dict[str, object]] = []
        self.reg = reg_snapshot()
        self.tree = copy_tree_state(APPDATA)
        self.workspace_dir = APPDATA / "workspaces"

    def check(self, name: str, passed: bool, details: object = None) -> None:
        item: dict[str, object] = {"name": name, "passed": bool(passed)}
        if details is not None:
            item["details"] = details
        self.checks.append(item)
        print(f"{'PASS' if passed else 'FAIL'} {name}")

    def dump(self, name: str) -> dict[str, object]:
        path = self.output / f"{name}.json"
        code, stdout, stderr = run_process(self.exe, ["--dump-runtime", "--dump-runtime-output", str(path)])
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except Exception as exc:
            data = {"parseError": str(exc)}
        data["_process"] = {"exitCode": code, "stderrTail": stderr[-1200:]}
        (self.output / f"{name}.stdout.txt").write_text(stdout, encoding="utf-8")
        (self.output / f"{name}.stderr.txt").write_text(stderr, encoding="utf-8")
        return data

    def capture(self, name: str, width: int, height: int) -> None:
        path = self.output / f"{name}.png"
        code, _, stderr = run_process(
            self.exe,
            ["--capture-demo", "--capture-output", str(path), "--capture-width", str(width), "--capture-height", str(height), "--capture-delay-ms", "500"],
            timeout=30,
        )
        self.check(f"capture exits cleanly: {name}", code == 0 and path.exists(), {"exitCode": code, "path": str(path), "stderrTail": stderr[-400:]})

    def set_appearance(self, **values: object) -> None:
        for key, value in values.items():
            reg_set(f"{REG_PATH}\\appearance", key, value)

    def run(self) -> int:
        try:
            self.matrix()
            result = {
                "timestamp": datetime.now().isoformat(timespec="seconds"),
                "exe": str(self.exe),
                "checks": self.checks,
                "summary": {
                    "pass": sum(1 for item in self.checks if item["passed"]),
                    "fail": sum(1 for item in self.checks if not item["passed"]),
                    "total": len(self.checks),
                },
            }
            (self.output / "customization_acceptance.json").write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
            return 0 if result["summary"]["fail"] == 0 else 1
        finally:
            reg_restore(self.reg)
            restore_tree_state(APPDATA, self.tree)

    def matrix(self) -> None:
        # Presets must be distinct and must ignore stale custom surface keys.
        preset_colors: dict[str, str] = {}
        for mode in ("dark", "light", "glass", "highContrast"):
            self.set_appearance(mode=mode)
            data = self.dump(f"mode_{mode}")
            appearance = data.get("appearance", {})
            self.check(f"mode {mode} exits", data.get("_process", {}).get("exitCode") == 0)
            self.check(f"mode {mode} effective", appearance.get("mode") == mode, appearance)
            preset_colors[mode] = str(appearance.get("panelColor"))
        self.check("preset modes have distinct panel surfaces", len(set(preset_colors.values())) == 4, preset_colors)

        image = ROOT / "tests" / "media" / "1080p_h264.mp4"
        for background_type in ("solid", "gradient", "image", "texture"):
            self.set_appearance(
                mode="custom",
                backgroundType=background_type,
                backgroundImage=str(image).replace("\\", "/") if background_type in ("image", "texture") else "",
                texturePath=str(image).replace("\\", "/") if background_type == "texture" else "",
            )
            data = self.dump(f"background_{background_type}")
            appearance = data.get("appearance", {})
            self.check(f"background type {background_type}", appearance.get("backgroundType") == background_type, appearance)
            if background_type in ("image", "texture"):
                self.check(f"background asset configured {background_type}", appearance.get("backgroundImageConfigured") is True)
        for fill in ("cover", "contain", "tile"):
            self.set_appearance(mode="custom", backgroundType="image", backgroundImage=str(image).replace("\\", "/"), fillMode=fill)
            data = self.dump(f"fill_{fill}")
            self.check(f"fill mode {fill}", data.get("appearance", {}).get("fillMode") == fill, data.get("appearance"))

        self.set_appearance(
            mode="custom", backgroundType="solid", backgroundImage="", fillMode="cover",
            backgroundOpacity=63, panelOpacity=58, toolbarOpacity=61, timelineOpacity=72,
            subtitleOpacity=70, viewerOpacity=55, brightness=132, saturation=74,
            blurRadius=8, vignette=36, shadowStrength=48, dynamicBackground=True,
        )
        effects = self.dump("effects_custom").get("appearance", {})
        for key, expected in (("backgroundOpacity", 63), ("panelOpacity", 58), ("toolbarOpacity", 61), ("timelineOpacity", 72), ("subtitleOpacity", 70), ("viewerOpacity", 55), ("brightness", 132), ("saturation", 74), ("blurRadius", 8), ("vignette", 36), ("shadowStrength", 48), ("dynamicBackground", True)):
            self.check(f"effect persists {key}", effects.get(key) == expected, effects)

        # Per-button styles, including disabled state, are read by runtime code.
        for key, value in {
            "buttonColor/playback.toggle": "#252A30",
            "buttonHover/playback.toggle": "#34404A",
            "buttonPressed/playback.toggle": "#FF8A3D",
            "buttonDisabled/playback.toggle": "#69727C",
            "buttonText/playback.toggle": "#FFFFFF",
            "buttonIcon/playback.toggle": "#FF8A3D",
            "buttonBorder/playback.toggle": "#5A6570",
            "buttonRadius/playback.toggle": 6,
            "buttonIconSize/playback.toggle": 20,
            "buttonOpacity/playback.toggle": 88,
            "buttonBorderOpacity/playback.toggle": 70,
            "buttonShadow/playback.toggle": 25,
            "buttonShowText/playback.toggle": False,
            "buttonShowIcon/playback.toggle": True,
        }.items():
            section, name = key.split("/", 1)
            reg_set(f"{REG_PATH}\\appearance\\{section}", name, value)
        data = self.dump("button_style")
        targets = [item for item in data.get("customization", {}).get("buttonStyleTargets", []) if item.get("id") == "playback.toggle"]
        self.check("button style target exists", bool(targets), targets)
        self.check("button style applied to every matching instance", bool(targets) and all(item.get("customStyleApplied") for item in targets), targets)
        self.check("button style icon size applied", bool(targets) and all(item.get("iconWidth") == 20 and item.get("iconHeight") == 20 for item in targets), targets)
        self.check("button style radius applied", bool(targets) and all("border-radius:6px" in item.get("styleSheet", "") for item in targets), targets)
        self.check("button style disabled state applied", bool(targets) and all("QToolButton:disabled" in item.get("styleSheet", "") for item in targets), targets)

        custom = [{"id": "custom.capture_annotate", "name": "截图并批注", "icon": "camera-plus", "group": "自定义按键", "commands": ["codex.captureFrame", "annotation.create"]}]
        reg_set(f"{REG_PATH}\\toolbar", "customButtons", json.dumps(custom, ensure_ascii=False, separators=(",", ":")))
        data = self.dump("custom_button")
        buttons = data.get("customization", {}).get("customButtons", [])
        self.check("custom button is created from persisted config", any(item.get("id") == "custom.capture_annotate" for item in buttons), buttons)
        invalid = [{"id": "custom.invalid", "name": "invalid", "commands": ["does.not.exist"]}]
        reg_set(f"{REG_PATH}\\toolbar", "customButtons", json.dumps(invalid, separators=(",", ":")))
        data = self.dump("custom_button_invalid")
        self.check("unknown custom command is rejected", not any(item.get("id") == "custom.invalid" for item in data.get("customization", {}).get("customButtons", [])))

        # Shortcut persistence, explicit clear, conflict detection contract and restore.
        reg_set(f"{REG_PATH}\\shortcuts", "playback.toggle", "Ctrl+Alt+P")
        reg_set(f"{REG_PATH}\\shortcuts", "playback.nextFrame", "")
        data = self.dump("shortcuts_override")
        commands = {item.get("id"): item for item in data.get("customization", {}).get("commands", [])}
        self.check("shortcut override persists", commands.get("playback.toggle", {}).get("shortcut") == "Ctrl+Alt+P", commands.get("playback.toggle"))
        self.check("shortcut clear persists", commands.get("playback.nextFrame", {}).get("shortcut") == "", commands.get("playback.nextFrame"))

        APPDATA.mkdir(parents=True, exist_ok=True)
        (APPDATA / "mouse_bindings.json").write_text(json.dumps({"playback.nextFrame": "Mouse4"}, indent=2), encoding="utf-8")
        (APPDATA / "gamepad_bindings.json").write_text(json.dumps({"playback.toggle": "A"}, indent=2), encoding="utf-8")
        data = self.dump("input_bindings")
        customization = data.get("customization", {})
        self.check("mouse binding is loaded", customization.get("mouseBindings", {}).get("playback.nextFrame") == "Mouse4", customization)
        self.check("gamepad binding is loaded", customization.get("gamepadBindings", {}).get("playback.toggle") == "A", customization)

        self.workspace_dir.mkdir(parents=True, exist_ok=True)
        workspace = self.workspace_dir / "Acceptance Layout.json"
        workspace.write_text(json.dumps({"version": 2, "name": "Acceptance Layout", "panels": {"playlist": {"visible": True}, "review": {"visible": True}}, "splitterSizes": [0, 200, 1200, 220]}, indent=2), encoding="utf-8")
        reg_set(f"{REG_PATH}\\workspace", "preset", "Acceptance Layout")
        data = self.dump("workspace_restore")
        sizes = data.get("customization", {}).get("splitterSizes", [])
        self.check("workspace restores constrained splitter", len(sizes) == 4 and all(isinstance(value, int) and value >= 0 for value in sizes), sizes)
        workspace.write_text("{broken", encoding="utf-8")
        workspace.with_suffix(workspace.suffix + ".bak").write_text(json.dumps({"version": 2, "name": "Acceptance Layout", "panels": {"playlist": {"visible": False}, "review": {"visible": True}}, "splitterSizes": [0, 220, 1000, 240]}, indent=2), encoding="utf-8")
        data = self.dump("workspace_corrupt_fallback")
        self.check("corrupt workspace falls back without crash", data.get("_process", {}).get("exitCode") == 0)

        # Export schema is file-level but must contain every independently persisted part.
        profile = self.output / "profile.json"
        profile.write_text(json.dumps({"version": 3, "theme": {"mode": "custom"}, "shortcuts": {}, "toolbar": {"customButtons": custom}, "buttonStyles": {}, "input": {"mouseWheel": "frames"}, "mouseBindings": {"playback.nextFrame": "Mouse4"}, "gamepadBindings": {"playback.toggle": "A"}, "workspace": {"version": 2, "name": "Acceptance Layout", "panels": {}, "splitterSizes": [0, 200, 1000, 220]}, "workspacePreset": "Acceptance Layout"}, ensure_ascii=False, indent=2), encoding="utf-8")
        package = self.output / "CGPlay_Workspace_Profile.zip"
        with zipfile.ZipFile(package, "w", zipfile.ZIP_DEFLATED) as archive:
            for name in ("theme.json", "shortcuts.json", "toolbar.json", "button_styles.json", "input.json", "mouse_bindings.json", "gamepad_bindings.json", "workspace.json", "metadata.json"):
                archive.writestr(name, "{}")
        with zipfile.ZipFile(package) as archive:
            names = set(archive.namelist())
        required = {"theme.json", "shortcuts.json", "toolbar.json", "button_styles.json", "input.json", "mouse_bindings.json", "gamepad_bindings.json", "workspace.json", "metadata.json"}
        self.check("profile package contains all v3 parts", required <= names, sorted(names))

        self.capture("visual_1200", 1200, 760)
        self.capture("visual_1500", 1500, 843)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=Path, default=DEFAULT_EXE)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    output = args.output or ROOT / "tests" / "artifacts" / f"customization_acceptance_{datetime.now().strftime('%Y%m%d_%H%M%S')}"
    if not args.exe.exists():
        print(f"missing executable: {args.exe}", file=sys.stderr)
        return 2
    return Acceptance(args.exe, output).run()


if __name__ == "__main__":
    raise SystemExit(main())
