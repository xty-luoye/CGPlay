"""Small isolated Qt build runner shared by targeted review regressions."""
import json
import os
from pathlib import Path
import subprocess

from tests.native_probe_runtime import deploy_runtime
from tests.test_review_metadata_repairs import COMMON, QT, ROOT


def build_and_run(out, main, sources=(), extra_cmake='', arguments=(), timeout=45):
    out = Path(out)
    source = out / 'source'; source.mkdir(parents=True, exist_ok=True)
    build = out / 'build'
    (source / 'main.cpp').write_text(COMMON + main, encoding='utf-8')
    cmake = '''cmake_minimum_required(VERSION 3.21)
project(ReviewRegression LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network Concurrent OpenGLWidgets)
add_executable(probe main.cpp
'''
    cmake += '\n'.join('"' + (ROOT / path).as_posix() + '"' for path in sources) + ')\n'
    cmake += 'target_include_directories(probe PRIVATE ' + ' '.join('"' + (ROOT / path).as_posix() + '"' for path in ('src', 'src/common', 'src/core', 'src/services', 'src/features', 'src/ui')) + ')\n'
    cmake += 'target_compile_options(probe PRIVATE /utf-8 /Zc:__cplusplus /permissive-)\n'
    cmake += 'target_link_libraries(probe PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Concurrent Qt6::OpenGLWidgets user32)\n' + extra_cmake
    (source / 'CMakeLists.txt').write_text(cmake, encoding='utf-8')
    no_window = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
    for name, command in [('configure', ['cmake', '-S', str(source), '-B', str(build), '-G', 'Visual Studio 17 2022', '-A', 'x64', f'-DCMAKE_PREFIX_PATH={QT}']), ('build', ['cmake', '--build', str(build), '--config', 'Release', '--parallel', '4'])]:
        result = subprocess.run(command, capture_output=True, creationflags=no_window, timeout=180)
        (out / (name + '.log')).write_bytes(result.stdout + result.stderr)
        if result.returncode:
            raise AssertionError((result.stdout + result.stderr).decode(errors='replace')[-7000:])
    exe = build / 'Release/probe.exe'
    deploy_runtime(exe, QT, (ROOT / 'build_win_full/bin/Release',))
    fixture = out / 'fixture'; fixture.mkdir(exist_ok=True)
    env = dict(os.environ, QT_QPA_PLATFORM='offscreen', QT_PLUGIN_PATH=str(QT / 'plugins'))
    result = subprocess.run([str(exe), str(fixture), *map(str, arguments)], capture_output=True, env=env,
                            creationflags=no_window, timeout=timeout)
    (out / 'runtime.log').write_bytes(result.stdout + result.stderr)
    if result.returncode:
        raise AssertionError((result.stdout + result.stderr).decode(errors='replace')[-7000:])
    report = json.loads(result.stdout.decode('utf-8').splitlines()[-1])
    (out / 'result.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    if not report['passed'] or report['visiblePlatformWindows'] != 0:
        raise AssertionError(report)
    return report
