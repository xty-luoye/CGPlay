"""Real Qt cache leases, eviction and settings controls, entirely in background."""
from pathlib import Path
import hashlib
import json
import os
import re
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
ARTIFACT = ROOT / 'tests/artifacts/migration_1_0_8_20261025/remaining_optimizations/disk_cache_validation'
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
FILES = ['src/services/media/ThumbnailDiskCache.cpp', 'src/services/media/ThumbnailDiskCache.h',
         'src/features/settings/ThumbnailCacheSettings.cpp', 'src/features/settings/ThumbnailCacheSettings.h',
         'src/core/hwdecode/HardwareDecodeManager.cpp', 'src/core/hwdecode/HardwareDecodeManager.h']

RUNNER = r'''
#include "media/ThumbnailDiskCache.h"
#include "features/settings/ThumbnailCacheSettings.h"
#include "hwdecode/HardwareDecodeManager.h"
#include "settings/api/ISettingsService.h"
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QSaveFile>
#include <QSpinBox>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <cstdio>
#define NOMINMAX
#include <windows.h>
using namespace cgplay;
class MemorySettings final:public ISettingsService {
public:
    QMap<QString,QVariant> values;
    QVariant value(const QString& k,const QVariant& d={})const override{return values.value(k,d);}
    void setValue(const QString& k,const QVariant& v)override{values.insert(k,v);}
    bool contains(const QString& k)const override{return values.contains(k);}
    QStringList allKeys()const override{return values.keys();}
    void remove(const QString& k)override{values.remove(k);}
    void sync()override{}
    QString organization()const override{return "CGPlay";}
    QString application()const override{return "CGPlayAutomation_thumbnail_cache";}
};
int visibleWindows(){int count=0;EnumWindows([](HWND w,LPARAM p)->BOOL{
    DWORD pid=0;GetWindowThreadProcessId(w,&pid);
    if(pid==GetCurrentProcessId()&&IsWindowVisible(w))++*reinterpret_cast<int*>(p);
    return TRUE;},reinterpret_cast<LPARAM>(&count));return count;}
bool write(const QString& path,int bytes=100){QFile f(path);return f.open(QIODevice::WriteOnly)&&f.write(QByteArray(bytes,'x'))==bytes;}
void stamp(const QString& path,int seconds){QFile f(path);if(f.open(QIODevice::ReadOnly))
    f.setFileTime(QDateTime::fromSecsSinceEpoch(1600000000+seconds,Qt::UTC),QFileDevice::FileModificationTime);}
QString entry(const ThumbnailDiskCache::Policy& p,int key){
    return ThumbnailDiskCache::path(p,QByteArray::number(key,16).rightJustified(40,'0'),0);}
bool settle(const std::function<bool()>& condition,int timeout=10000){QElapsedTimer t;t.start();
    while(!condition()&&t.elapsed()<timeout){qApp->processEvents();QThread::msleep(1);}return condition();}
int main(int argc,char**argv){
    QApplication app(argc,argv);app.setApplicationName("CGPlayAutomation_thumbnail_cache");
    app.setProperty("cgplay.automationBackground",true);
    if(argc>1&&QString::fromLocal8Bit(argv[1])=="--lock"){
        auto lease=ThumbnailDiskCache::acquire(QString::fromLocal8Bit(argv[2]));
        if(!lease||!write(QString::fromLocal8Bit(argv[3]),1))return 2;
        return settle([&]{return QFileInfo::exists(QString::fromLocal8Bit(argv[4]));})?0:3;
    }
    const QString mode=QString::fromLocal8Bit(argv[1]);const QDir base(QString::fromLocal8Bit(argv[2]));
    QDir().mkpath(base.path());QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,base.filePath("settings"));
    QJsonArray cases;auto add=[&](const QString& name,bool pass,QJsonObject extra={}){
        extra.insert("name",name);extra.insert("passed",pass);cases.append(extra);};
    ThumbnailDiskCache::Policy p;p.baseDirectory=base.filePath("cache-a");p.quotaBytes=160;
    if(mode=="disk"){
        QString error;add("prepare owned namespace",ThumbnailDiskCache::prepare(p,&error),{{"error",error}});
        const QString a=entry(p,0),b=entry(p,1),c=entry(p,2);
        write(a,80);write(b,80);write(c,80);stamp(a,0);stamp(b,1);stamp(c,2);
        auto stats=ThumbnailDiskCache::maintain(p);
        add("quota evicts oldest",!QFileInfo::exists(a)&&QFileInfo::exists(b)&&QFileInfo::exists(c)&&stats.bytes==160&&stats.removedFiles==1);
        write(a,80);stamp(a,0);stamp(b,1);stamp(c,2);ThumbnailDiskCache::touch(a);
        stats=ThumbnailDiskCache::maintain(p);
        add("hit updates LRU",QFileInfo::exists(a)&&!QFileInfo::exists(b)&&QFileInfo::exists(c)&&stats.bytes==160);
        auto lease=ThumbnailDiskCache::acquire(a);auto shared=ThumbnailDiskCache::acquire(a);
        stats=ThumbnailDiskCache::maintain(p,true);
        add("active shared lease retained",lease&&shared&&lease==shared&&QFileInfo::exists(a)&&!QFileInfo::exists(c)&&stats.activeFiles==1&&stats.bytes==80);
        lease.reset();shared.reset();stats=ThumbnailDiskCache::maintain(p,true);
        add("released lease can be cleared",!QFileInfo::exists(a)&&stats.bytes==0);
        const QString unknown=QDir(ThumbnailDiskCache::namespaceDirectory(p)).filePath("precious.jpg");
        const QString stage=QDir(ThumbnailDiskCache::namespaceDirectory(p)).filePath(".cgplay-thumb-private.jpg");
        const QString outside=base.filePath("user-video.mp4");write(unknown,127);write(stage,129);write(outside,131);
        write(a,80);stats=ThumbnailDiskCache::maintain(p,true);
        add("never deletes user or staging files",QFileInfo(unknown).size()==127&&QFileInfo(stage).size()==129&&QFileInfo(outside).size()==131&&!QFileInfo::exists(a));
        ThumbnailDiskCache::Policy invalid=p;invalid.baseDirectory=base.filePath("unknown-base");
        const QString unknownRoot=ThumbnailDiskCache::namespaceDirectory(invalid);QDir().mkpath(unknownRoot);
        const QString precious=QDir(unknownRoot).filePath("precious.mov");write(precious,141);
        error.clear();const bool accepted=ThumbnailDiskCache::prepare(invalid,&error);
        add("unknown namespace never adopted",!accepted&&!error.isEmpty()&&QFileInfo(precious).size()==141);
        const QString junctionBase=base.filePath("junction-base");QDir().mkpath(junctionBase);
        const QString junctionRoot=QDir(junctionBase).filePath("cgplay-thumbnails-v1");
        QProcess junction;junction.start("cmd.exe",{"/c","mklink","/J",QDir::toNativeSeparators(junctionRoot),QDir::toNativeSeparators(unknownRoot)});
        junction.waitForFinished(5000);ThumbnailDiskCache::Policy linked=p;linked.baseDirectory=junctionBase;
        add("junction namespace rejected",junction.exitCode()==0&&!ThumbnailDiskCache::prepare(linked)&&QFileInfo(precious).size()==141);
        write(a,80);const QString ready=base.filePath("child-ready"),release=base.filePath("child-release");
        QProcess child;child.start(QCoreApplication::applicationFilePath(),{"--lock",a,ready,release});
        const bool childReady=settle([&]{return QFileInfo::exists(ready);});
        stats=ThumbnailDiskCache::maintain(p,true);const bool childProtected=QFileInfo::exists(a)&&stats.activeFiles==1;
        write(release,1);settle([&]{return child.state()==QProcess::NotRunning;});
        stats=ThumbnailDiskCache::maintain(p,true);
        add("cross process lease retained then released",childReady&&childProtected&&child.exitCode()==0&&!QFileInfo::exists(a));
        write(a,80);auto oldLease=ThumbnailDiskCache::acquire(a);ThumbnailDiskCache::configure(p);
        auto newer=p;newer.baseDirectory=base.filePath("cache-b");ThumbnailDiskCache::configure(newer);
        const QString newPath=entry(ThumbnailDiskCache::policy(),8);write(newPath,90);
        stats=ThumbnailDiskCache::maintain(p,true);
        add("root switch preserves active old snapshot",oldLease&&QFileInfo::exists(a)&&newPath!=a&&QFileInfo(newPath).size()==90&&stats.activeFiles==1);
        oldLease.reset();stats=ThumbnailDiskCache::maintain(p,true,[]{return true;});
        add("cancellation preserves files",stats.canceled&&QFileInfo::exists(a));
        add("invalid names rejected",ThumbnailDiskCache::path(p,"../bad",0).isEmpty()&&ThumbnailDiskCache::path(p,QByteArray(40,'a'),-1).isEmpty());
        int polls=0;bool replaced=false;
        stats=ThumbnailDiskCache::maintain(p,true,[&]{
            if(++polls==2){QSaveFile writer(a);replaced=writer.open(QIODevice::WriteOnly)&&writer.write(QByteArray(173,'y'))==173&&writer.commit();}
            return false;});
        add("clear reports replacement writer current bytes",replaced&&stats.removedBytes==173&&stats.bytes==0&&!QFileInfo::exists(a));
    }else if(mode=="hardware"){
        {QSettings settings("CGPlay",app.applicationName());settings.setValue("hwdecode/type",static_cast<int>(HwAccelType::NVDEC));}
        {HardwareDecodeManager hw;hw.probe();
            add("saved request distinct from actual backend",hw.hwAccelType()==HwAccelType::NVDEC&&!hw.isAvailable()&&!hw.isEnabled()&&
                hw.currentAccelName()==hwAccelName(HwAccelType::None)&&hw.currentAccelFFmpeg().isEmpty()&&!hw.fallbackReason().isEmpty());
            bool supported=hw.availableAccelTypes()==QStringList{hwAccelName(HwAccelType::None)};
            for(int i=1;i<=static_cast<int>(HwAccelType::Auto);++i)supported=supported&&!hw.supportsAccelType(static_cast<HwAccelType>(i));
            for(const auto& gpu:hw.gpus())supported=supported&&!gpu.supportsNvdec&&!gpu.supportsAmf&&!gpu.supportsQuickSync&&!gpu.supportsD3d11va&&!gpu.supportsDxva2;
            add("GPU name never proves decoder support",supported);
            hw.setHwAccelType(static_cast<HwAccelType>(99));add("invalid enum rejected",hw.hwAccelType()==HwAccelType::NVDEC);
            hw.setHwAccelType(HwAccelType::None);add("software request explicit",hw.fallbackReason().isEmpty()&&!hw.isEnabled());}
        {HardwareDecodeManager reloaded;add("isolated request persisted",reloaded.hwAccelType()==HwAccelType::None);}
    }else if(mode=="ui"){
        const int font=QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc");
        if(font>=0)app.setFont(QFont(QFontDatabase::applicationFontFamilies(font).first(),10));
        auto settings=std::make_shared<MemorySettings>();p.quotaBytes=2ULL*1024*1024*1024;
        ThumbnailDiskCache::configure(p);auto* widget=new ThumbnailCacheSettings(settings);
        widget->setAttribute(Qt::WA_DontShowOnScreen);widget->setAttribute(Qt::WA_ShowWithoutActivating);widget->resize(680,300);widget->show();
        int beats=0;QElapsedTimer last;last.start();qint64 maxGap=0;QTimer heartbeat;
        QObject::connect(&heartbeat,&QTimer::timeout,&app,[&]{maxGap=std::max(maxGap,last.restart());++beats;});heartbeat.start(1);
        auto idle=[&]{return !widget->property("cgplay.thumbnailCacheBusy").toBool();};
        const bool initial=settle(idle);maxGap=0;last.restart();auto* quota=widget->findChild<QSpinBox*>("thumbnailCacheQuotaGB");
        auto* choose=widget->findChild<QPushButton*>("thumbnailCacheChooseDirectory");auto* refresh=widget->findChild<QPushButton*>("thumbnailCacheRefresh");
        auto* clear=widget->findChild<QPushButton*>("thumbnailCacheClear");auto* usage=widget->findChild<QLabel*>("thumbnailCacheUsage");
        add("controls initialized asynchronously",initial&&quota&&choose&&refresh&&clear&&usage&&usage->text().contains("已占用"));
        quota->setValue(3);QMetaObject::invokeMethod(quota,"editingFinished",Qt::DirectConnection);settle(idle);
        add("quota control persists",settings->value("cache/thumbnails/quotaBytes").toULongLong()==3ULL*1024*1024*1024);
        const QString oldPath=entry(ThumbnailDiskCache::policy(),5);write(oldPath,111);
        const QString chosen=base.filePath("chosen-location");widget->setProperty("cgplay.thumbnailCacheSelectedDirectory",chosen);choose->click();settle(idle);
        add("location uses deterministic selection without deleting old cache",settings->value("cache/thumbnails/baseDirectory").toString()==QFileInfo(chosen).canonicalFilePath()&&QFileInfo(oldPath).size()==111);
        const QString item=entry(ThumbnailDiskCache::policy(),7);write(item,130);refresh->click();settle(idle);
        add("refresh reports actual file count",usage->text().contains("1 张")&&QFileInfo::exists(item));
        const QString user=QDir(chosen).filePath("user.jpg");write(user,151);clear->click();settle(idle);
        add("clear control only deletes owned cache",!QFileInfo::exists(item)&&QFileInfo(user).size()==151&&usage->text().contains("本次清理"));
        heartbeat.stop();const QString picture=base.filePath("thumbnail_cache_settings.png");add("background settings capture",widget->grab().save(picture),{{"capture",picture}});
        last.restart();heartbeat.start(1);refresh->click();delete widget;
        const bool drained=settle([]{return QThreadPool::globalInstance()->activeThreadCount()==0;});
        heartbeat.stop();app.processEvents();
        add("close cancels data-only worker safely",drained&&beats>0&&maxGap<150,{{"heartbeats",beats},{"maxHeartbeatGapMs",maxGap},{"workersDrained",drained}});
    }else return 4;
    QThreadPool::globalInstance()->waitForDone();const int visible=visibleWindows();bool passed=visible==0;
    for(const auto& value:cases)passed=passed&&value.toObject()["passed"].toBool();
    QJsonObject report{{"mode",mode},{"cases",cases},{"backgroundMode",qEnvironmentVariable("QT_QPA_PLATFORM")=="offscreen"},
        {"visiblePlatformWindows",visible},{"passed",passed&&qEnvironmentVariable("QT_QPA_PLATFORM")=="offscreen"}};
    std::puts(QJsonDocument(report).toJson(QJsonDocument::Compact).constData());return report["passed"].toBool()?0:1;
}
'''


