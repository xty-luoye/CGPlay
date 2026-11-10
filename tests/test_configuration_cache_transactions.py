"""R54/R102: isolated credential and subtitle failure paths, with real Windows IO."""
import json
import os
from pathlib import Path
import subprocess
import unittest
import uuid

from tests.native_probe_runtime import deploy_runtime
from tests.test_review_metadata_repairs import COMMON, QT, function

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'tests/artifacts/fix_review_20261101/stage15'
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)

DETECTOR = r'''
#include "ai/discovery/AIProviderDetector.h"
#include "ai/api/IAICredentialStore.h"
#include "settings/api/ISettingsService.h"
#include <QHash>
using namespace cgplay;
class MemorySettings:public ISettingsService {public:
QHash<QString,QVariant>values;QVariant value(const QString&k,const QVariant&f={})const override{return values.value(k,f);}void setValue(const QString&k,const QVariant&v)override{values[k]=v;}bool contains(const QString&k)const override{return values.contains(k);}QStringList allKeys()const override{return values.keys();}void remove(const QString&k)override{values.remove(k);}void sync()override{}QString organization()const override{return "fixture";}QString application()const override{return "fixture";}};
class LegacySecrets:public IAICredentialStore {public:QHash<QString,QByteArray>values;int individualWrites=0;
bool storeSecret(const QString&k,const QByteArray&v,QString*)override{++individualWrites;values[k]=v;return true;}QByteArray loadSecret(const QString&k,QString*)const override{return values.value(k);}bool hasSecret(const QString&k)const override{return values.contains(k);}bool removeSecret(const QString&k,QString*)override{values.remove(k);return true;}};
class AtomicSecrets:public LegacySecrets,public IAtomicAICredentialStore {public:int failAt=0,prepared=0;
bool storeSecrets(const QMap<QString,QByteArray>&secrets,QString*error)override{auto candidate=values;prepared=0;for(auto it=secrets.begin();it!=secrets.end();++it){if(++prepared==failAt){*error="Injected candidate failure";return false;}candidate[it.key()]=it.value();}values=candidate;return true;}};
int main(int argc,char**argv){QCoreApplication app(argc,argv);QJsonObject report;
for(int failAt:{0,1,2}){MemorySettings settings;AtomicSecrets secrets;secrets.failAt=failAt;settings.values["ai/connection/baseUrl"]="https://old.invalid/v1";secrets.values["ai/defaultApiKey"]="old";secrets.values["qwen/apiKey"]="old-category";
AIProviderDetector detector(&secrets,&settings);AIDetectionRequest request;request.apiKey="new-fixture";AIDetectionResult detected;detected.success=true;detected.category=AIProviderCategory::Qwen;detected.providerType="Qwen";detected.baseUrl="https://new.invalid/v1";detected.models={"fixture"};detected.recommendedModel="fixture";QString error;
const bool saved=detector.saveConfiguration(request,detected,&error);
report[QString::number(failAt)+"candidateFailureAtomic"]=failAt?(!saved&&!error.isEmpty()&&secrets.values["ai/defaultApiKey"]=="old"&&secrets.values["qwen/apiKey"]=="old-category"&&settings.value("ai/connection/baseUrl").toString()=="https://old.invalid/v1"):(saved&&secrets.values["ai/defaultApiKey"]=="new-fixture"&&secrets.values["qwen/apiKey"]=="new-fixture"&&settings.value("ai/connection/baseUrl").toString()=="https://new.invalid/v1");
report[QString::number(failAt)+"noIndividualOverwrite"]=secrets.individualWrites==0;}
MemorySettings settings;LegacySecrets legacy;legacy.values["ai/defaultApiKey"]="old";AIProviderDetector detector(&legacy,&settings);AIDetectionRequest request;request.apiKey="new-fixture";AIDetectionResult detected;detected.success=true;detected.category=AIProviderCategory::Qwen;detected.baseUrl="https://new.invalid/v1";QString error;
report["unsupportedAtomicStoreFailsWithoutWrites"]=!detector.saveConfiguration(request,detected,&error)&&!error.isEmpty()&&legacy.individualWrites==0&&legacy.values["ai/defaultApiKey"]=="old"&&settings.allKeys().isEmpty();
return finish(report);}
'''

