"""Link targeted background probes against the current full product objects."""
import json
import os
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET
from tests.native_probe_runtime import deploy_runtime
from tests.test_review_metadata_repairs import ROOT,COMMON

def build_object_probe(out,main,arguments=(),timeout=60):
    out=Path(out);out.mkdir(parents=True,exist_ok=True)
    project=ROOT/'build_win_full/src/CGPlay.vcxproj'
    ns={'m':'http://schemas.microsoft.com/developer/msbuild/2003'}
    tree=ET.parse(project)
    group=next(x for x in tree.findall('m:ItemDefinitionGroup',ns) if 'Release|x64' in x.attrib.get('Condition',''))
    includes=group.find('m:ClCompile/m:AdditionalIncludeDirectories',ns).text.split(';')
    defs=group.find('m:ClCompile/m:PreprocessorDefinitions',ns).text.split(';')
    libs=group.find('m:Link/m:AdditionalDependencies',ns).text.split(';')
    objects=sorted(p for p in (project.parent/'CGPlay.dir/Release').glob('*.obj') if p.name!='main.obj')
    quote=lambda v:'[=['+str(v).replace('\\','/')+']=]'
    lines=['cmake_minimum_required(VERSION 3.21)','project(CurrentProductProbe LANGUAGES CXX)','set(CMAKE_CXX_STANDARD 20)','add_executable(probe main.cpp)']
    lines+=['target_sources(probe PRIVATE '+quote(p)+')' for p in objects]
    lines+=['target_include_directories(probe PRIVATE '+quote(p)+')' for p in includes if p and not p.startswith('%')]
    lines+=['target_compile_definitions(probe PRIVATE '+quote(p)+')' for p in defs if p and not p.startswith(('%','CMAKE_INTDIR'))]
    lines+=['target_compile_options(probe PRIVATE /utf-8 /Zc:__cplusplus /permissive-)']
    for lib in libs:
        if not lib or lib.startswith('%'):continue
        if lib.startswith('..'):lib=(project.parent/lib).resolve()
        lines+=['target_link_libraries(probe PRIVATE '+quote(lib)+')']
    (out/'main.cpp').write_text(COMMON+main,encoding='utf-8')
    (out/'CMakeLists.txt').write_text('\n'.join(lines),encoding='utf-8')
    no_window=subprocess.CREATE_NO_WINDOW
    for name,args in [('configure',['cmake','-S',str(out),'-B',str(out/'build'),'-G','Visual Studio 17 2022','-A','x64']),('build',['cmake','--build',str(out/'build'),'--config','Release','-j','4'])]:
        p=subprocess.run(args,capture_output=True,creationflags=no_window,timeout=180);(out/(name+'.log')).write_bytes(p.stdout+p.stderr)
        if p.returncode:raise AssertionError((p.stdout+p.stderr).decode(errors='replace')[-6000:])
    qt=Path('E:/rjkf/CGPlay-deps/QtClean/6.5.3/msvc2019_64')
    exe=out/'build/Release/probe.exe';deploy_runtime(exe,qt,[ROOT/'build_win_full/bin/Release'])
    fixture=out/'fixture';fixture.mkdir(exist_ok=True)
    env=dict(os.environ,QT_QPA_PLATFORM='windows',QT_PLUGIN_PATH=str(qt/'plugins'),QT_IM_MODULE='none')
    p=subprocess.run([str(exe),str(fixture),*map(str,arguments)],capture_output=True,creationflags=no_window,timeout=timeout,env=env)
    (out/'runtime.log').write_bytes(p.stdout+p.stderr)
    if p.returncode:raise AssertionError((p.stdout+p.stderr).decode(errors='replace')[-6000:])
    report=json.loads(p.stdout.decode('utf-8').splitlines()[-1]);(out/'result.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
    if report.get('passed') is False or report.get('visiblePlatformWindows',report.get('visiblePlatformWindowsMax',-1))!=0:raise AssertionError(report)
    return report
