"""R27/R29/R64/R79: actual queue/search/line parser and real Windows file handles."""
import unittest
from tests.native_review_probe import build_and_run
from tests.test_review_metadata_repairs import ROOT

OUT = ROOT / 'tests/artifacts/fix_review_20261101/stage19'
MAIN = r'''
#include "ai/AIProviderManager.h"
#include "ai/AIWorkflowService.h"
#include "annotation/api/IAnnotationService.h"
#include "common/core/ServiceLocator.h"
#include "services/agent/CodexAppServerSession.h"
#include <QThread>
#include <QElapsedTimer>
using namespace cgplay;
struct EmitTag {using type=void(CodexAppServerSession::*)(QByteArray*,bool,bool);friend type access(EmitTag);};
template<class Tag,typename Tag::type member>struct ProbeAccess {friend typename Tag::type access(Tag){return member;}};
template struct ProbeAccess<EmitTag,&CodexAppServerSession::emitLines>;
struct Annotations:IAnnotationService{QVector<AnnotationItem>items;int count()const override{return items.size();}QVector<AnnotationItem>annotations()const override{return items;}
void replaceAnnotations(const QVector<AnnotationItem>&v)override{items=v;}void clearAnnotations()override{items.clear();}
bool undo()override{return false;}bool redo()override{return false;}QString addAnnotation(const AnnotationItem&)override{return {};}
bool removeAnnotation(const QString&)override{return false;}bool removeSelectedAnnotation()override{return false;}bool updateAnnotationComment(const QString&,const QString&)override{return false;}
QString createNote(const QString&)override{return {};}void selectAnnotation(const QString&)override{}QString selectedAnnotationId()const override{return {};}
void setTool(int)override{}int currentTool()const override{return 0;}void setToolColor(const QColor&)override{}QColor currentToolColor()const override{return {};}};
struct Provider:IAIProvider{bool succeeds=true;QString providerId()const override{return "fixture";}QString providerName()const override{return "fixture";}bool isAvailable()const override{return true;}AIProviderCapabilities capabilities()const override{return {};}
AIResponse chat(const AIRequest&r)override{QThread::msleep(30);AIResponse v;v.jobId=r.jobId;v.success=succeeds;return v;}AIAudioTranscriptionResult transcribe(const AIAudioTranscriptionRequest&)override{return {};}};
TRANSCRIBE
int main(int argc,char**argv){QCoreApplication app(argc,argv);QJsonObject report;
Annotations annotations;AnnotationItem a;a.id="open";a.frame=5;a.comment=QString::fromUtf8("调整色彩");AnnotationItem b=a;b.id="closed";b.status=ReviewStatus::Resolved;annotations.items={a,b};
ServiceLocator::registerService<IAnnotationService>(&annotations);AIWorkflowService workflow(nullptr,nullptr,nullptr);
report["statusOnlySearchFindsOpenNote"]=workflow.searchAnnotations(QString::fromUtf8("未解决")).size()==1;
report["statusAndTextSearch"]=workflow.searchAnnotations(QString::fromUtf8("未解决 色彩")).size()==1&&workflow.searchAnnotations(QString::fromUtf8("未解决 灯光")).isEmpty();
report["plainTextAndEnglishStatus"]=workflow.searchAnnotations(QString::fromUtf8("色彩")).size()==2&&workflow.searchAnnotations("open").size()==1;
AIProviderManager manager;AIRequest request;request.providerId="missing";AIMediaFrameReference frame;frame.frame=42;frame.size={1280,720};frame.encodedBytes=QByteArray(1024*1024,'x');request.frames={frame};request.chatHistory={{}};request.context.annotations={a};
bool released=true;for(int i=0;i<32;++i){auto id=manager.submit(request);auto snapshot=manager.jobSnapshot(id);released=released&&snapshot.state==AIJobState::Failed&&snapshot.request.frames[0].encodedBytes.isEmpty()&&snapshot.request.chatHistory.isEmpty()&&snapshot.request.context.annotations.isEmpty()&&snapshot.request.frames[0].frame==42&&snapshot.request.frames[0].size==frame.size;}
report["failedJobsReleaseLargePayloadKeepIdentity"]=released;
auto provider=std::make_shared<Provider>();manager.registerProvider(provider);request.providerId="fixture";
auto wait=[&](QString id){QElapsedTimer t;t.start();while(t.elapsed()<3000){auto s=manager.jobSnapshot(id);if(s.state==AIJobState::Failed||s.state==AIJobState::Succeeded)return s;QCoreApplication::processEvents();QThread::msleep(1);}return manager.jobSnapshot(id);};
auto successful=wait(manager.submit(request));report["successfulJobReleasesPayload"]=successful.state==AIJobState::Succeeded&&successful.request.frames[0].encodedBytes.isEmpty();
provider->succeeds=false;auto failure=wait(manager.submit(request));report["workerFailureReleasesPayload"]=failure.state==AIJobState::Failed&&failure.request.frames[0].encodedBytes.isEmpty();
auto id=manager.submit(request);manager.cancel(id);report["cancelReleasesSnapshotPayload"]=manager.jobSnapshot(id).request.frames[0].encodedBytes.isEmpty();QThread::msleep(60);QCoreApplication::processEvents();
CodexAppServerSession session;int lines=0,errors=0;QObject::connect(&session,&CodexAppServerSession::jsonRpcLineReceived,[&](const QByteArray&){++lines;});QObject::connect(&session,&CodexAppServerSession::standardErrorLineReceived,[&](const QByteArray&){++lines;});QObject::connect(&session,&CodexAppServerSession::sessionError,[&](const QString&){++errors;});
constexpr int limit=4*1024*1024;for(bool stderrStream:{false,true}){QByteArray buffer("{}\r\n{}\n");int before=lines;(session.*access(EmitTag{}))(&buffer,stderrStream,false);report[QString::number(stderrStream)+"NormalLines"]=lines==before+2&&buffer.isEmpty();
before=lines;int beforeErrors=errors;buffer=QByteArray(limit,'x')+'\n';(session.*access(EmitTag{}))(&buffer,stderrStream,false);report[QString::number(stderrStream)+"ExactLimitAccepted"]=lines==before+1&&errors==beforeErrors;
before=lines;buffer=QByteArray(limit+1,'x')+'\n';(session.*access(EmitTag{}))(&buffer,stderrStream,false);report[QString::number(stderrStream)+"CompletedOversizeRejected"]=lines==before&&errors==beforeErrors+1&&buffer.isEmpty();
beforeErrors=errors;buffer=QByteArray(limit+1,'x');(session.*access(EmitTag{}))(&buffer,stderrStream,true);report[QString::number(stderrStream)+"UnfinishedOversizeRejected"]=errors==beforeErrors+1&&buffer.isEmpty();}
const QString path=QDir(QString::fromUtf8(argv[1])).filePath("audio.fixture");{QFile file(path);file.open(QIODevice::WriteOnly);file.write("fixture");}
FixtureTranscriber transcriber;AIAudioTranscriptionRequest audio;audio.audioFilePath=path;auto warmup=transcriber.transcribe(audio);DWORD before=0,after=0;GetProcessHandleCount(GetCurrentProcess(),&before);bool failed=true;
for(int i=0;i<64;++i){auto result=transcriber.transcribe(audio);failed=failed&&!result.success&&!result.errorMessage.isEmpty();}GetProcessHandleCount(GetCurrentProcess(),&after);
report["missingKeyDoesNotLeakAudioHandles"]=failed&&after<=before;report["handleGrowth"]=int(after)-int(before);return finish(report);}
'''