STORE = r'''
#include "ai/WindowsDpapiCredentialStore.h"
QString credentialFixturePath;
QByteArray bytes(){QFile f(credentialFixturePath);f.open(QIODevice::ReadOnly);return f.readAll();}
int main(int argc,char**argv){QCoreApplication app(argc,argv);credentialFixturePath=QDir(QString::fromUtf8(argv[1])).filePath("credentials.ini");cgplay::WindowsDpapiCredentialStore store;QString error;QJsonObject report;
report["initialDpapiWrite"]=store.storeSecrets({{"ai/defaultApiKey","old"},{"qwen/apiKey","old-category"},{"other","untouched"}},&error);
report["initialError"]=error;
const auto before=bytes();HANDLE reader=CreateFileW(reinterpret_cast<LPCWSTR>(credentialFixturePath.utf16()),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
const bool blocked=store.storeSecrets({{"ai/defaultApiKey","new"},{"qwen/apiKey","new-category"}},&error);if(reader!=INVALID_HANDLE_VALUE)CloseHandle(reader);
report["failedAtomicCommitPreservesDiskAndCache"]=reader!=INVALID_HANDLE_VALUE&&!blocked&&!error.isEmpty()&&bytes()==before&&store.loadSecret("ai/defaultApiKey")=="old"&&store.loadSecret("qwen/apiKey")=="old-category";
report["retryCommitsBothSecrets"]=store.storeSecrets({{"ai/defaultApiKey","new"},{"qwen/apiKey","new-category"}},&error)&&store.loadSecret("ai/defaultApiKey")=="new"&&store.loadSecret("qwen/apiKey")=="new-category"&&store.loadSecret("other")=="untouched";
const auto valid=bytes();report["invalidCandidatePreservesActiveSecrets"]=!store.storeSecrets({{"","bad"},{"other","changed"}},&error)&&bytes()==valid&&store.loadSecret("other")=="untouched";
report["singleSecretRemovalPreservesOthers"]=store.removeSecret("qwen/apiKey",&error)&&!store.hasSecret("qwen/apiKey")&&store.loadSecret("ai/defaultApiKey")=="new"&&store.loadSecret("other")=="untouched";
return finish(report);}
'''

CACHE_STUBS = r'''
#include <QDateTime>
#include <QSaveFile>
#include <QStandardPaths>
struct SubtitleGenerationService{static QString defaultSourceSrtPath(const QString&);static QString mediaFingerprint(const QString&){return "fixture-fingerprint";}};
QJsonObject makeSummary(QJsonArray assertions){return {{"failed",assertions.size()}};}
OUTPUT_DIRECTORY
BASE_PATH
QString SubtitleGenerationService::defaultSourceSrtPath(const QString&path){return baseOutputPath(path,{})+".source.srt";}
GUARD
QJsonObject backupRestore(QStringList touchedPaths,bool failRestore){QJsonObject report;QJsonArray assertions;
auto addAssertion=[&](QString,bool,QString){assertions.append("failed");};
SAVED_TYPE
QVector<SavedSidecar> savedSidecars;
BACKUP_RESTORE
for(const auto&saved:savedSidecars){QFile f(saved.path);f.open(QIODevice::WriteOnly);f.write("modified fixture");}
HANDLE locked=INVALID_HANDLE_VALUE;if(failRestore)locked=CreateFileW(reinterpret_cast<LPCWSTR>(touchedPaths.first().utf16()),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
report["restored"]=restoreSidecars();if(locked!=INVALID_HANDLE_VALUE)CloseHandle(locked);return report;}
QByteArray content(QString path){QFile f(path);f.open(QIODevice::ReadOnly);return f.readAll();}
int main(int argc,char**argv){QCoreApplication app(argc,argv);const QString root=QString::fromUtf8(argv[1]);QJsonObject report;qunsetenv("CGPLAY_AUTOMATION_SUBTITLE_CACHE_ROOT");
const auto normal=defaultSubtitleOutputDirectory(root+"/media.mp4");report["directUnisolatedDiagnosticRejected"]=!subtitleAutomationCacheReady(root+"/media.mp4");
const QString isolated=QDir(root).filePath("cache");QDir().mkpath(isolated);qputenv("CGPLAY_AUTOMATION_SUBTITLE_CACHE_ROOT",isolated.toUtf8());
report["isolatedDirectorySelected"]=defaultSubtitleOutputDirectory(root+"/media.mp4")==isolated&&normal!=isolated&&subtitleAutomationCacheReady(root+"/media.mp4");
QStringList paths;for(int i=0;i<3;++i){QString path=QDir(isolated).filePath(QString::number(i)+".vtt");QFile f(path);f.open(QIODevice::WriteOnly);f.write("original fixture");paths.append(path);}
HANDLE reader=CreateFileW(reinterpret_cast<LPCWSTR>(paths.first().utf16()),GENERIC_READ,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);const auto locked=backupRestore(paths,false);if(reader!=INVALID_HANDLE_VALUE)CloseHandle(reader);
report["failedBackupAbortsBeforeModification"]=reader!=INVALID_HANDLE_VALUE&&locked["summary"].toObject()["failed"].toInt()>0&&content(paths[0])=="original fixture"&&content(paths[1])=="original fixture";
report["successfulRestorePreservesAllBytes"]=backupRestore(paths,false)["restored"].toBool()&&content(paths[0])=="original fixture"&&content(paths[1])=="original fixture"&&content(paths[2])=="original fixture";
const auto failed=backupRestore(paths,true);report["failedRestoreReportedAndBackupRetained"]=!failed["restored"].toBool()&&content(paths[0]+".restore-backup")=="original fixture"&&content(paths[0])=="modified fixture";
return finish(report);}
'''


