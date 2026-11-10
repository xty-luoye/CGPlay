"""Isolated native NSIS ownership deletion; never run the real installer."""
import json
import os
from pathlib import Path
import re
import subprocess

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'tests/artifacts/fix_review_20261101/nsis_ownership'
NSIS=Path('C:/Program Files (x86)/NSIS/makensis.exe')


def run(command,name,ok=True):
    result=subprocess.run([str(x) for x in command],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,
        creationflags=subprocess.CREATE_NO_WINDOW,timeout=90)
    (OUT/(name+'.log')).write_bytes(result.stdout)
    if ok and result.returncode:raise RuntimeError(result.stdout.decode(errors='replace'))
    return result


def safe(path):
    assert path.resolve().is_relative_to(OUT.resolve()) and path.resolve()!=OUT.resolve(),path
    return path


def generate(source,include,name,ok=True):
    return run(['powershell.exe','-NoProfile','-ExecutionPolicy','Bypass','-File',ROOT/'tools/generate_nsis_uninstall.ps1',
        '-SourceDirectory',source,'-IncludePath',include],name,ok)


def main():
    OUT.mkdir(parents=True,exist_ok=True)
    # Re-running uses a fresh controlled directory; no recursive cleanup.
    import uuid
    work=OUT/uuid.uuid4().hex[:8];work.mkdir()
    payload=work/'payload';payload.mkdir()
    files={'CGPlay.exe':b'installed exe','runtime/nested/module.bin':b'installed runtime',
        'tools/owned.txt':b'installed tools','cash$foo.txt':b'installed literal dollar'}
    for name,data in files.items():
        path=payload/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(data)
    include=work/'owned.nsh';generate(payload,include,'generate')
    generated=include.read_text(encoding='utf-8-sig')
    assert 'RMDir /r' not in generated and 'cash$$foo.txt' in generated
    source=(ROOT/'installer/CGPlay_Installer.nsi').read_text(encoding='utf-8-sig')
    start=source.index('!macro CGPlayRemoveOwnedPath ')
    end=source.index('!insertmacro CGPlayRemoveOwnedPath un.RemoveOwnedDirectory RMDir',start)
    end+=len('!insertmacro CGPlayRemoveOwnedPath un.RemoveOwnedDirectory RMDir')
    helper=source[start:end].replace('un.','fixture.')
    instructions=generated.replace('un.','fixture.')
    reports=[]
    for junction in [False,True]:
        fixture=safe(work/('junction-install' if junction else 'normal-install'));fixture.mkdir()
        for name,data in files.items():
            if junction and name.startswith('runtime/'):continue
            path=fixture/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(data)
        canaries={'user_notes.txt':b'user note','nested_project/source.txt':b'user project',
            'tools/user.txt':b'user tool file'}
        for name,data in canaries.items():
            path=fixture/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(data)
        external=safe(work/'external')
        if junction:
            external.mkdir();(external/'nested').mkdir();(external/'nested/module.bin').write_bytes(b'external data')
            (external/'nested/user.txt').write_bytes(b'external canary')
            # Junction source and target are both verified artifact descendants.
            link=safe(fixture/'runtime')
            quote=lambda value:"'"+str(value).replace("'","''")+"'"
            run(['powershell.exe','-NoProfile','-Command',f'New-Item -ItemType Junction -Path {quote(link)} -Target {quote(external)} | Out-Null'],'junction-create')
            safe(link)
        else:
            (fixture/'runtime/nested/user.txt').write_bytes(b'user runtime file')
        script=work/('probe-junction.nsi' if junction else 'probe-normal.nsi')
        exe=script.with_suffix('.exe')
        script.write_text('Unicode True\nName "CGPlay isolated ownership probe"\nOutFile "'+str(exe)+'"\nSilentInstall silent\nRequestExecutionLevel user\n!include "FileFunc.nsh"\n!insertmacro GetParent\n'+helper+'\n'+instructions+'\nSection\nStrCpy $INSTDIR "'+str(fixture)+'"\n!insertmacro CGPlayUninstallOwnedFiles\nPush "$INSTDIR"\nCall fixture.RemoveOwnedDirectory\nSectionEnd\n',encoding='utf-8')
        run([NSIS,'/V2',script],'compile-'+str(junction));safe(fixture)
        run([exe],'execute-'+str(junction))
        for name,data in canaries.items():assert (fixture/name).read_bytes()==data
        for name in files:
            if not (junction and name.startswith('runtime/')):assert not (fixture/name).exists(),name
        if junction:
            assert (external/'nested/module.bin').read_bytes()==b'external data'
            assert (external/'nested/user.txt').read_bytes()==b'external canary'
            rejected=work/'reject.nsh';failure=generate(fixture,rejected,'reject-reparse',False)
            assert failure.returncode!=0
        else:assert (fixture/'runtime/nested/user.txt').read_bytes()==b'user runtime file'
        reports.append({'junction':junction,'knownFilesRemoved':True,'unknownFilesPreserved':True,'externalTargetPreserved':True})
    # Compile the complete production script with a tiny disposable payload.
    # Never execute it: its process/registry association hooks are out of scope.
    run([NSIS,'/V2','/DPACKAGE_MODE=lite',f'/DPACKAGE_SOURCE_DIR={payload}',f'/DINSTALLER_OUTPUT_DIR={work}',
        f'/DUNINSTALL_FILES_INCLUDE={include}',ROOT/'installer/CGPlay_Installer.nsi'],'complete-script-compile')
    assert 'RMDir /r "$INSTDIR"' not in source
    package=(ROOT/'tools/package_installer.ps1').read_text(encoding='utf-8-sig')
    assert 'generate_nsis_uninstall.ps1' in package and '/DUNINSTALL_FILES_INCLUDE=' in package
    result={'passed':True,'backgroundMode':True,'registryTouched':False,'productionInstallerExecuted':False,
        'productionInstallerCompiled':True,'cases':reports,'fixture':str(work)}
    (OUT/'result.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');print(json.dumps(result))


if __name__=='__main__':main()
