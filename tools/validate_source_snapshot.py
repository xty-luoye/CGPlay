"""Check source-distribution completeness without building or launching CGPlay."""
from __future__ import annotations

import argparse
import ast
import json
from pathlib import Path
import re
import subprocess
import xml.etree.ElementTree as ET


def source_files(root: Path) -> list[Path]:
    # Only use Git's inventory when this directory is itself the checkout root.
    try:
        top = subprocess.check_output(
            ['git', '-C', str(root), 'rev-parse', '--show-toplevel'],
            stderr=subprocess.DEVNULL, text=True).strip()
        if Path(top).resolve() == root.resolve():
            names = subprocess.check_output(
                ['git', '-C', str(root), 'ls-files', '-z', '--cached', '--others',
                 '--exclude-standard']).decode('utf-8').split('\0')
            return sorted({root / name for name in names if name and (root / name).is_file()})
    except (OSError, subprocess.CalledProcessError):
        pass
    ignored = {'.git', '__pycache__', '.pytest_cache', '.venv', 'build_win_full', 'build_win'}
    return sorted(path for path in root.rglob('*') if path.is_file()
                  and not ignored.intersection(path.relative_to(root).parts)
                  and path.relative_to(root).parts[:2] != ('tests', 'artifacts'))


def validate(root: Path) -> dict:
    root = root.resolve()
    files = source_files(root)
    errors = []
    references = 0

    def check_reference(owner: Path, name: str) -> None:
        nonlocal references
        references += 1
        target = (owner.parent / name).resolve()
        if not target.is_relative_to(root) or not target.is_file():
            errors.append(f'{owner.relative_to(root).as_posix()}: missing/local-external source {name}')

    for required in ('CMakeLists.txt', 'src/CMakeLists.txt', 'resources/resources.qrc', 'LICENSE.txt'):
        if not (root / required).is_file():
            errors.append(f'Missing required file: {required}')

    for path in files:
        rel = path.relative_to(root).as_posix()
        if (path.name in {'AGENTS.md', 'auth.json', '.env', 'CMakeUserPresets.json'}
                or path.name.startswith(('TOKEN_', 'PLAN'))
                or path.suffix.lower() in {'.pfx', '.p12', '.key'}
                or rel.startswith(('.agents/', 'docs/handoffs/', 'docs/history/', 'tools/video/'))):
            errors.append(f'Private/local file in source distribution: {rel}')
        if path.stat().st_size > 10 * 1024 * 1024:
            errors.append(f'Unexpected large source file: {rel}')
        if path.suffix == '.qrc':
            try:
                for entry in ET.parse(path).getroot().iter('file'):
                    check_reference(path, (entry.text or '').strip())
            except ET.ParseError as error:
                errors.append(f'{rel}: invalid resource XML: {error}')
        if path.suffix == '.py':
            try:
                ast.parse(path.read_text(encoding='utf-8-sig'), filename=rel)
            except (SyntaxError, UnicodeError) as error:
                errors.append(f'{rel}: invalid Python syntax: {error}')
        if rel == 'src/CMakeLists.txt':
            # Literal application sources listed one per line. Generated and
            # external dependency sources use CMake variables and are configured separately.
            for name in re.findall(r'^\s*"?([\w./-]+\.(?:cpp|h|rc|qrc))"?\s*$',
                                   path.read_text(encoding='utf-8-sig'), re.MULTILINE):
                check_reference(path, name)
    return {'passed': not errors, 'fileCount': len(files), 'localSourceReferences': references,
            'backgroundMode': True, 'launchedApplicationCount': 0, 'errors': errors}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    result = validate(args.root)
    text = json.dumps(result, indent=2, ensure_ascii=False)
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(text + '\n', encoding='utf-8')
    print(text)
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
