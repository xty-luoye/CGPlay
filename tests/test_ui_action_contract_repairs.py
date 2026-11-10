"""R80/R83/R90/R96/R101: hidden native states and extracted production action contracts."""
import unittest
from tests.native_review_probe import build_and_run
from tests.test_review_metadata_repairs import ROOT, function

OUT = ROOT / 'tests/artifacts/fix_review_20261101/stage21'
CODE = r'''
#include <QApplication>
#include <QMainWindow>
#include <QDockWidget>
#include <QAction>
#include <QMap>
#include <QHash>
#include <QVariant>
#include <QComboBox>
#include <QStatusBar>
#include <QToolButton>
#include <QSignalBlocker>
#include <windowsx.h>
#include "ui/app/PlaybackBar.h"
HELPERS
struct InputBindingStore{enum class Device{Mouse,Gamepad};QString commandFor(Device,const QString&g){return g=="Mouse4"?"playback.nextFrame":"playback.prevFrame";}};
class ProbeWindow:public QMainWindow{public:struct Private{InputBindingStore*inputBindings;}state;Private*_p=&state;InputBindingStore bindings;int calls=0;
ProbeWindow(){state.inputBindings=&bindings;setAttribute(Qt::WA_DontShowOnScreen);}bool _executeCommandId(QString){++calls;return true;}bool nativeEvent(const QByteArray&,void*,qintptr*)override;};
NATIVE_EVENT
class BindingPreflight:public QMainWindow{public:struct Editor{InputBindingStore::Device device;QString gesture;QComboBox*combo;};QVector<Editor>editors;QVector<Editor>*bindingEditors=&editors;bool accepted=false;
void validate(){accepted=false;
PREFLIGHT
accepted=true;}};
struct Workspace:QObject{void appendLogLine(QString){}void appendWorkbenchEvent(QString,QString){}};
QByteArray redactSensitiveText(QByteArray value){return value;}
class CodexPlugin:public QObject{public:
enum class PendingRequest{McpServerStatus,SkillsList,PluginList,AppList};Workspace workspace;Workspace*_workspace=&workspace;QJsonArray _mcpServers;QMap<int,PendingRequest>pending;QMap<int,QString>methods;int next=0;bool allow=true;
bool sendRequest(PendingRequest kind,QString method,QJsonObject,QString*){if(!allow)return false;pending[++next]=kind;methods[next]=method;return true;}
bool refreshNativeCatalog(QString*);bool listSkills(bool,QString*);
void respond(int id,QJsonObject result){auto kind=pending.take(id);switch(kind){
CATALOG_RESPONSE
}}};
CATALOG_METHODS
struct CompletionFixture{cgplay::PlaybackBar bar{nullptr};struct Private{cgplay::PlaybackBar*playbackBar;}privateState{&bar};Private*_p=&privateState;bool applied=false;
void finish(){
COMPLETION_GUARD
if(_p->playbackBar)_p->playbackBar->setTranslationVisible(true);applied=true;}};
int main(int argc,char**argv){QApplication app(argc,argv);QJsonObject report;
QMainWindow win;win.setAttribute(Qt::WA_DontShowOnScreen);QDockWidget dock("AI workspace",&win);win.addDockWidget(Qt::RightDockWidgetArea,&dock);auto*action=dock.toggleViewAction();action->setText("AI workspace");dock.hide();
report["dockShowChangesActualState"]=setWindowActionChecked(&win,"AI workspace",true)&&!dock.isHidden();
report["dockHideChangesActualState"]=setWindowActionChecked(&win,"AI workspace",false)&&dock.isHidden();
auto*ordinary=new QAction("Ordinary",&win);ordinary->setCheckable(true);int toggles=0;QObject::connect(ordinary,&QAction::toggled,[&]{++toggles;});
report["ordinaryTogglePreserved"]=setWindowActionChecked(&win,"Ordinary",true)&&ordinary->isChecked()&&toggles==1;
ProbeWindow input;for(auto state:{Qt::WindowNoState,Qt::WindowMaximized,Qt::WindowFullScreen}){input.setWindowState(state);int before=input.calls;MSG msg{};msg.message=WM_XBUTTONDOWN;msg.wParam=MAKEWPARAM(0,XBUTTON1);qintptr out=0;report[QString("sideButtonState%1").arg(int(state))]=input.nativeEvent("windows_generic_MSG",&msg,&out)&&input.calls==before+1;}
BindingPreflight bindings;QComboBox first,second;for(auto*c:{&first,&second}){c->addItem("None","");c->addItem("Play","playback.toggle");c->addItem("Next","playback.nextFrame");c->setCurrentIndex(1);}
bindings.editors={{InputBindingStore::Device::Mouse,"Mouse4",&first},{InputBindingStore::Device::Mouse,"Mouse5",&second}};bindings.validate();report["duplicateMouseCommandsRejectedBeforeMutation"]=!bindings.accepted;
second.setCurrentIndex(2);bindings.validate();report["differentCommandsAccepted"]=bindings.accepted;
second.setCurrentIndex(1);bindings.editors[1].device=InputBindingStore::Device::Gamepad;bindings.validate();report["sameCommandOnDifferentDevicesAccepted"]=bindings.accepted;
bindings.editors[0].device=InputBindingStore::Device::Gamepad;bindings.validate();report["duplicateGamepadCommandsRejected"]=!bindings.accepted;
first.setCurrentIndex(0);second.setCurrentIndex(0);bindings.validate();report["multipleUnassignedControlsAccepted"]=bindings.accepted;
CodexPlugin catalog;catalog.workspace.setProperty("codexApps",QJsonArray{QJsonObject{{"id","old"}}});report["catalogRefreshSendsTypedRequests"]=catalog.refreshNativeCatalog(nullptr)&&catalog.pending.size()==4;
const auto requests=catalog.pending;for(auto it=requests.cbegin();it!=requests.cend();++it)catalog.respond(it.key(),{{"data",QJsonArray{QJsonObject{{"id","new"}}}}});
for(const char*property:{"codexApps","codexSkills","codexPlugins","codexMcpServers"})report[QString(property)+"Updated"]=catalog.workspace.property(property).toJsonArray()[0].toObject()["id"]=="new";
catalog.allow=false;report["catalogSendFailurePreservesExistingState"]=!catalog.refreshNativeCatalog(nullptr)&&catalog.workspace.property("codexApps").toJsonArray()[0].toObject()["id"]=="new";
CompletionFixture completion;completion.bar.setTranslationVisible(false);completion.finish();report["completionDoesNotReopenUserDisabledTranslation"]=!completion.applied&&!completion.bar.translationVisible();
completion.bar.setTranslationVisible(true);completion.finish();report["enabledTranslationStillAcceptsCompletion"]=completion.applied&&completion.bar.translationVisible();return finish(report);}
'''