class ConfigurationCacheTransactions(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        OUT.mkdir(parents=True, exist_ok=True)
        store = (ROOT / 'src/services/ai/WindowsDpapiCredentialStore.cpp').read_text(encoding='utf-8-sig')
        path_method = function(store, 'QString WindowsDpapiCredentialStore::_settingsPath(')
        store = '#include <QString>\nextern QString credentialFixturePath;\n' + store.replace(path_method, 'QString WindowsDpapiCredentialStore::_settingsPath() const {return credentialFixturePath;}')
        (OUT / 'store_backend.cpp').write_text(store, encoding='utf-8')
        support = (ROOT / 'src/services/ai/SubtitleGenerationSupport.cpp').read_text(encoding='utf-8-sig')
        automation = (ROOT / 'src/ui/app/MainWindowAutomationSubtitles.cpp').read_text(encoding='utf-8-sig')
        a = automation.index('    struct SavedSidecar {')
        saved_type = automation[a:automation.index('    QVector<SavedSidecar>', a)]
        a = automation.index('    for (const QString& path : touchedPaths)', a)
        backup = automation[a:automation.index('    auto setMtime =', a)]
        cache = CACHE_STUBS.replace('OUTPUT_DIRECTORY', function(support, 'QString defaultSubtitleOutputDirectory('))
        cache = cache.replace('BASE_PATH', function(support, 'QString baseOutputPath(')).replace('GUARD', function(automation, 'bool subtitleAutomationCacheReady('))
        cache = cache.replace('SAVED_TYPE', saved_type).replace('BACKUP_RESTORE', backup)
        for name, source in [('detector', DETECTOR), ('store', STORE), ('cache', cache)]:
            (OUT / (name + '.cpp')).write_text(COMMON + source, encoding='utf-8')
        cmake = f'''cmake_minimum_required(VERSION 3.21)
project(ConfigurationCacheTransactions LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Network)
add_executable(detector detector.cpp "{ROOT.as_posix()}/src/services/ai/discovery/AIProviderDetector.cpp" "{ROOT.as_posix()}/src/services/ai/discovery/AIConnectionValidator.cpp" "{ROOT.as_posix()}/src/services/ai/discovery/AIModelDiscovery.cpp")
add_executable(store store.cpp store_backend.cpp)
add_executable(cache cache.cpp)
foreach(target detector store cache)
target_include_directories(${{target}} PRIVATE "{ROOT.as_posix()}/src" "{ROOT.as_posix()}/src/services" "{ROOT.as_posix()}/src/common" "{ROOT.as_posix()}/src/services/ai" "{ROOT.as_posix()}/src/features")
target_compile_options(${{target}} PRIVATE /utf-8 /Zc:__cplusplus /permissive-)
target_link_libraries(${{target}} PRIVATE Qt6::Core Qt6::Gui Qt6::Network user32 crypt32)
endforeach()
'''
        (OUT / 'CMakeLists.txt').write_text(cmake, encoding='utf-8')
        for name, command in [('configure', ['cmake', '-S', str(OUT), '-B', str(OUT / 'build'), '-G', 'Visual Studio 17 2022', '-A', 'x64', f'-DCMAKE_PREFIX_PATH={QT.as_posix()}']),
                              ('build', ['cmake', '--build', str(OUT / 'build'), '--config', 'Release', '--parallel', '4'])]:
            result = subprocess.run(command, capture_output=True, creationflags=NO_WINDOW, timeout=120)
            (OUT / (name + '.log')).write_bytes(result.stdout + result.stderr)
            if result.returncode:
                raise AssertionError((result.stdout + result.stderr).decode(errors='replace')[-5000:])
        deploy_runtime(OUT / 'build/Release/detector.exe', QT, (ROOT / 'build_win_full/bin/Release',))

    def probe(self, name):
        fixture = OUT / ('fixture_' + name + '_' + uuid.uuid4().hex)
        fixture.mkdir()
        result = subprocess.run([str(OUT / ('build/Release/' + name + '.exe')), str(fixture)], capture_output=True, creationflags=NO_WINDOW, timeout=30,
                                env=dict(os.environ, QT_QPA_PLATFORM='offscreen'))
        (OUT / (name + '.log')).write_bytes(result.stdout + result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        report = json.loads(result.stdout.decode().splitlines()[-1])
        (OUT / (name + '.json')).write_text(json.dumps(report, indent=2), encoding='utf-8')
        self.assertTrue(report['passed'], report)

    def test_detector_credential_candidate_failure_is_atomic(self):
        self.probe('detector')

    def test_real_dpapi_atomic_file_and_failed_commit(self):
        self.probe('store')

    def test_subtitle_isolation_failed_backup_and_atomic_restore(self):
        self.probe('cache')


if __name__ == '__main__':
    unittest.main()