def _build():
    ARTIFACT.mkdir(parents=True, exist_ok=True)
    source, build = ARTIFACT / 'source', ARTIFACT / 'build'
    source.mkdir(exist_ok=True)
    cache = (ROOT / 'build_win_full/CMakeCache.txt').read_text(encoding='utf-8')
    qt = Path(re.search(r'^Qt6_DIR:[^=]*=(.+)$', cache, re.MULTILINE).group(1).strip()).parents[2]
    digest = hashlib.sha256(RUNNER.encode('utf-8'))
    for file in FILES:
        digest.update((ROOT / file).read_bytes())
    stamp = build / 'source.sha256'
    exe = build / 'Release/thumbnail_cache_contracts.exe'
    if not exe.exists() or not stamp.exists() or stamp.read_text() != digest.hexdigest():
        (source / 'runner.cpp').write_text(RUNNER, encoding='utf-8')
        cmake = 'cmake_minimum_required(VERSION 3.21)\nproject(ThumbnailCacheContracts LANGUAGES CXX)\nset(CMAKE_CXX_STANDARD 17)\nset(CMAKE_AUTOMOC ON)\nfind_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Concurrent)\n'
        cmake += 'add_executable(thumbnail_cache_contracts runner.cpp ' + ' '.join('"' + (ROOT / f).as_posix() + '"' for f in FILES) + ')\n'
        cmake += 'target_include_directories(thumbnail_cache_contracts PRIVATE ' + ' '.join('"' + (ROOT / f).as_posix() + '"' for f in ['src', 'src/common', 'src/services', 'src/core']) + ')\n'
        cmake += 'target_compile_options(thumbnail_cache_contracts PRIVATE /utf-8)\ntarget_link_libraries(thumbnail_cache_contracts PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Concurrent user32)\n'
        (source / 'CMakeLists.txt').write_text(cmake, encoding='utf-8')
        for label, args in [('configure', ['cmake', '-S', str(source), '-B', str(build), '-G', 'Visual Studio 17 2022', '-A', 'x64', '-DCMAKE_PREFIX_PATH=' + qt.as_posix()]),
                            ('build', ['cmake', '--build', str(build), '--config', 'Release', '--parallel', '2'])]:
            result = subprocess.run(args, capture_output=True, creationflags=NO_WINDOW, timeout=180)
            (ARTIFACT / (label + '.log')).write_bytes(result.stdout + result.stderr)
            if result.returncode:
                raise AssertionError((result.stdout + result.stderr)[-7000:])
        stamp.write_text(digest.hexdigest())
    return exe, qt


