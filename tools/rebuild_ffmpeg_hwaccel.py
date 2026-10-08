#!/usr/bin/env python3
"""
CGPlay FFmpeg Hardware Acceleration Rebuild Helper

This script rebuilds FFmpeg with hardware decode support (NVDEC/D3D11VA/DXVA2).
It uses the system-installed MSYS2 to compile FFmpeg, then copies the resulting
DLLs and import libraries to the tlRender install directory.

Usage:
    python tools/rebuild_ffmpeg_hwaccel.py [--msys PATH_TO_MSYS2]

Requirements:
    - MSYS2 with gcc toolchain (pacman -S mingw-w64-x86_64-gcc make)
    - Visual Studio 2022 Build Tools (for .lib generation)
    - tlRender source at C:\\Users\\1\\Desktop\\CGV\\tlRender-main

The script will:
    1. Download FFmpeg 8.1 source
    2. Configure with hwaccel flags (--enable-d3d11va --enable-dxva2 --enable-nvdec)
    3. Build via MSYS2/MinGW
    4. Generate MSVC .lib import libraries from the .dll.a files
    5. Install to tlRender/install-Release
    6. Trigger CGPlay rebuild
"""

import subprocess
import os
import sys
import shutil
import glob
from pathlib import Path

# Paths
TLRENDER_DIR = Path(r"C:\Users\1\Desktop\CGV\tlRender-main")
CGPLAY_DIR = Path(r"C:\Users\1\Desktop\RVLite")
INSTALL_DIR = TLRENDER_DIR / "install-Release"

# FFmpeg version to build
FFMPEG_VERSION = "8.1"
FFMPEG_URL = f"https://ffmpeg.org/releases/ffmpeg-{FFMPEG_VERSION}.tar.xz"

# Default MSYS2 locations to search
MSYS2_CANDIDATES = [
    Path(r"C:\msys64"),
    Path(r"C:\msys2"),
    Path(r"C:\tools\msys64"),
]


def find_msys2():
    """Find MSYS2 installation."""
    for path in MSYS2_CANDIDATES:
        bash = path / "usr" / "bin" / "bash.exe"
        if bash.exists():
            return path
    # Check PATH
    result = subprocess.run(["where", "bash"], capture_output=True, text=True)
    if result.returncode == 0:
        bash_path = Path(result.stdout.strip().split("\n")[0])
        return bash_path.parent.parent.parent  # usr/bin/bash.exe -> msys64 root
    return None


def generate_lib(dll_path: Path, visual_studio_env: dict):
    """Generate MSVC .lib from MinGW .dll using dumpbin + lib."""
    dll = dll_path
    def_file = dll.with_suffix(".def")
    lib_file = dll.with_name(dll.stem + ".lib")

    # Step 1: dumpbin /exports
    result = subprocess.run(
        ["dumpbin", "/exports", str(dll)],
        capture_output=True, text=True, env=visual_studio_env,
    )
    if result.returncode != 0:
        print(f"  WARN: dumpbin failed for {dll.name}")
        return False

    # Step 2: Parse exports and create .def
    with open(def_file, "w") as f:
        f.write("EXPORTS\n")
        for line in result.stdout.split("\n"):
            # dumpbin output format: "    ordinal hint RVA      name"
            parts = line.strip().split()
            if (
                len(parts) >= 4
                and parts[0].isdigit()
                and not parts[-1].startswith("_")
            ):
                f.write(f"    {parts[-1]}\n")

    # Step 3: lib /def:xxx.def /machine:x64 /out:xxx.lib
    result = subprocess.run(
        ["lib", f"/def:{def_file}", "/machine:x64", f"/out:{lib_file}"],
        capture_output=True, text=True, env=visual_studio_env,
    )
    if result.returncode != 0:
        print(f"  ERROR: lib failed for {dll.name}: {result.stderr[:200]}")
        return False

    # Cleanup .def file
    def_file.unlink(missing_ok=True)
    return True


def find_vs_env():
    """Find Visual Studio environment for dumpbin/lib."""
    vswhere = (
        r"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
    )
    if not os.path.exists(vswhere):
        # Try BuildTools path
        vswhere = (
            r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
            r"\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
        )
        # Fall back to just using PATH
        return os.environ.copy()

    result = subprocess.run(
        [vswhere, "-latest", "-property", "installationPath"],
        capture_output=True, text=True,
    )
    if result.returncode != 0:
        return os.environ.copy()

    vs_path = result.stdout.strip()
    vcvars = Path(vs_path) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
    if not vcvars.exists():
        return os.environ.copy()

    # Run vcvars and capture environment
    result = subprocess.run(
        f'call "{vcvars}" >nul && set',
        shell=True, capture_output=True, text=True,
    )
    env = os.environ.copy()
    for line in result.stdout.split("\n"):
        if "=" in line:
            k, v = line.split("=", 1)
            env[k] = v
    return env