class UiActionContractRepairs(unittest.TestCase):
    def test_hidden_actions_and_state(self):
        support = (ROOT / 'src/ui/app/AIAgentWorkspaceSupport.cpp').read_text(encoding='utf-8-sig')
        helpers = '\n'.join(function(support, sig) for sig in ('QString sanitizedActionText(', 'QAction* findWindowAction(', 'bool setWindowActionChecked('))
        input = (ROOT / 'src/ui/app/ApplicationInput.cpp').read_text(encoding='utf-8-sig')
        native = function(input, 'bool MainWindow::nativeEvent(').replace('bool MainWindow::nativeEvent(', 'bool ProbeWindow::nativeEvent(', 1)
        settings = (ROOT / 'src/ui/app/ApplicationSettings.cpp').read_text(encoding='utf-8-sig')
        start = settings.index('        QMap<QString, QString> assignedCommands;')
        end = settings.index('        for (const auto& binding : *bindingEditors) {\n            const QString old', start)
        preflight = settings[start:end]
        plugin = (ROOT / 'src/plugins/codex/CodexPlugin.cpp').read_text(encoding='utf-8-sig')
        methods = function(plugin, 'bool CodexPlugin::refreshNativeCatalog(') + '\n' + function(plugin, 'bool CodexPlugin::listSkills(')
        start = plugin.index('    case PendingRequest::McpServerStatus:')
        response = plugin[start:plugin.index('    case PendingRequest::ProviderCapabilities:', start)]
        enhancement = (ROOT / 'src/ui/app/ApplicationEnhancement.cpp').read_text(encoding='utf-8-sig')
        start = enhancement.index('        // Generation can finish after the user')
        guard = enhancement[start:enhancement.index('        if (!result.success)', start)]
        main = CODE.replace('HELPERS', helpers).replace('NATIVE_EVENT', native).replace('PREFLIGHT', preflight).replace('CATALOG_RESPONSE', response).replace('CATALOG_METHODS', methods).replace('COMPLETION_GUARD', guard)
        build_and_run(OUT, main, sources=['src/ui/app/PlaybackBar.cpp', 'src/ui/app/PlaybackBar.h', 'src/core/playback/api/PlaybackServiceSignals.h'])


if __name__ == '__main__':
    unittest.main()