def run_native(mode):
    exe, qt = _build()
    run = ARTIFACT / f'{mode}_{os.getpid()}'
    run.mkdir(exist_ok=True)
    environment = os.environ.copy()
    environment.update(QT_QPA_PLATFORM='offscreen', PATH=str(qt / 'bin') + ';' + environment.get('PATH', ''))
    result = subprocess.run([str(exe), mode, str(run)], env=environment, capture_output=True, creationflags=NO_WINDOW, timeout=50)
    (run / 'runtime.log').write_bytes(result.stdout + result.stderr)
    if not result.stdout:
        raise AssertionError((result.returncode, result.stderr[-5000:]))
    report = json.loads(result.stdout.decode('utf-8').strip().splitlines()[-1])
    (run / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    if result.returncode or not report['passed']:
        raise AssertionError(report)
    return report


class ThumbnailDiskCacheTests(unittest.TestCase):
    def test_real_quota_leases_and_owned_paths(self):
        report = run_native('disk')
        self.assertGreaterEqual(len(report['cases']), 10)
        self.assertTrue(report['backgroundMode'])
        self.assertEqual(0, report['visiblePlatformWindows'])

    def test_real_settings_controls_and_close(self):
        report = run_native('ui')
        self.assertEqual(7, len(report['cases']))
        self.assertTrue(report['backgroundMode'])
        self.assertEqual(0, report['visiblePlatformWindows'])


if __name__ == '__main__':
    unittest.main()
