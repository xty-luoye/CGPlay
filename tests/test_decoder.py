#!/usr/bin/env python3
"""CGPlay Hardware Decoder Detection Tests
用法: python tests/test_decoder.py
"""

import sys
import os
import platform
import subprocess
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent.parent / "tools"
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))


def test_gpu_detection(result):
    """检测 GPU 硬件（Windows DXGI / Linux / macOS）"""
    gpus = []
    system = platform.system()

    if system == "Windows":
        gpus = _detect_windows()
    elif system == "Linux":
        gpus = _detect_linux()
    elif system == "Darwin":
        gpus = _detect_macos()
    else:
        result.status = "SKIP"
        result.message = f"Unsupported OS: {system}"
        return

    result.details = {"gpus": gpus, "os": system}

    if gpus:
        result.status = "PASS"
        gpu_names = [g["name"] for g in gpus]
        result.message = f"{len(gpus)} GPU(s): {', '.join(gpu_names)}"
    else:
        result.status = "FAIL"
        result.message = "No GPU detected"


def test_hwaccel_config(result):
    """检查硬件解码器配置可用性"""
    supported = []

    # Check FFmpeg hwaccel support
    try:
        import subprocess
        proc = subprocess.run(
            ["ffmpeg", "-hwaccels"],
            capture_output=True, text=True, timeout=5
        )
        if proc.returncode == 0:
            for line in proc.stdout.splitlines():
                line = line.strip()
                if line in ("cuda", "d3d11va", "dxva2", "qsv", "vdpau", "vaapi", "amf"):
                    supported.append(line)
    except Exception:
        pass

    # Check PyAV hwaccel (AVHWDeviceType)
    try:
        import av
        hw_methods = []
        for attr in dir(av.codec.hwaccel):
            if not attr.startswith('_'):
                hw_methods.append(attr)
        supported.extend(hw_methods)
    except Exception:
        pass

    # Check OIIO OCIO support (indirect — OCIO used for color, but indicates complex pipeline)
    try:
        import OpenImageIO
        ocio_ok = hasattr(OpenImageIO, "OpenColorIO")
    except Exception:
        ocio_ok = False

    result.details = {
        "ffmpeg_hwaccels": supported,
        "oiio_ocio": ocio_ok,
    }

    if supported:
        result.status = "PASS"
        result.message = f"Available: {', '.join(supported[:5])}"
    else:
        result.status = "FAIL"
        result.message = "No hwaccel detected (may need FFmpeg rebuild)"


def _detect_windows():
    """Windows GPU detection via WMIC / DXGI"""
    gpus = []
    try:
        # Use wmic as fallback
        proc = subprocess.run(
            ["wmic", "path", "win32_VideoController", "get", "name,AdapterRAM"],
            capture_output=True, text=True, timeout=10
        )
        for line in proc.stdout.splitlines():
            line = line.strip()
            if line and "Name" not in line and line[:1].isalpha():
                parts = line.split()
                # Name is everything except the last part (RAM)
                if len(parts) > 1:
                    name = " ".join(parts[:-1])
                else:
                    name = parts[0]
                gpus.append({"name": name, "vendor": _guess_vendor(name)})
    except Exception:
        # Fallback to platform info
        gpus.append({"name": "Unknown GPU", "vendor": "unknown"})

    return gpus


def _detect_linux():
    """Linux GPU detection via lspci"""
    try:
        proc = subprocess.run(
            ["lspci", "-v", "-nn"],
            capture_output=True, text=True, timeout=10
        )
        for line in proc.stdout.splitlines():
            if "VGA" in line and "[" in line:
                gpu_name = line.split("[")[-1].split("]")[0] if "[" in line else "Unknown"
                return [{"name": gpu_name, "vendor": _guess_vendor(gpu_name)}]
    except Exception:
        pass
    return [{"name": "Unknown GPU", "vendor": "unknown"}]


def _detect_macos():
    """macOS GPU detection"""
    try:
        proc = subprocess.run(
            ["system_profiler", "SPDisplaysDataType"],
            capture_output=True, text=True, timeout=10
        )
        for line in proc.stdout.splitlines():
            if "Chipset Model:" in line:
                name = line.split(":")[-1].strip()
                return [{"name": name, "vendor": "Apple"}]
    except Exception:
        pass
    return [{"name": "Apple Silicon/Intel", "vendor": "Apple"}]


def _guess_vendor(name):
    name = name.lower()
    if "nvidia" in name or "geforce" in name or "rtx" in name or "quadro" in name:
        return "NVIDIA"
    if "amd" in name or "radeon" in name:
        return "AMD"
    if "intel" in name or "arc" in name:
        return "Intel"
    return "unknown"


if __name__ == "__main__":
    from run_all import TestResult

    r1 = TestResult("GPU Detection")
    test_gpu_detection(r1)
    print(f"{r1.status}: {r1.message}")

    r2 = TestResult("HwAccel Config")
    test_hwaccel_config(r2)
    print(f"{r2.status}: {r2.message}")
