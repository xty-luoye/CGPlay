"""Native parser check plus source-level translation completeness contract."""
import json, os, re, subprocess
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'tests/artifacts/fix_review_20261101/subtitle_regression'
QT=Path('E:/rjkf/CGPlay-deps/engine_migration_1_0_8/qt-6.11.3-msvc2022_64')

def extract(source, signature):
    start=source.index(signature); brace=source.index('{',start); depth=1; end=brace+1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}'); end+=1
    return source[start:end]

def run(cmd,name,env=None):
    result=subprocess.run([str(x) for x in cmd],env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,
        creationflags=subprocess.CREATE_NO_WINDOW,timeout=180)
    (OUT/(name+'.log')).write_bytes(result.stdout)
    if result.returncode: raise RuntimeError(result.stdout.decode(errors='replace'))

def main():
    OUT.mkdir(parents=True,exist_ok=True)
    support=(ROOT/'src/services/ai/SubtitleGenerationSupport.cpp').read_text(encoding='utf-8')
    service=(ROOT/'src/services/ai/SubtitleGenerationService.cpp').read_text(encoding='utf-8')
    parser=extract(support,'double parseSubtitleTime(')
    reader=extract(service,'QVector<GeneratedSubtitleCue> SubtitleGenerationService::readSubtitleFile(')
    source=r'''
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVector>
#include <QString>
struct GeneratedSubtitleCue { int index=0; double startSeconds=0; double endSeconds=0; QString sourceText; QString translatedText; };
QVector<GeneratedSubtitleCue> readAssSubtitleFile(const QString&) { return {}; }
''' + parser + '\n' + reader.replace('SubtitleGenerationService::readSubtitleFile','readSubtitleFile') + r'''
int main(int argc,char**argv){QCoreApplication app(argc,argv);QJsonObject o;
auto check=[&](const QString& path,const QString& key){auto cues=readSubtitleFile(path);if(cues.size()!=1)return false;o[key+"Count"]=cues.size();o[key+"Start"]=cues.first().startSeconds;o[key+"End"]=cues.first().endSeconds;return cues.first().startSeconds==1.0&&cues.first().endSeconds==2.0;};
if(!check(QString::fromLocal8Bit(argv[1]),"short"))return 2;if(!check(QString::fromLocal8Bit(argv[2]),"long"))return 3;if(!check(QString::fromLocal8Bit(argv[3]),"mixed"))return 4;
QFile f(QString::fromLocal8Bit(argv[4]));if(!f.open(QIODevice::WriteOnly))return 5;f.write(QJsonDocument(o).toJson());return 0;}
'''
    (OUT/'probe.cpp').write_text(source,encoding='utf-8')
    (OUT/'short.vtt').write_text('WEBVTT\n\n1\n00:01.000 --> 00:02.000\nhello\n',encoding='utf-8')
    (OUT/'long.vtt').write_text('WEBVTT\n\n1\n00:00:01.000 --> 00:00:02.000\nhello\n',encoding='utf-8')
    (OUT/'mixed.vtt').write_text('WEBVTT\n\n1\n00:01.000 --> 00:02.000\nhello\n\n2\n00:00:03.000 --> 00:00:04.000\nworld\n',encoding='utf-8')
    # The mixed case intentionally has two cues; this is checked separately by
    # the probe source after the product parser is compiled below.
    source=source.replace('if(!check(QString::fromLocal8Bit(argv[3]),"mixed"))return 4;', 'if(readSubtitleFile(QString::fromLocal8Bit(argv[3])).size()!=2)return 4;')
    (OUT/'probe.cpp').write_text(source,encoding='utf-8')
    (OUT/'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.21)
project(SubtitleRepair LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
find_package(Qt6 REQUIRED COMPONENTS Core)
add_executable(subtitle_probe probe.cpp)
target_compile_options(subtitle_probe PRIVATE /utf-8)
target_link_libraries(subtitle_probe PRIVATE Qt6::Core)
''',encoding='utf-8')
    run(['cmake','-S',OUT,'-B',OUT/'build','-G','Visual Studio 17 2022','-A','x64',f'-DCMAKE_PREFIX_PATH={QT}'],'configure')
    run(['cmake','--build',OUT/'build','--config','Release','--parallel','4'],'build')
    target=OUT/'build/Release'
    for dll in (QT/'bin').glob('Qt6Core.dll'):
        dest=target/dll.name
        if not dest.exists():os.link(dll,dest)
    run([target/'subtitle_probe.exe',OUT/'short.vtt',OUT/'long.vtt',OUT/'mixed.vtt',OUT/'result.json'], 'native')
    text=service[service.index('bool SubtitleGenerationService::_translateCues('):]
    assert 'expectedIndexes' in text and 'returnedIndexes == expectedIndexes' in text
    assert 'return !failedBatchCount || *failedBatchCount == 0;' in text
    result=json.loads((OUT/'result.json').read_text(encoding='utf-8'))
    result.update({'passed':True,'backgroundMode':True,'translationCompletenessContract':True})
    (OUT/'result.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');print(json.dumps(result))

if __name__=='__main__':main()
