"""Collect an explicit public source tree and checksum-fixed dependency files.

Uses only the standard library. Never reads credentials, build trees or test
artifacts into the application snapshot. The private local map stays local.
"""
from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import xml.etree.ElementTree as ET
import zipfile

SOURCE_TREES = ("src", "cmake", "installer", "scripts", "tools", "resources", "docs", "tests")
ROOT_FILES = ("README.md", "LICENSE.txt", "CMakeLists.txt", "CMakePresets.json",
              "build_release.bat", "build_installer.bat", ".gitignore", ".gitattributes",
              "CONTRIBUTING.md", "SECURITY.md", "run_player_smoke_one_click.bat",
              "一键测试播放器功能和快捷键.bat")
BLOCKED_PARTS = {".git", "__pycache__", "artifacts", "media", "reports", "backups",
                 "node_modules", "target", "video", "rvlite_defect_triage", "handoffs", ".agents"}
BLOCKED_SUFFIXES = {".exe", ".dll", ".pyd", ".lib", ".obj", ".pdb", ".pyc", ".zip",
                    ".7z", ".mp4", ".mov", ".exr", ".wav", ".log", ".tmp"}
BLOCKED_NAMES = {"AGENTS.md", "TOKEN.md", ".env", "credentials.json", "auth.json",
                 "id_rsa", "CMakeUserPresets.json"}
PUBLIC_DOCS = {"CGPlay_Commercial_EULA.txt", "THIRD_PARTY_NOTICES.txt", "FFMPEG_SOURCE_OFFER.txt",
               "INSTALLER_BUILD.md", "OFFICIAL_BUILD.md", "QUICKLOOK_HEALTH.md", "UPDATE_MANIFEST_SECURITY.md"}
PUBLIC_DOC_PATTERNS = ("RELEASE_NOTES_*.md", "DEPENDENCY_SOURCES_*.md", "DEPENDENCY_PROVENANCE_*.json",
                       "CGPlay_使用说明_*.md", "CGPlay_使用说明_*.html", "CGPlay_User_Guide_*.md", "CGPlay_User_Guide_*.html")
PRIVATE_TEST_PATTERNS = ("test_adaptive_*", "test_context_router.py", "test_daily_consent.py",
                         "test_default_skill_composition.py", "test_evolution_loop.py",
                         "test_global_adaptive_assistant.py", "test_rvlite_defect_triage.py")


def digest(path: Path) -> str:
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def safe_relative(value: str) -> str:
    path = PurePosixPath(value)
    if not value or "\\" in value or path.is_absolute() or ".." in path.parts or ":" in value:
        raise ValueError(f"Unsafe archive path: {value}")
    return path.as_posix()


