"""Background-only release fixtures; no packaged application/helper is executed."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from prepare_release_sources import build_bundle, digest, public_source_files, safe_relative
from validate_release_sources import validate
ARTIFACT=ROOT/'tests/artifacts/migration_1_0_8_20261025/release_source_closure/tests'
ARTIFACT.mkdir(parents=True,exist_ok=True)
class ReleaseSourceTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(dir=ARTIFACT)
        self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name)
        self.app=self.root/'app';self.app.mkdir()
        self.write(self.app/'CMakeLists.txt','project(CGPlay VERSION 1.0.8)')
        self.write(self.app/'LICENSE.txt','GNU GENERAL PUBLIC LICENSE\nVersion 3')
        self.write(self.app/'README.md','Public source')
        self.write(self.app/'src/main.cpp','int main(){}')
        self.dep=self.root/'dependency.tar.gz';self.dep.write_bytes(b'original source archive')
        self.prov=self.root/'provenance.json'
        self.data={'schema':1,'version':'1.0.8','files':[{'id':'dependency','archive':'dependency.tar.gz','sha256':digest(self.dep),'sourceUrl':'https://example.org/dependency.tar.gz'}],'requiredNotices':['licenses/third_party/1.0.8/Dependency.txt']}
        self.prov.write_text(json.dumps(self.data))
        self.mapping=self.root/'local-map.json';self.mapping.write_text(json.dumps({'files':{'dependency':str(self.dep)}}))
        self.bundle=self.root/'sources.zip'
    def write(self,path,text='fixture'):
        path.parent.mkdir(parents=True,exist_ok=True);path.write_text(text,encoding='utf-8')
    def build(self):return build_bundle(self.app,self.prov,self.mapping,self.bundle)
    def ps(self,script,*args):
        command=shutil.which('pwsh') or shutil.which('powershell')
        self.assertTrue(command)
        run=subprocess.run([command,'-NoProfile','-ExecutionPolicy','Bypass','-File',str(ROOT/'tools'/script),*map(str,args)],capture_output=True,text=True,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        if run.returncode: self.fail(run.stdout+'\n'+run.stderr)
        return run.stdout
    def test_round_trip_all_inventory_digests(self):
        result=self.build();self.assertEqual(result['dependencyFiles'],1)
        self.assertTrue(validate(self.prov,bundle=self.bundle)['passed'])
    def test_private_credentials_and_artifacts_excluded(self):
        for name in ['tools/.env','tools/credentials.json','tools/auth.json','tests/artifacts/private.json','tests/media/private.mp4','docs/handoffs/TOKEN_HANDOFF.md']:
            self.write(self.app/name,'private')
        self.build()
        with zipfile.ZipFile(self.bundle) as archive:self.assertFalse(any('private' in n or '.env' in n or 'auth.json' in n or 'credentials.json' in n or 'handoffs' in n for n in archive.namelist()))
    def test_media_service_sources_retained_without_media_fixtures(self):
        sources = ['src/services/media/MediaProbe.cpp', 'src/services/media/MediaService.h',
                   'src/services/media/api/IMediaService.h']
        private = ['tests/media/private.txt', 'src/services/media/artifacts/private.txt',
                   'src/services/media/media/private.txt', 'src/services/media/clip.mp4']
        for name in sources + private:
            self.write(self.app/name, 'fixture')
        self.build()
        with zipfile.ZipFile(self.bundle) as archive:
            for name in sources:
                self.assertIn('application/' + name, archive.namelist())
            for name in private:
                self.assertNotIn('application/' + name, archive.namelist())
        self.assertTrue(validate(self.prov, bundle=self.bundle)['passed'])
    def test_personal_tests_and_internal_roadmaps_excluded(self):
        for name in ['tests/test_adaptive_orchestrator.py','tests/test_context_router.py','tests/test_rvlite_defect_triage.py','docs/CGPlay_B站个人作品视频脚本.md','docs/CGPlay_1.0.8_PLAN.md']:
            self.write(self.app/name,'private personal workflow')
        self.write(self.app/'CONTRIBUTING.md','contribution guide')
        self.write(self.app/'SECURITY.md','security guide')
        self.build()
        with zipfile.ZipFile(self.bundle) as archive:
            self.assertIn('application/CONTRIBUTING.md',archive.namelist())
            self.assertIn('application/SECURITY.md',archive.namelist())
            self.assertFalse(any('adaptive' in n or 'context_router' in n or '_PLAN' in n or 'B站' in n or 'defect_triage' in n for n in archive.namelist()))
    def test_required_source_gap_blocks_bundle_and_validation(self):
        self.data['releaseBlockers']=['required source missing'];self.prov.write_text(json.dumps(self.data))
        with self.assertRaisesRegex(ValueError,'gaps'):self.build()
        self.assertFalse(validate(self.prov)['passed'])
    def test_bundle_hash_mismatch_fails(self):
        self.build();self.bundle.with_suffix('.zip.sha256').write_text('0'*64)
        self.assertFalse(validate(self.prov,bundle=self.bundle)['passed'])
    def test_source_parts_index_and_missing_part(self):
        self.dep.write_bytes(b'x'*500)
        self.data['files'][0]['sha256']=digest(self.dep);self.prov.write_text(json.dumps(self.data))
        result=build_bundle(self.app,self.prov,self.mapping,self.bundle,max_part_bytes=550)
        self.assertEqual(len(result['assets']),2)
        self.assertTrue(validate(self.prov,bundle=self.bundle)['passed'])
        (self.bundle.parent/result['assets'][1]['name']).unlink()
        with self.assertRaises(FileNotFoundError):validate(self.prov,bundle=self.bundle)
    def test_dependency_hash_mismatch_refused(self):
        self.dep.write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError,'checksum'): self.build()
        self.assertFalse(self.bundle.exists())
    def test_missing_dependency_refused(self):
        self.dep.unlink()
        with self.assertRaises(FileNotFoundError):self.build()
    def test_duplicate_dependency_name_refused(self):
        self.data['files'].append(dict(self.data['files'][0],id='duplicate'))
        self.prov.write_text(json.dumps(self.data))
        with self.assertRaisesRegex(ValueError,'Duplicate'):self.build()
    def test_wrong_application_version_refused(self):
        self.write(self.app/'CMakeLists.txt','project(CGPlay VERSION 1.0.7)')
        with self.assertRaisesRegex(ValueError,'version'):self.build()
    def test_application_license_required(self):
        self.write(self.app/'LICENSE.txt','MIT')
        with self.assertRaisesRegex(ValueError,'GPLv3'):self.build()
    def test_path_traversal_refused(self):
        for name in ['../private','/absolute','a\\b','C:/path']:
            with self.assertRaises(ValueError):safe_relative(name)
    def test_existing_output_unchanged(self):
        self.bundle.write_bytes(b'keep')
        with self.assertRaisesRegex(ValueError,'never overwritten'):self.build()
        self.assertEqual(self.bundle.read_bytes(),b'keep')
    def test_missing_qrc_asset_refused(self):
        self.write(self.app/'resources/resources.qrc','<RCC><qresource><file>missing.png</file></qresource></RCC>')
        with self.assertRaisesRegex(ValueError,'QRC'):public_source_files(self.app)
    def test_required_notice_gate(self):
        stage=self.root/'stage';stage.mkdir()
        for name in ['LICENSE.txt','licenses/CGPlay_Commercial_EULA.txt','licenses/THIRD_PARTY_NOTICES.txt','licenses/FFMPEG_SOURCE_OFFER.txt','licenses/DEPENDENCY_SOURCES_1.0.8.md']:
            self.write(stage/name,'GNU GENERAL PUBLIC LICENSE')
        self.write(stage/'licenses/DEPENDENCY_PROVENANCE_1.0.8.json',json.dumps(self.data))
        self.assertFalse(validate(self.prov,app_root=stage)['passed'])
        self.write(stage/self.data['requiredNotices'][0],'license')
        self.assertTrue(validate(self.prov,app_root=stage)['passed'])
    def test_reparse_source_refused(self):
        outside=self.root/'private';outside.mkdir();self.write(outside/'credential.txt','secret')
        source=self.app/'tools';source.mkdir()
        if os.name=='nt':
            run=subprocess.run(['cmd','/c','mklink','/J',str(source/'linked'),str(outside)],capture_output=True,creationflags=subprocess.CREATE_NO_WINDOW)
            self.assertEqual(run.returncode,0)
            self.addCleanup(lambda: os.rmdir(source/'linked') if (source/'linked').exists() else None)
        else:(source/'linked').symlink_to(outside,target_is_directory=True)
        with self.assertRaisesRegex(ValueError,'Reparse'): public_source_files(self.app)
    def test_component_zip_layout_and_publisher_full_identity(self):
        package=self.root/'package';package.mkdir()
        required=['CGPlay.exe','CGPlayQuickLook.exe','ffmpeg.exe','ffprobe.exe','runtime/python/python.exe','runtime/codex/codex.exe','tools/cgplay/export/export_engine.py','tools/cgplay/export/compare_engine.py','tools/cgplay/export/recording_finalize.py','runtime/recorder/CGPlayRecorder.exe','runtime/recorder/runtime-manifest.json','LICENSE.txt','licenses/THIRD_PARTY_NOTICES.txt','licenses/FFMPEG_SOURCE_OFFER.txt','licenses/DEPENDENCY_SOURCES_1.0.8.md','licenses/DEPENDENCY_PROVENANCE_1.0.8.json']
        for name in required:self.write(package/name,'fixture '+name)
        out=self.root/'releases';out.mkdir();self.write(out/'keep-sources.zip','keep')
        self.ps('build_release_components.ps1','-ProjectRoot',ROOT,'-PackageRoot',package,'-OutputDir',out,'-Version','1.0.8')
        self.assertEqual((out/'keep-sources.zip').read_text(),'keep')
        manifest=json.loads((out/'manifest.json').read_text(encoding='utf-8-sig'))
        self.assertEqual(set(manifest['components']),{'core','ffmpeg','tools','python-runtime','recording-runtime'})
        for key,component in manifest['components'].items():
            archive=out/component['archiveName'];self.assertEqual(digest(archive),component['sha256'])
            self.assertIn('/v1.0.8/',component['url']);self.assertNotIn('netlify',component['url'])
            with zipfile.ZipFile(archive) as zipped:
                for name in component['requiredFiles']:self.assertIn(name,zipped.namelist())
                self.assertTrue(any('THIRD_PARTY_NOTICES.txt' in n for n in zipped.namelist()))
                if key=='tools':self.assertNotIn('tools/cgplay/export/export_engine.py',zipped.namelist())
                if key=='core':self.assertIn('runtime/codex/codex.exe',zipped.namelist())
        installer=self.root/'installers'
        self.write(installer/'full/CGPlay_Setup_1.0.8_full.exe','REAL FULL')
        self.write(installer/'lite/CGPlay_Setup_1.0.8_lite.exe','REAL LITE')
        self.write(out/'cgplay-corresponding-sources-1.0.8.zip','sources')
        self.write(out/'cgplay-corresponding-sources-1.0.8.zip.sha256','sha')
        notes=self.root/'notes.md';self.write(notes,'release')
        result=json.loads(self.ps('publish_github_release.ps1','-ProjectRoot',ROOT,'-ReleaseDir',out,'-InstallerRoot',installer,'-Version','1.0.8','-NotesFile',notes,'-DryRun'))
        self.assertTrue(result['fullInstaller'].endswith('CGPlay_Setup_1.0.8_full.exe'))
        self.assertEqual((installer/'full/CGPlay_Setup_1.0.8_full.exe').read_text(),'REAL FULL')
        self.assertTrue(any('recording-runtime' in p for p in result['assets']))
        self.assertTrue(any('corresponding-sources' in p for p in result['assets']))
    def test_real_publisher_digest_and_zip_gate_before_fake_gh(self):
        package=self.root/'package';package.mkdir()
        for name in ['CGPlay.exe','CGPlayQuickLook.exe','ffmpeg.exe','ffprobe.exe','runtime/python/python.exe','tools/cgplay/export/export_engine.py','tools/cgplay/export/compare_engine.py','tools/cgplay/export/recording_finalize.py','runtime/recorder/CGPlayRecorder.exe','runtime/recorder/runtime-manifest.json','LICENSE.txt','licenses/THIRD_PARTY_NOTICES.txt','licenses/FFMPEG_SOURCE_OFFER.txt','licenses/DEPENDENCY_SOURCES_1.0.8.md','licenses/DEPENDENCY_PROVENANCE_1.0.8.json']:
            self.write(package/name,'fixture '+name)
        out=self.root/'releases'
        self.ps('build_release_components.ps1','-ProjectRoot',ROOT,'-PackageRoot',package,'-OutputDir',out,'-Version','1.0.8')
        self.write(self.app/'docs/DEPENDENCY_PROVENANCE_1.0.8.json',json.dumps(self.data))
        source=out/'cgplay-corresponding-sources-1.0.8.zip'
        build_bundle(self.app,self.prov,self.mapping,source)
        installer=self.root/'installers';self.write(installer/'full/CGPlay_Setup_1.0.8_full.exe','full')
        notes=self.app/'docs/RELEASE_NOTES_1.0.8.md';self.write(notes,'release notes')
        fakebin=self.root/'fakebin';fakebin.mkdir();log=self.root/'fake-gh.log'
        self.write(fakebin/'gh.cmd','@echo off\necho %*>>"%CGPLAY_FAKE_GH_LOG%"\nexit /b 0\n')
        command=[shutil.which('pwsh') or shutil.which('powershell'),'-NoProfile','-ExecutionPolicy','Bypass','-File',str(ROOT/'tools/publish_github_release.ps1'),'-ProjectRoot',str(self.app),'-ReleaseDir',str(out),'-InstallerRoot',str(installer),'-Version','1.0.8','-NotesFile',str(notes)]
        env=dict(os.environ,PATH=str(fakebin)+os.pathsep+os.environ['PATH'],CGPLAY_FAKE_GH_LOG=str(log))
        def invoke():return subprocess.run(command,capture_output=True,text=True,env=env,creationflags=subprocess.CREATE_NO_WINDOW)
        run=invoke();self.assertEqual(run.returncode,0,run.stdout+run.stderr)
        self.assertIn('release upload',log.read_text());log.unlink()
        (out/'cgplay-tools-win64.zip').write_bytes(b'broken ZIP')
        run=invoke();self.assertNotEqual(run.returncode,0);self.assertFalse(log.exists())
        # Even updating all declared digests cannot make a malformed ZIP valid.
        manifest=json.loads((out/'manifest.json').read_text(encoding='utf-8-sig'))
        manifest['components']['tools']['sha256']=digest(out/'cgplay-tools-win64.zip')
        self.write(out/'manifest.json',json.dumps(manifest));self.write(out/'manifest.sha256',digest(out/'manifest.json'))
        run=invoke();self.assertNotEqual(run.returncode,0);self.assertFalse(log.exists())
    def test_staging_guard_ast_and_real_junction_escape(self):
        stage=self.app/'build_win_full/package/full';stage.mkdir(parents=True)
        outside=self.root/'user-media';outside.mkdir();self.write(outside/'keep.txt','user media survives')
        linked=stage/'linked'
        run=subprocess.run(['cmd','/c','mklink','/J',str(linked),str(outside)],capture_output=True,creationflags=subprocess.CREATE_NO_WINDOW)
        self.assertEqual(run.returncode,0)
        self.addCleanup(lambda: os.rmdir(linked) if linked.exists() else None)
        script=self.root/'guard.ps1'
        self.write(script,r'''param([string]$ToolRoot,[string]$ProjectRoot,[string]$Outside)
$ErrorActionPreference='Stop'
foreach($name in @('package_installer.ps1','build_release_components.ps1','publish_github_release.ps1','collect_release_files.ps1','validate_commercial_release.ps1')) {
 $tokens=$null;$errors=$null
 $ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $ToolRoot $name),[ref]$tokens,[ref]$errors)
 if($errors.Count){throw "Invalid PowerShell AST: $name"}
 if($name -eq 'package_installer.ps1'){$packageAst=$ast}
}
$function=$packageAst.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Assert-PackageStagingPath'},$true)
. ([scriptblock]::Create($function.Extent.Text))
Assert-PackageStagingPath (Join-Path $ProjectRoot 'build_win_full/package/full/safe-new-child')
foreach($target in @($Outside,(Join-Path $ProjectRoot 'build_win_full/package/full/linked'))) {
 $rejected=$false
 try {Assert-PackageStagingPath $target} catch {$rejected=$true}
 if(-not $rejected){throw "Unsafe staging target accepted"}
}
''')
        run=subprocess.run([shutil.which('pwsh') or shutil.which('powershell'),'-NoProfile','-File',str(script),str(ROOT/'tools'),str(self.app),str(outside)],capture_output=True,text=True,creationflags=subprocess.CREATE_NO_WINDOW)
        self.assertEqual(run.returncode,0,run.stdout+run.stderr)
        self.assertEqual((outside/'keep.txt').read_text(),'user media survives')
    def test_selected_build_identity_and_legacy_source_alias(self):
        preset={'configurePresets':[{'name':'windows-full-release','generator':'Visual Studio 17 2022','architecture':'x64','binaryDir':'${sourceDir}/build_win_full','cacheVariables':{'CGPLAY_USE_TLRENDER':'ON','CMAKE_BUILD_TYPE':'Release','Qt6_DIR':'sdk/lib/cmake/Qt6'}}], 'buildPresets':[{'name':'windows-full-release-apps','configurePreset':'windows-full-release','configuration':'Release','targets':['CGPlay','CGPlayQuickLook']}]}
        self.write(self.app/'CMakePresets.json',json.dumps(preset))
        qt=self.root/'sdk';self.write(qt/'Qt6Config.cmake','config')
        fresh=self.app/'fresh build';release=fresh/'bin/Release'
        for name in ['CGPlay.exe','CGPlayQuickLook.exe']:self.write(release/name,'built application fixture')
        alias=self.root/'legacy-project'
        run=subprocess.run(['cmd','/c','mklink','/J',str(alias),str(self.app)],capture_output=True,creationflags=subprocess.CREATE_NO_WINDOW)
        self.assertEqual(run.returncode,0);self.addCleanup(lambda:os.rmdir(alias) if alias.exists() else None)
        def cache(source,binary):
            return f'CMAKE_HOME_DIRECTORY:INTERNAL={source}\nCMAKE_CACHEFILE_DIR:INTERNAL={binary}\nCGPlay_BINARY_DIR:STATIC={binary}\nCMAKE_PROJECT_NAME:STATIC=CGPlay\nCGPLAY_USE_TLRENDER:BOOL=ON\nCMAKE_GENERATOR:INTERNAL=Visual Studio 17 2022\nCMAKE_GENERATOR_PLATFORM:INTERNAL=x64\nCMAKE_BUILD_TYPE:STRING=Release\nQt6_DIR:PATH={qt}\n'
        # A source compatibility junction is valid only when its resolved source
        # and source-relative binary directory identify this exact selected tree.
        self.write(fresh/'CMakeCache.txt',cache(alias,alias/'fresh build'))
        report=self.root/'selected.json'
        command=[shutil.which('pwsh') or shutil.which('powershell'),'-NoProfile','-File',str(ROOT/'tools/validate_official_build_config.ps1'),'-ProjectRoot',str(self.app),'-BuildDir',str(fresh),'-ReleaseDir',str(release),'-RequireBuiltApps','-ReportPath',str(report)]
        def invoke():return subprocess.run(command,capture_output=True,text=True,creationflags=subprocess.CREATE_NO_WINDOW)
        run=invoke();self.assertEqual(run.returncode,0,run.stdout+run.stderr)
        self.assertEqual(Path(json.loads(report.read_text(encoding='utf-8-sig'))['buildDir']),fresh)
        # A good default cache cannot validate a different selected tree.
        decoy=self.app/'build_win_full';self.write(decoy/'CMakeCache.txt',cache(self.app,decoy))
        self.write(fresh/'CMakeCache.txt',cache(self.app,decoy))
        self.assertNotEqual(invoke().returncode,0)
        self.write(fresh/'CMakeCache.txt',cache(self.root/'other-source',fresh))
        self.assertNotEqual(invoke().returncode,0)
if __name__=='__main__':unittest.main()
