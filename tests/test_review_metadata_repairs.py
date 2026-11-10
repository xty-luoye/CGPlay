"""Native regressions for R19/R31/R40/R42/R43/R46/R47/R60; no user media/settings."""
import json
import os
from pathlib import Path
import subprocess
import unittest
import uuid
import zipfile

from tests.native_probe_runtime import deploy_runtime

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'tests/artifacts/fix_review_20261101/stage10'
QT = Path(os.environ.get('CGPLAY_TEST_QT_ROOT', 'E:/rjkf/CGPlay-deps/engine_migration_1_0_8/qt-6.11.3-msvc2022_64'))
TL = Path(os.environ.get('CGPLAY_TEST_TL_ROOT', 'E:/rj/CGV/tlRender-main/install-Release'))
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)


def function(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


COMMON = r'''
#define NOMINMAX
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <windows.h>
#include <cstdio>
int finish(QJsonObject report) {
 int visible=0; EnumWindows([](HWND h,LPARAM p)->BOOL {DWORD pid=0;GetWindowThreadProcessId(h,&pid);
 if(pid==GetCurrentProcessId()&&IsWindowVisible(h))++*reinterpret_cast<int*>(p);return TRUE;},reinterpret_cast<LPARAM>(&visible));
 bool passed=visible==0;for(auto it=report.begin();it!=report.end();++it)if(it.value().isBool())passed=passed&&it.value().toBool();
 report["backgroundMode"]=true;report["visiblePlatformWindows"]=visible;report["passed"]=passed;report["qtVersion"]=qVersion();
 puts(QJsonDocument(report).toJson(QJsonDocument::Compact).constData());return passed?0:1;
}
'''

ANNOTATIONS = r'''
#include "features/annotation/AnnotationManager.h"
#include "features/annotation/AnnotationStorage.h"
int main(int argc,char**argv) {
 QCoreApplication app(argc,argv);using namespace cgplay;QJsonObject result;const QDir dir(QString::fromUtf8(argv[1]));
 AnnotationItem a;a.id="A";a.points={QPointF(10,20)};a.comment="original";Comment c;c.text=a.comment;a.comments={c};
 AnnotationManager manager;manager.add(a);manager.selectAnnotation(a.id);manager.updateAnnotationComment(a.id,"edited");
 result["commentAndHistoryAgree"]=manager.get(a.id)->latestCommentText()=="edited"&&manager.getFrameSummary(0).contains("edited");
 result["undoRestoresHistory"]=manager.undo()&&manager.get(a.id)->comment=="original"&&manager.get(a.id)->latestCommentText()=="original";
 result["redoRestoresHistory"]=manager.redo()&&manager.get(a.id)->comment=="edited"&&manager.get(a.id)->latestCommentText()=="edited";
 const QString valid=dir.filePath("valid.review.json");QVector<AnnotationItem> loaded;QString error;
 result["saveLoadHistoryRoundTrip"]=AnnotationStorage::save(manager.all(),valid)&&AnnotationStorage::load(valid,&loaded,&error)&&loaded.size()==1&&loaded[0].latestCommentText()=="edited";
 manager.remove(a.id);AnnotationItem b;b.id="B";b.points={QPointF(30,40)};manager.replaceAnnotations({b});
 result["replacementIsHistoryBoundary"]=!manager.canUndo()&&!manager.canRedo()&&!manager.undo()&&manager.selectedAnnotationId().isEmpty()&&manager.count()==1&&manager.get("B");
 const QString bad=dir.filePath("broken.review.json");QFile broken(bad);broken.open(QIODevice::WriteOnly);broken.write("{unfinished");broken.close();
 loaded={b};bool ok=AnnotationStorage::load(bad,&loaded,&error);if(ok)manager.replaceAnnotations(loaded);
 result["failedImportPreservesCollection"]=!ok&&!error.isEmpty()&&manager.count()==1&&manager.get("B")&&loaded.size()==1&&loaded[0].id=="B";
 result["missingFilePreservesCollection"]=!AnnotationStorage::load(dir.filePath("missing.json"),&loaded,&error)&&loaded.size()==1;
 QFile empty(dir.filePath("empty.review.json"));empty.open(QIODevice::WriteOnly);empty.write("{\"version\":1,\"annotations\":[]}");empty.close();
 result["validEmptyCollectionAccepted"]=AnnotationStorage::load(empty.fileName(),&loaded,&error)&&loaded.isEmpty()&&error.isEmpty();
 QFile partial(dir.filePath("partial.review.json"));partial.open(QIODevice::WriteOnly);partial.write("{\"annotations\":[{\"id\":\"good\",\"points\":[[1,2]]},null]}");partial.close();loaded={b};
 result["partialParseNeverCommits"]=!AnnotationStorage::load(partial.fileName(),&loaded,&error)&&loaded.size()==1&&loaded[0].id=="B";
 return finish(result);
}
'''

MEDIA_MAIN = r'''
}
int main(int argc,char**argv) {
 QCoreApplication app(argc,argv);using namespace cgplay;const QDir dir(QString::fromUtf8(argv[1]));QJsonObject result;
 for(const QString ext:{QString("mkv"),QString("mp4")}) {MediaInfo info;probeVideo(dir.filePath("clip."+ext),info,nullptr);
 result[ext+"HasFullDuration"]=info.error.isEmpty()&&std::abs(info.durationSeconds-4.0)<.1&&info.effectiveFrameCount()==100;
 result[ext+"durationSeconds"]=info.durationSeconds;result[ext+"frames"]=info.frameCount;}
 auto touch=[&](const QString& name){QFile f(dir.filePath(name));f.open(QIODevice::WriteOnly);f.write("fixture");};
 touch("episode10.srt");touch("episode1.mp4");
 result["otherEpisodeRejected"]=collectExternalSubtitleCandidates(QFileInfo(dir.filePath("episode1.mp4"))).isEmpty();
 touch("episode1.en.srt");touch("episode1_zh.vtt");touch("episode1.srt");
 const auto paths=collectExternalSubtitleCandidates(QFileInfo(dir.filePath("episode1.mp4")));
 result["exactAndLanguageVariantsAccepted"]=paths.size()==3&&paths.first().endsWith("episode1.srt");
 return finish(result);
}
'''

OTIO_MAIN = r'''
#include "otio/OtioExporter.h"
#include "playlist/PlaylistModel.h"
namespace cgplay {const ShotItem& PlaylistModel::shotAt(int)const{static ShotItem unused;return unused;}}
int main(int argc,char**argv){QCoreApplication app(argc,argv);const QDir dir(QString::fromUtf8(argv[1]));QJsonObject result;
 for(const QString filename:{QString("dot.0001.png"),QString("short.01.PNG"),QString("under_0001.png"),QString("dash-000001.exr")}){
 cgplay::ShotItem shot;shot.path=dir.filePath(filename);shot.name=filename;shot.firstFrame=1;shot.lastFrame=2;shot.fps=24;
 cgplay::OtioExporter exporter;const QString output=dir.filePath(filename+".otio");bool ok=exporter.exportShotsToFile(output,{shot},"fixture");
 QFile file(output);ok=ok&&file.open(QIODevice::ReadOnly);auto root=QJsonDocument::fromJson(file.readAll()).object();
 auto clip=root["tracks"].toObject()["children"].toArray()[0].toObject()["children"].toArray()[0].toObject();
 auto media=clip["media_references"].toObject()["DEFAULT_MEDIA"].toObject();if(media.isEmpty())media=clip["media_reference"].toObject();
 QString reconstructed=media["name_prefix"].toString()+QString::number(1).rightJustified(media["frame_zero_padding"].toInt(),'0')+media["name_suffix"].toString();
 result[filename]=ok&&reconstructed==filename&&media["target_url_base"].toString().endsWith('/');}
 return finish(result);}
'''


class ReviewMetadataRepairs(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = OUT / 'source'; cls.build = OUT / 'build'
        cls.source.mkdir(parents=True, exist_ok=True)
        media = (ROOT / 'src/services/media/MediaProbe.cpp').read_text(encoding='utf-8-sig')
        signatures = ['bool fileHasSubtitleSuffix(', 'bool isCgplayGeneratedSubtitleSidecar(',
                      'QStringList collectExternalSubtitleCandidates(', 'int MediaInfo::effectiveFrameCount()',
                      'double MediaProbe::parseFpsRatio(', 'static void probeVideo(']
        extracted = '\n'.join(function(media, sig) for sig in signatures)
        settings = (ROOT / 'src/ui/app/ApplicationSettings.cpp').read_text(encoding='utf-8-sig')
        guard_start = settings.index('if (!source.isFile() || canonicalSource.isEmpty()')
        guard = settings[guard_start + 4:settings.index(') {', guard_start)]
        component = (ROOT / 'src/services/component/ComponentManager.cpp').read_text(encoding='utf-8-sig')
        quote_start = component.index('    const auto quotePath =', component.index('bool ComponentManager::_extractArchive('))
        start_end = component.index('QIODevice::ReadOnly);', quote_start) + len('QIODevice::ReadOnly);')
        archive_command = component[quote_start:start_end]
        paths = COMMON + r'''
#include <QProcess>
int main(int argc,char**argv){QCoreApplication app(argc,argv);const QDir dir(QString::fromUtf8(argv[1]));QJsonObject result;
const QString extractedRoot=QFileInfo(dir.filePath("extracted")).canonicalFilePath();
auto accepts=[&](const QString& path){QFileInfo source(path);const QString canonicalSource=source.canonicalFilePath();return !(GUARD);};
result["canonicalChildAccepted"]=accepts(dir.filePath("extracted/assets/background.png"));
result["siblingEscapeRejected"]=!accepts(dir.filePath("extracted_evil/background.png"));
result["missingRejected"]=!accepts(dir.filePath("extracted/assets/missing.png"));
for(const QString name:{QString("ordinary"),QString("O'Brien [fixture]")}){
const QString archivePath=dir.filePath(name+"/component.zip");const QString destinationPath=dir.filePath(name+"/output");
QDir().mkpath(destinationPath);QProcess process;
ARCHIVE_COMMAND
bool ok=process.waitForStarted(10000)&&process.waitForFinished(30000)&&process.exitStatus()==QProcess::NormalExit&&process.exitCode()==0;
result[name+"stderr"]=QString::fromLocal8Bit(process.readAllStandardError());
QFile marker(QDir(destinationPath).filePath("marker.txt"));result[name]=ok&&marker.open(QIODevice::ReadOnly)&&marker.readAll()=="component fixture";
}return finish(result);}
'''
        paths = paths.replace('GUARD', guard).replace('ARCHIVE_COMMAND', archive_command)
        media_headers = r'''
#include "media/MediaProbe.h"
#include "common/jobs/JobSystem.h"
#include <QProcess>
#include <algorithm>
#include <cmath>
#include <limits>
namespace cgplay {
QString MediaProbe::locateFfprobe(){return qEnvironmentVariable("CGPLAY_TEST_FFPROBE");}
'''
        for filename, code in [('annotations.cpp', COMMON + ANNOTATIONS),
                               ('media.cpp', COMMON + media_headers + extracted + MEDIA_MAIN),
                               ('otio.cpp', COMMON + OTIO_MAIN), ('paths.cpp', paths)]:
            (cls.source / filename).write_text(code, encoding='utf-8')
        cmake = '''cmake_minimum_required(VERSION 3.21)
project(ReviewMetadataRepairs LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Concurrent)
'''
        sources = {
            'annotations': ['src/features/annotation/AnnotationManager.cpp', 'src/features/annotation/AnnotationManager.h', 'src/features/annotation/AnnotationStorage.cpp'],
            'media': ['src/common/jobs/JobSystem.cpp', 'src/common/jobs/JobSystem.h'],
            'otio': ['src/features/otio/OtioExporter.cpp'],
            'paths': [],
        }
        for target, files in sources.items():
            cmake += f'add_executable({target} {target}.cpp ' + ' '.join('"' + (ROOT / f).as_posix() + '"' for f in files) + ')\n'
            cmake += f'target_include_directories({target} PRIVATE ' + ' '.join('"' + (ROOT / f).as_posix() + '"' for f in ['src', 'src/common', 'src/features', 'src/services']) + ')\n'
            cmake += f'target_compile_options({target} PRIVATE /utf-8 /Zc:__cplusplus /permissive-)\n'
            cmake += f'target_link_libraries({target} PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Concurrent user32)\n'
        cmake += f'target_include_directories(otio PRIVATE "{TL.as_posix()}/include")\n'
        cmake += 'target_compile_definitions(otio PRIVATE OTIO_STATIC OPENTIME_STATIC)\n'
        cmake += f'target_link_libraries(otio PRIVATE "{TL.as_posix()}/lib/opentimelineio.lib" "{TL.as_posix()}/lib/opentime.lib")\n'
        (cls.source / 'CMakeLists.txt').write_text(cmake, encoding='utf-8')
        for name, command in [('configure', ['cmake', '-S', str(cls.source), '-B', str(cls.build), '-G', 'Visual Studio 17 2022', '-A', 'x64', f'-DCMAKE_PREFIX_PATH={QT.as_posix()}']),
                              ('build', ['cmake', '--build', str(cls.build), '--config', 'Release', '--parallel', '4'])]:
            result = subprocess.run(command, capture_output=True, creationflags=NO_WINDOW, timeout=180)
            (OUT / (name + '.log')).write_bytes(result.stdout + result.stderr)
            if result.returncode:
                raise AssertionError((result.stdout + result.stderr).decode(errors='replace')[-6000:])
        deploy_runtime(cls.build / 'Release/annotations.exe', QT, (ROOT / 'build_win_full/bin/Release',))
        cls.env = dict(os.environ, QT_QPA_PLATFORM='offscreen', QT_PLUGIN_PATH=str(QT / 'plugins'),
                       CGPLAY_TEST_FFPROBE=str(ROOT / 'build_win_full/bin/Release/ffprobe.exe'))

    def probe(self, name, fixture=None):
        fixture = fixture or OUT / ('fixture_' + name + '_' + uuid.uuid4().hex)
        fixture.mkdir(exist_ok=True)
        result = subprocess.run([str(self.build / ('Release/' + name + '.exe')), str(fixture)], env=self.env,
                                capture_output=True, creationflags=NO_WINDOW, timeout=30)
        (OUT / (name + '.log')).write_bytes(result.stdout + result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        report = json.loads(result.stdout.decode('utf-8').splitlines()[-1])
        (OUT / (name + '.json')).write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
        self.assertTrue(report['passed'], report)
        self.assertEqual(0, report['visiblePlatformWindows'])

    def test_annotations_edit_undo_replace_and_failed_import(self):
        self.probe('annotations')

    def test_mkv_duration_and_subtitle_episode_identity(self):
        fixture = OUT / ('fixture_media_' + uuid.uuid4().hex); fixture.mkdir(exist_ok=True)
        ffmpeg = ROOT / 'build_win_full/bin/Release/ffmpeg.exe'
        for ext in ('mp4', 'mkv'):
            result = subprocess.run([str(ffmpeg), '-hide_banner', '-loglevel', 'error', '-y', '-f', 'lavfi', '-i',
                                     'testsrc2=size=160x90:rate=25', '-frames:v', '100', '-c:v', 'libx264', str(fixture / ('clip.' + ext))],
                                    capture_output=True, creationflags=NO_WINDOW, timeout=30)
            self.assertEqual(0, result.returncode, result.stderr)
        self.probe('media', fixture)

    def test_otio_uses_original_sequence_prefix_padding_and_case(self):
        self.probe('otio')

    def test_canonical_import_boundary_and_literal_archive_paths(self):
        fixture = OUT / ('fixture_paths_' + uuid.uuid4().hex)
        for relative in ('extracted/assets/background.png', 'extracted_evil/background.png'):
            path = fixture / relative; path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(b'fixture')
        for name in ('ordinary', "O'Brien [fixture]"):
            directory = fixture / name; directory.mkdir()
            with zipfile.ZipFile(directory / 'component.zip', 'w') as archive:
                archive.writestr('marker.txt', 'component fixture')
        self.probe('paths', fixture)


if __name__ == '__main__':
    unittest.main()