def main():
    import argparse
    parser = argparse.ArgumentParser(description="Rebuild FFmpeg with HW decode")
    parser.add_argument(
        "--msys", type=Path, help="Path to MSYS2 installation"
    )
    parser.add_argument(
        "--skip-build", action="store_true", help="Skip FFmpeg build, only generate .lib"
    )
    parser.add_argument(
        "--dry-run", action="store_true", help="Show what would be done"
    )
    args = parser.parse_args()

    print("=" * 60)
    print("CGPlay FFmpeg Hardware Acceleration Rebuild")
    print("=" * 60)

    # Find MSYS2
    msys_path = args.msys or find_msys2()
    if not msys_path:
        print("ERROR: MSYS2 not found.")
        print("Install MSYS2 from https://www.msys2.org/")
        print("Then run: pacman -S mingw-w64-x86_64-gcc make")
        print("Or specify path: python rebuild_ffmpeg_hwaccel.py --msys C:/msys64")
        return 1

    bash = msys_path / "usr" / "bin" / "bash.exe"
    if not bash.exists():
        print(f"ERROR: bash.exe not found at {bash}")
        return 1

    print(f"MSYS2: {msys_path}")
    print(f"Bash:  {bash}")

    # Build directory
    build_dir = TLRENDER_DIR / "ffmpeg_hwaccel_build"
    build_dir.mkdir(exist_ok=True)

    # Download FFmpeg
    ffmpeg_src = build_dir / f"ffmpeg-{FFMPEG_VERSION}"
    if not ffmpeg_src.exists():
        print(f"\nDownloading FFmpeg {FFMPEG_VERSION}...")
        archive = build_dir / f"ffmpeg-{FFMPEG_VERSION}.tar.xz"
        subprocess.run(["curl", "-L", "-o", str(archive), FFMPEG_URL], check=True)
        subprocess.run(["tar", "-xf", str(archive), "-C", str(build_dir)], check=True)

    if args.dry_run:
        print("\n[DRY RUN] Would configure and build FFmpeg with:")
        print("  --enable-hwaccels --enable-d3d11va --enable-dxva2 --enable-nvdec")
        return 0

    if not args.skip_build:
        # Configure and build FFmpeg via MSYS2
        print("\nConfiguring FFmpeg...")
        configure_cmd = f"""
cd /c/Users/1/Desktop/CGV/tlRender-main/ffmpeg_hwaccel_build/ffmpeg-{FFMPEG_VERSION}
./configure \\
    --prefix=/c/Users/1/Desktop/CGV/tlRender-main/install-Release \\
    --enable-shared --disable-static \\
    --disable-programs --disable-doc --disable-avfilter --disable-devices \\
    --enable-hwaccels --enable-d3d11va --enable-dxva2 --enable-nvdec \\
    --disable-amf --disable-cuda-llvm --disable-cuvid --disable-d3d12va \\
    --arch=x86_64 --toolchain=msvc
"""
        bash_script = build_dir / "configure.sh"
        with open(bash_script, "w") as f:
            f.write(configure_cmd)

        subprocess.run(
            [str(bash), "-l", str(bash_script)],
            check=True
        )

        print("\nBuilding FFmpeg...")
        build_cmd = f"cd /c/Users/1/Desktop/CGV/tlRender-main/ffmpeg_hwaccel_build/ffmpeg-{FFMPEG_VERSION} && make -j$(nproc)"
        make_script = build_dir / "make.sh"
        with open(make_script, "w") as f:
            f.write(build_cmd)

        subprocess.run([str(bash), "-l", str(make_script)], check=True)

        print("\nInstalling...")
        install_cmd = f"cd /c/Users/1/Desktop/CGV/tlRender-main/ffmpeg_hwaccel_build/ffmpeg-{FFMPEG_VERSION} && make install"
        install_script = build_dir / "install.sh"
        with open(install_script, "w") as f:
            f.write(install_cmd)

        subprocess.run([str(bash), "-l", str(install_script)], check=True)

    # Generate .lib files from .dll.a
    print("\nGenerating MSVC import libraries...")
    vs_env = find_vs_env()

    ffmpeg_bin = INSTALL_DIR / "bin"
    dlls = list(ffmpeg_bin.glob("av*.dll")) + list(ffmpeg_bin.glob("sw*.dll"))

    for dll in dlls:
        print(f"  {dll.name} -> {dll.stem}.lib")
        if not generate_lib(dll, vs_env):
            return 1

    # Copy to tlRender install
    lib_dest = INSTALL_DIR / "lib"
    lib_dest.mkdir(exist_ok=True)
    for lib_file in ffmpeg_bin.glob("*.lib"):
        shutil.copy(lib_file, lib_dest / lib_file.name)

    print("\nFFmpeg hardware decode rebuild complete!")

    # Rebuild CGPlay
    print("\nRebuilding CGPlay with hardware decode FFmpeg...")
    os.chdir(CGPLAY_DIR)

    # Regenerate CGPlay CMake cache and build
    subprocess.run([
        "cmake", "-S", str(CGPLAY_DIR), "-B", "build_win_full",
        "-G", "Visual Studio 17 2022", "-A", "x64",
        "-DCMAKE_BUILD_TYPE=Release",
        f"-DQt6_DIR=C:/Qt/6.5.3/msvc2019_64/lib/cmake/Qt6",
        f"-DtlRender_DIR={INSTALL_DIR}/lib/cmake/tlRender",
        "-DCGPLAY_USE_TLRENDER=ON",
        f"-DCMAKE_PREFIX_PATH={INSTALL_DIR}",
    ], check=True)

    subprocess.run([
        "cmake", "--build", "build_win_full", "--config", "Release",
        "--target", "CGPlay",
    ], check=True)

    print("\n" + "=" * 60)
    print("DONE! CGPlay now has hardware decode FFmpeg.")
    print(f"Exe: {CGPLAY_DIR}/build_win_full/bin/Release/CGPlay.exe")
    print("=" * 60)
    return 0


if __name__ == "__main__":
    sys.exit(main())