class AiResourceBoundaryRepairs(unittest.TestCase):
    def test_actual_paths(self):
        source = (ROOT / 'src/services/ai/OpenAIResponsesProvider.cpp').read_text(encoding='utf-8-sig')
        start = source.index('AIAudioTranscriptionResult OpenAIResponsesProvider::transcribe(')
        end = source.index('    const QString resolvedModel = _transcriptionModelForRequest(request);', start)
        prefix = source[start:end].replace('OpenAIResponsesProvider::', 'FixtureTranscriber::') + 'return result;\n}\n'
        transcribe = r'''
bool looksLikeOllamaBaseUrl(const QString&){return false;}
class FixtureTranscriber{public:QString providerId(){return "fixture";}QString _baseUrl(){return "https://fixture.invalid";}
QByteArray _apiKey(const QString&,const QString&,QString*){return {};}
AIAudioTranscriptionResult transcribe(const AIAudioTranscriptionRequest&);};
''' + prefix
        build_and_run(OUT, MAIN.replace('TRANSCRIBE', transcribe), sources=[
            'src/services/ai/AIProviderManager.cpp', 'src/services/ai/AIProviderManager.h',
            'src/services/ai/AIWorkflowService.cpp', 'src/services/agent/CodexAppServerSession.cpp',
            'src/services/agent/CodexAppServerSession.h', 'src/common/jobs/JobSystem.cpp', 'src/common/jobs/JobSystem.h'])


if __name__ == '__main__':
    unittest.main()
