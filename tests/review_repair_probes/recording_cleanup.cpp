#include "recording/RecordingService.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTimer>
#include <cstdio>
#include <memory>
using namespace cgplay;
int main(int argc,char**argv){QCoreApplication app(argc,argv);app.setProperty("cgplay.automationBackground",true);app.setProperty("cgplay.recordingSmoke",true);
 QString root=QString::fromUtf8(argv[1]);QDir().mkpath(root);app.setProperty("cgplay.recordingArtifactRoot",root);QString target=QDir(root).filePath("existing.mp4");QFile existing(target);existing.open(QIODevice::WriteOnly);existing.write("original");existing.close();
 RecordingStartRequest request;request.synthetic=true;request.finalOutputPath=target;request.helperPlan={{"source","synthetic"},{"width",320},{"height",180},{"fps",30}};
 auto& r=request.runtime;r.recorderRoot=QString::fromUtf8(argv[2]);r.helperExecutable=QDir(r.recorderRoot).filePath("CGPlayRecorder.exe");r.pythonExecutable=QString::fromUtf8(argv[3]);r.pythonHome=QFileInfo(r.pythonExecutable).absolutePath();r.toolsRoot=QString::fromUtf8(argv[4]);r.finalizerScript=QDir(r.toolsRoot).filePath("cgplay/export/recording_finalize.py");r.ffmpegExecutable=QString::fromUtf8(argv[5]);r.ffprobeExecutable=QString::fromUtf8(argv[6]);
 auto service=std::make_unique<RecordingService>();int finished=0;QJsonArray outcomes;QString error;
 auto start=[&]{if(!service->start(request,&error)) app.exit(2);};
 QObject::connect(service.get(),&RecordingService::finished,&app,[&](const QJsonObject& v){outcomes.append(v);if(++finished==3)app.quit();else QTimer::singleShot(0,&app,start);});QTimer::singleShot(0,&app,start);QTimer::singleShot(20000,&app,[&]{app.exit(3);});int code=app.exec();service.reset();
 const auto dirs=QDir(root).entryList({"cgplay-recording-control-*"},QDir::Dirs|QDir::NoDotAndDotDot);QJsonArray kept;for(const auto& d:dirs)kept.append(QDir(root).filePath(d+"/request.json"));existing.open(QIODevice::ReadOnly);bool unchanged=existing.readAll()=="original";
 QJsonObject result{{"threePrepareFailures",finished==3},{"noControlDirectoriesRemain",kept.isEmpty()},{"existingOutputPreserved",unchanged}};return code?code:finish(result);}
