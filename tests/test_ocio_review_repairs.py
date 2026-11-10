"""Native OCIO/session/dialog contracts; run through run_hidden.py."""
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'tests/artifacts/fix_review_20261101/ocio_regression'
REFERENCE = ROOT / 'tests/artifacts/review_full_20261031/phase21'
QT = Path('E:/rjkf/CGPlay-deps/engine_migration_1_0_8/qt-6.11.3-msvc2022_64')


def run(command, name, env=None):
    with (OUT / (name + '.log')).open('w', encoding='utf-8') as log:
        subprocess.run([str(arg) for arg in command], check=True, env=env, stdout=log, stderr=subprocess.STDOUT,
                       creationflags=subprocess.CREATE_NO_WINDOW, timeout=240)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for name in ['configA.ocio', 'configB.ocio']:
        shutil.copyfile(REFERENCE / name, OUT / name)
    session = (ROOT / 'src/core/session/SessionManager.cpp').read_text(encoding='utf-8')
    start = session.index('bool SessionManager::_deserialize(')
    block = session[start:session.index('    // ── Playback', start)]
    block = block.replace('SessionManager::_deserialize', 'SessionHarness::restore') + '    return true;\n}\n'
    source = r'''
#include "ocio/OcioManager.h"
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QComboBox>
#include <QLineEdit>
#include <QLabel>
#include <QTimer>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QDebug>
#include <QFont>
#include <memory>
using cgplay::OcioManager;
class SessionHarness:public QObject {
public:std::shared_ptr<OcioManager> _ocioMgr=std::make_shared<OcioManager>();
bool restore(const QJsonObject&,QString*);
};
''' + block + r'''
int main(int argc,char**argv){
QApplication app(argc,argv);app.setFont(QFont("Microsoft YaHei",9));QDir dir(QString::fromLocal8Bit(argv[1]));
QJsonObject results;int checks=0;
auto check=[&](bool ok,const char* name){results[name]=ok;++checks;if(!ok)qFatal("Failed: %s",name);};
SessionHarness session;auto& m=*session._ocioMgr;
const QString saved="ocio://studio-config-v1.0.0_aces-v1.3_ocio-v2.1";
QString error;
check(m.applyConfiguration({true,saved,"scene_linear",{},{}},&error),"initialNativeConfig");
auto original=m.previewTransformSettings();
QImage before(2,2,QImage::Format_RGB32);before.fill(QColor(128,64,32));
check(OcioManager::applyPreviewTransform(before,original),"initialProcessor");
QJsonObject root{{"version",1},{"ocio",QJsonObject{{"config_path",saved},{"input",original.input},
 {"display",original.display},{"view",original.view},{"enabled",true}}}};
m.loadBuiltinConfig("ACES");
check(session.restore(root,&error),"actualSessionRestoreBlock");
QImage after(2,2,QImage::Format_RGB32);after.fill(QColor(128,64,32));
check(m.currentConfigPath()==saved && OcioManager::applyPreviewTransform(after,m.previewTransformSettings()) && before==after,"exactIdentityAndPixels");
int optionEvents=0;QObject::connect(&m,&OcioManager::optionsChanged,&app,[&]{++optionEvents;});
for(const auto& field:{"config_path","input","display","view"}){
 auto bad=root;auto color=bad["ocio"].toObject();color[field]="missing-value";bad["ocio"]=color;
 int count=optionEvents;error.clear();check(!session.restore(bad,&error)&&!error.isEmpty()&&m.currentConfigPath()==saved&&m.currentInput()==original.input&&m.currentView()==original.view&&m.isEnabled()&&optionEvents==count,field);
}
m.loadConfig(dir.filePath("missing.ocio"));
check(m.currentConfigPath()==saved && m.isEnabled(),"legacyLoadFailurePreservesState");
m.loadConfig(dir.filePath("configA.ocio"));m.setEnabled(true);
auto dialogAction=[&](const QString& path,bool accept,bool expectFailure,const QString& shot){
 QTimer::singleShot(0,&app,[&,path,accept,expectFailure,shot]{
  auto* dlg=qobject_cast<QDialog*>(QApplication::activeModalWidget());if(!dlg)qFatal("Missing dialog");
  auto* edit=dlg->findChild<QLineEdit*>("ocioConfigPath");edit->setText(path);
  if(path.endsWith("configB.ocio"))check(dlg->findChild<QComboBox*>("ocioInput")->currentText()=="LinearB"&&dlg->findChild<QComboBox*>("ocioView")->currentText()=="ViewB","dialogNamesRefreshed");
  if(!shot.isEmpty())check(dlg->grab().save(dir.filePath(shot)),"screenshotSaved");
  if(accept)dlg->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
  if(expectFailure){check(dlg->isVisible()&&!dlg->findChild<QLabel*>("ocioConfigError")->text().isEmpty(),"dialogFailureDisplayed");dlg->reject();}
  else if(!accept)dlg->reject();
 });m.showSettings();
};
int count=optionEvents;
dialogAction(dir.filePath("configB.ocio"),false,false,"ocio_candidate.png");
check(m.currentInput()=="LinearA"&&optionEvents==count,"dialogCancelPreservesState");
dialogAction(dir.filePath("missing.ocio"),true,true,{});
check(m.currentInput()=="LinearA"&&optionEvents==count,"dialogFailurePreservesState");
dialogAction(dir.filePath("configB.ocio"),true,false,{});
check(m.currentConfigPath()==dir.filePath("configB.ocio")&&m.currentInput()=="LinearB"&&m.currentView()=="ViewB"&&m.isEnabled(),"dialogSwitchCommitted");
check(OcioManager::applyPreviewTransform(after,m.previewTransformSettings()),"switchedProcessorValid");
results["passed"]=true;results["checks"]=checks;results["backgroundMode"]=true;
QFile output(dir.filePath("result.json"));if(!output.open(QIODevice::WriteOnly))return 2;
output.write(QJsonDocument(results).toJson());return 0;}
'''
    (OUT / 'probe.cpp').write_text(source, encoding='utf-8')
    (OUT / 'CMakeLists.txt').write_text((REFERENCE / 'CMakeLists.txt').read_text(encoding='utf-8'), encoding='utf-8')
    run(['cmake','-S',OUT,'-B',OUT/'build','-G','Visual Studio 17 2022','-A','x64'], 'configure')
    run(['cmake','--build',OUT/'build','--config','Release','--parallel','4'], 'build')
    target=OUT/'build/Release'
    for folder in [REFERENCE/'build/Release', QT/'bin']:
        for dll in folder.glob('*.dll'):
            dest=target/dll.name
            if not dest.exists():os.link(dll,dest)
    env=os.environ.copy();env.update(QT_QPA_PLATFORM='offscreen',QT_QPA_PLATFORM_PLUGIN_PATH=str(QT/'plugins/platforms'),
        QT_PLUGIN_PATH=str(QT/'plugins'),QT_QPA_FONTDIR='C:/Windows/Fonts')
    run([target/'native_probe.exe',OUT],'native_result',env)
    result=json.loads((OUT/'result.json').read_text(encoding='utf-8'))
    assert result['passed'] and result['checks']>=15
    print(json.dumps(result,ensure_ascii=False))


if __name__=='__main__':main()
