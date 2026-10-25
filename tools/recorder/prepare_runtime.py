"""Stage only pinned OBS33 helper dependencies; produce same-DLL import libs."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess

MODULES = ("obs-x264", "obs-ffmpeg", "win-capture", "win-wasapi")


def run(command):
    return subprocess.check_output(command, text=True, encoding="utf-8", errors="replace", creationflags=subprocess.CREATE_NO_WINDOW)


def import_library(dll, sdk, dumpbin, lib):
    exports = run([dumpbin, "/nologo", "/exports", str(dll)])
    symbols = []
    for line in exports.splitlines():
        match = re.match(r"\s+\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+(\S+)", line)
        if match:
            symbols.append(match.group(1))
    if not symbols:
        raise RuntimeError("No exports found: " + str(dll))
    definition = sdk / "lib" / (dll.stem + ".def")
    definition.write_text("LIBRARY " + dll.name + "\nEXPORTS\n" + "\n".join(symbols) + "\n", encoding="ascii")
    run([lib, "/nologo", "/machine:x64", "/def:" + str(definition), "/out:" + str(definition.with_suffix(".lib"))])


def write_manifest(stage):
    files = []
    for path in sorted(stage.rglob("*")):
        if path.is_file() and path.name not in ("runtime-manifest.json", "CGPlayRecorder.pdb"):
            files.append({"path": path.relative_to(stage).as_posix(), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()})
    value = {"schema": 1, "obsApi": 33 << 24, "obsVersion": "33.0.0",
             "sourceCommit": "72e86655715d9a25d69ce446aa4dd44ba58e10d2",
             "capturePatch": 1, "wasapiPatch": 1, "files": files,
             "modules": [{"id": name, "path": f"core/{name}/{name}.dll", "data": f"core/{name}/data"} for name in MODULES]}
    (stage / "runtime-manifest.json").write_text(json.dumps(value, indent=2), encoding="utf-8")


def prepare(root, dumpbin, lib):
    root = root.resolve()
    manifest = json.loads((root / "dependency_manifest.json").read_text(encoding="utf-8"))
    if manifest.get("commit") != "72e86655715d9a25d69ce446aa4dd44ba58e10d2" or not manifest.get("passed"):
        raise RuntimeError("OBS source/runtime provenance not verified")
    stage, sdk = root / "stage", root / "sdk"
    stage.mkdir(exist_ok=True)
    (sdk / "lib").mkdir(parents=True, exist_ok=True)
    (sdk / "include").mkdir(exist_ok=True)
    (sdk / "include/obsconfig.h").write_text('#pragma once\n#define OBS_DATA_PATH "data"\n#define OBS_PLUGIN_PATH "core"\n#define OBS_PLUGIN_DESTINATION "core"\n#define OBS_INSTALL_PREFIX "."\n#define OBS_RELEASE_CANDIDATE 0\n#define OBS_BETA 0\n', encoding="ascii")
    runtime = root / "official-runtime"
    binary = runtime / "bin/64bit"
    for name in ("obs.dll", "w32-pthreads.dll"):
        import_library(binary / name, sdk, dumpbin, lib)
    # Follow actual import tables, and add the two explicitly loaded backends.
    pending = [binary / name for name in ("obs.dll", "libobs-d3d11.dll", "libobs-winrt.dll", "obs-ffmpeg-mux.exe")]
    pending.extend(runtime / f"core/{name}/{name}.dll" for name in MODULES)
    visited = set()
    while pending:
        file = pending.pop()
        if file in visited:
            continue
        visited.add(file)
        if file.parent == binary:
            shutil.copy2(file, stage / file.name)
        for name in re.findall(r"^\s+([\w.-]+\.dll)\s*$", run([dumpbin, "/nologo", "/dependents", str(file)]), re.MULTILINE | re.IGNORECASE):
            match = next((candidate for candidate in binary.iterdir() if candidate.name.casefold() == name.casefold()), None)
            if match:
                if match.name.casefold().startswith("qt") or match.name in ("obs-frontend-api.dll", "obs-scripting.dll"):
                    raise RuntimeError("Unexpected frontend dependency: " + name)
                pending.append(match)
            elif not (Path(os.environ["SystemRoot"]) / "System32" / name).exists() and not name.lower().startswith(("api-ms-", "ext-ms-")):
                raise RuntimeError("Unresolved private dependency: " + name)
    for name in MODULES:
        destination = stage / "core" / name
        destination.mkdir(parents=True, exist_ok=True)
        # Patched capture DLLs are produced by this project's native build.
        if name not in ("win-capture", "win-wasapi"):
            shutil.copy2(runtime / f"core/{name}/{name}.dll", destination / (name + ".dll"))
        data = runtime / f"core/{name}/data"
        if data.exists():
            shutil.copytree(data, destination / "data", dirs_exist_ok=True,
                            ignore=lambda folder, entries: [name for name in entries if name.endswith((".exe", ".dll", ".pdb", ".json")) and name != "package.json"])
    shutil.copytree(runtime / "data/libobs", stage / "data/libobs", dirs_exist_ok=True)
    licenses = stage / "licenses"
    licenses.mkdir(exist_ok=True)
    for name in ("COPYING", "AUTHORS", "README.md"):
        file = root / "upstream" / name
        if file.is_file():
            shutil.copy2(file, licenses / ("OBS-" + name))
    (sdk / "source_dll_sha256.json").write_text(json.dumps({file.name: hashlib.sha256(file.read_bytes()).hexdigest() for file in visited}, indent=2), encoding="utf-8")
    return stage


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--dumpbin", default="dumpbin")
    parser.add_argument("--lib", default="lib")
    parser.add_argument("--finalize", action="store_true")
    args = parser.parse_args()
    if args.finalize:
        write_manifest(args.root / "stage")
    else:
        prepare(args.root, args.dumpbin, args.lib)