def public_source_files(root: Path) -> list[Path]:
    root = root.resolve(strict=True)
    selected: set[Path] = set()
    def blocked_directory(relative: Path) -> bool:
        parts = tuple(part.lower() for part in relative.parts)
        # This is application source, not a test-media fixture directory.
        return any(part in BLOCKED_PARTS and not
                   (index == 2 and parts[:3] == ("src", "services", "media"))
                   for index, part in enumerate(parts))
    def require_contained_regular(path: Path) -> None:
        if any(parent.is_symlink() or
               getattr(parent.stat(follow_symlinks=False), "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT
               for parent in (path, *path.parents) if parent != root):
            raise ValueError(f"Reparse source is not publishable: {path.relative_to(root)}")
        if not path.resolve().is_relative_to(root):
            raise ValueError(f"Source escapes project: {path.relative_to(root)}")
    for name in ROOT_FILES:
        path = root / name
        if path.is_file():
            require_contained_regular(path)
            selected.add(path)
    for name in SOURCE_TREES:
        tree = root / name
        if not tree.is_dir():
            continue
        candidates = []
        for directory, folders, files in os.walk(tree, followlinks=False):
            folders[:] = [folder for folder in folders if not
                          blocked_directory((Path(directory) / folder).relative_to(root))]
            candidates.extend(Path(directory) / name for name in files)
        for path in candidates:
            relative = path.relative_to(root)
            if name == "docs" and not (relative.parts[1] == "third_party_licenses" or
                    path.name in PUBLIC_DOCS or any(fnmatch.fnmatchcase(path.name, pattern) for pattern in PUBLIC_DOC_PATTERNS)):
                continue
            if name == "tests" and any(fnmatch.fnmatchcase(path.name, pattern) for pattern in PRIVATE_TEST_PATTERNS):
                continue
            if (not path.is_file() or blocked_directory(relative.parent)
                    or path.name in BLOCKED_NAMES or path.name.lower().startswith((".env", "token_handoff"))
                    or path.suffix.lower() in BLOCKED_SUFFIXES):
                continue
            # A source release must not follow junctions/symlinks into a private tree.
            require_contained_regular(path)
            selected.add(path)
    for required in ("LICENSE.txt", "CMakeLists.txt", "README.md"):
        if root / required not in selected:
            raise ValueError(f"Missing application source: {required}")
    for qrc in (p for p in selected if p.suffix == ".qrc"):
        for entry in ET.parse(qrc).getroot().iter("file"):
            reference = (qrc.parent / (entry.text or "")).resolve()
            if reference not in selected:
                raise ValueError(f"Missing public QRC resource: {qrc.relative_to(root)} -> {entry.text}")
    return sorted(selected, key=lambda p: p.relative_to(root).as_posix())


def build_bundle(application: Path, provenance: Path, local_map: Path, output: Path,
                 max_part_bytes: int = 1400 * 1024 * 1024) -> dict:
    public = json.loads(provenance.read_text(encoding="utf-8-sig"))
    mappings = json.loads(local_map.read_text(encoding="utf-8-sig"))["files"]
    version = public["version"]
    if public.get("releaseBlockers"):
        raise ValueError("Required corresponding-source gaps remain: " + json.dumps(public["releaseBlockers"]))
    text = (application / "CMakeLists.txt").read_text(encoding="utf-8-sig")
    match = re.search(r"project\s*\(\s*CGPlay\s+VERSION\s+([0-9.]+)", text)
    if not match or match[1] != version:
        raise ValueError("Public application snapshot version does not match provenance")
    if "GNU GENERAL PUBLIC LICENSE" not in (application / "LICENSE.txt").read_text(encoding="utf-8-sig"):
        raise ValueError("Application GPLv3 text is missing")
    selected = public_source_files(application)
    dependencies = []
    names = set()
    for row in public["files"]:
        name = safe_relative(row["archive"])
        if name.casefold() in names:
            raise ValueError(f"Duplicate source archive: {name}")
        names.add(name.casefold())
        path = Path(mappings[row["id"]]).resolve(strict=True)
        if not path.is_file() or digest(path) != row["sha256"]:
            raise ValueError(f"Dependency checksum mismatch: {row['id']}")
        dependencies.append((path, "dependencies/" + name))
    if output.exists():
        raise ValueError("Use a new source-bundle output; existing assets are never overwritten")
    output.parent.mkdir(parents=True, exist_ok=True)
    groups = [[(path, "application/" + path.relative_to(application).as_posix()) for path in selected]]
    current_size = sum(path.stat().st_size for path in selected)
    for path, name in dependencies:
        size = path.stat().st_size
        if size > max_part_bytes:
            raise ValueError("An individual source archive exceeds the part budget; split that archive explicitly: " + name)
        if current_size + size > max_part_bytes:
            groups.append([])
            current_size = 0
        groups[-1].append((path, name))
        current_size += size
    outputs = [output] + [output.with_name(output.stem + f"-part{index:02d}.zip") for index in range(1, len(groups))]
    index_path = output.with_suffix(".assets.json")
    for path in [*outputs, index_path]:
        if path.exists() or path.with_name(path.name + ".partial").exists() or path.with_suffix(path.suffix + ".sha256").exists():
            raise ValueError("Existing source asset or sidecar: " + str(path))
    inventory = []
    for target, group in zip(outputs, groups):
        inventory.extend({"path": name, "asset": target.name, "sha256": digest(path)} for path, name in group)
    created = []
    assets = []
    try:
        for index, (target, group) in enumerate(zip(outputs, groups)):
            partial = target.with_name(target.name + ".partial")
            created.append(partial)
            with zipfile.ZipFile(partial, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
                for path, name in group:
                    # Preserve originals without spending minutes recompressing
                    # multi-gigabyte archives which are already compressed.
                    compression = zipfile.ZIP_STORED if path.suffix.lower() in {".zip", ".xz", ".gz", ".bz2", ".whl", ".zst"} else zipfile.ZIP_DEFLATED
                    archive.write(path, name, compress_type=compression)
                if index == 0:
                    archive.writestr("source-inventory.json", json.dumps({"version": version, "files": inventory}, indent=2))
                    archive.writestr("dependency-provenance.json", json.dumps(public, indent=2))
            if partial.stat().st_size >= 2 * 1024 ** 3:
                raise ValueError("Source asset exceeds GitHub's 2GiB limit: " + target.name)
            partial.replace(target)
            created.append(target)
            sha = digest(target)
            sidecar = target.with_suffix(target.suffix + ".sha256")
            with sidecar.open("x", encoding="ascii") as stream:
                created.append(sidecar)
                stream.write(f"{sha}  {target.name}\n")
            assets.append({"name": target.name, "sha256": sha, "bytes": target.stat().st_size})
        with index_path.open("x", encoding="utf-8") as stream:
            created.append(index_path)
            json.dump({"schema": 1, "version": version, "primary": output.name, "assets": assets}, stream, indent=2)
    except BaseException:
        for path in created:
            path.unlink(missing_ok=True)
        raise
    return {"passed": True, "backgroundMode": True, "platformVisibleTopLevelWindows": 0,
            "version": version, "applicationFiles": len(selected), "dependencyFiles": len(dependencies),
            "sha256": assets[0]["sha256"], "output": str(output), "assets": assets, "assetIndex": str(index_path)}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--application", type=Path, required=True)
    parser.add_argument("--provenance", type=Path, required=True)
    parser.add_argument("--local-map", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(build_bundle(args.application.resolve(), args.provenance, args.local_map, args.output), indent=2))


if __name__ == "__main__":
    main()
