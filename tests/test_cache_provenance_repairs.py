"""R92 diagnostic provenance stays read-only; new writes bind the frozen media."""
import unittest
from tests.native_review_probe import build_and_run
from tests.test_review_metadata_repairs import ROOT,function
OUT=ROOT/'tests/artifacts/fix_review_20261101/stage25'
MAIN=r'''
#include <QSaveFile>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
class SubtitleGenerationService{public:static QJsonObject mediaIdentity(const QString&,double=0.);static QString mediaFingerprint(const QString&,double=0.);static QString mediaIdentitySidecarPath(const QString&);};
MEDIA_FUNCTIONS
HELPERS
QStringList commitPaths(bool workerExecutionAllowed,const QJsonObject&previousCacheHashes,const QJsonObject&mediaIdentity,const QString&path,bool written,bool reused){
struct Request{QString onlineSubtitleCachePath,onlineTranslatedCachePath,ocrSubtitleCachePath,ocrTranslatedCachePath,longAsrSourceCachePath,longAsrTranslatedCachePath,enhancedTranslatedVttPath,repairTranslatedVttPath;}request;request.ocrSubtitleCachePath=path;
QJsonObject onlineWorkerResult,onlineTranslationResult,longAsrWorkerResult,repairWorkerResult;QJsonObject ocrWorkerResult{{"ocrCacheWritten",written},{"reusedExistingCache",reused}};struct{bool success=false;}manualFusionResult;
COMMIT_BRANCH
return {};}
int main(int argc,char**argv){QCoreApplication app(argc,argv);QDir dir(QString::fromUtf8(argv[1]));QJsonObject result;
auto write=[](QString p,QByteArray b){QFile f(p);return f.open(QIODevice::WriteOnly)&&f.write(b)==b.size();};auto read=[](QString p){QFile f(p);f.open(QIODevice::ReadOnly);return f.readAll();};
const QString media=dir.filePath("media.bin"),cache=dir.filePath("cache.vtt"),sidecar=SubtitleGenerationService::mediaIdentitySidecarPath(cache);write(media,"media snapshot");write(cache,"old cache");write(sidecar,"{\"createdAt\":\"sentinel\",\"durationMs\":9000}");const auto identity=SubtitleGenerationService::mediaIdentity(media);auto hashes=cacheContentHashes({cache});auto old=read(sidecar);
write(cache,"new cache");commitPaths(false,hashes,identity,cache,true,false);result["diagnosticNeverWrites"]=read(sidecar)==old;
commitPaths(true,hashes,identity,cache,true,true);result["reusedCacheNeverResigned"]=read(sidecar)==old;
commitPaths(true,hashes,identity,cache,false,false);result["failedWorkerNeverResigned"]=read(sidecar)==old;
commitPaths(true,hashes,identity,cache,true,false);auto payload=QJsonDocument::fromJson(read(sidecar)).object();result["newCacheBoundToFrozenIdentity"]=payload["mediaFingerprint"]==identity["mediaFingerprint"]&&payload["cacheContentSha256"]==cacheContentHashes({cache})[cache];
old=read(sidecar);hashes=cacheContentHashes({cache});commitPaths(true,hashes,identity,cache,true,false);result["unchangedKeepsMetadata"]=read(sidecar)==old;
write(cache,"another version");write(media,"different media changed size");commitPaths(true,hashes,identity,cache,true,false);result["mediaChangedDuringWorkerNotResigned"]=read(sidecar)==old;
QFile::remove(cache);commitGeneratedCacheMediaIdentities({cache},hashes,SubtitleGenerationService::mediaIdentity(media));result["missingFileKeepsSidecar"]=read(sidecar)==old;
write(cache,"valid newly written");hashes=QJsonObject{{cache,""}};HANDLE lock=CreateFileW(reinterpret_cast<LPCWSTR>(sidecar.utf16()),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);commitGeneratedCacheMediaIdentities({cache},hashes,SubtitleGenerationService::mediaIdentity(media));result["commitFailurePreservesOldIdentity"]=lock!=INVALID_HANDLE_VALUE&&read(sidecar)==old;if(lock!=INVALID_HANDLE_VALUE)CloseHandle(lock);
return finish(result);}
'''
class CacheProvenanceRepairs(unittest.TestCase):
 def test_native_provenance(self):
    source=(ROOT/'src/services/ai/SubtitleGenerationService.cpp').read_text(encoding='utf-8-sig');methods='\n'.join(function(source,signature) for signature in ['QJsonObject SubtitleGenerationService::mediaIdentity(', 'QString SubtitleGenerationService::mediaFingerprint(', 'QString SubtitleGenerationService::mediaIdentitySidecarPath('])
    source=(ROOT/'src/services/ai/TranslationEnhancementSupport.cpp').read_text(encoding='utf-8-sig');helpers='\n'.join(function(source,signature) for signature in ['QJsonObject cacheContentHashes(', 'void commitGeneratedCacheMediaIdentities('])
    source=(ROOT/'src/services/ai/TranslationEnhancementScheduler.cpp').read_text(encoding='utf-8-sig');start=source.index('    if (workerExecutionAllowed) {\n        QStringList generatedPaths;');branch=function(source[start:],'if (workerExecutionAllowed)')
    build_and_run(OUT,MAIN.replace('MEDIA_FUNCTIONS',methods).replace('HELPERS',helpers).replace('COMMIT_BRANCH',branch))
if __name__=='__main__': unittest.main()
