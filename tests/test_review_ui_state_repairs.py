"""R41/R44/R45/R49/R50/R66: real hidden Qt controls and production report writer."""
import unittest
from tests.native_review_probe import build_and_run
from tests.test_review_metadata_repairs import ROOT, function

OUT = ROOT / 'tests/artifacts/fix_review_20261101/stage17'

HEADERS = r'''
#include <QApplication>
#include <QPointer>
#include <QMouseEvent>
#include <QLabel>
#include <QTextDocument>
#include <QSaveFile>
#include <QAbstractButton>
#include <QToolButton>
#include <QDateTime>
#include <QDebug>
#include <memory>
#include "features/annotation/AnnotationItem.h"
#include "features/annotation/AnnotationOverlay.h"
#include "ui/app/PlaybackBar.h"
#include "playback/api/IPlaybackService.h"
#define private public
#include "features/annotation/ReviewPanel.h"
#undef private
using namespace cgplay;
struct Mapper:IViewerCoordinateMapper{double scale=1;
QPointF imageToWidget(const QPointF&p,int,int)const override{return p*scale;}
QPointF widgetToImage(const QPointF&p,int,int)const override{return p/scale;}
QRectF visibleVideoRect()const override{return {0,0,320,200};}double zoom()const override{return scale;}QPointF viewPos()const override{return {};}};
'''

MAIN = r'''
int main(int argc,char**argv){QApplication app(argc,argv);QJsonObject report;
ReviewPanel panel(nullptr);panel.setAttribute(Qt::WA_DontShowOnScreen);AnnotationItem a;
a.id="same";a.frame=12;a.author="OldAuthor";a.createdTime="2025-01-01";a.comment="unchanged";
panel.refresh({a},12,24);a.author="NewAuthor";panel.refresh({a},12,24);panel._search->setText("NewAuthor");
auto cardCount=[&]{int n=0;for(int i=0;i<panel._cardLayout->count();++i){auto*w=panel._cardLayout->itemAt(i)->widget();if(w&&!w->property("annId").toString().isEmpty())++n;}return n;};
report["metadataOnlyAuthorRefreshAndSearch"]=panel._annotations[0].author=="NewAuthor"&&cardCount()==1;
a.createdTime="2030-01-02";panel.refresh({a},12,24);report["metadataOnlyTimeRefresh"]=panel._annotations[0].createdTime==a.createdTime;
Mapper mapper;AnnotationOverlay overlay(nullptr);overlay.setCoordinateMapper(&mapper);overlay.resize(640,400);overlay.setMediaSize(320,200);
auto press=[&](QPointF p){QMouseEvent e(QEvent::MouseButtonPress,p,p,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(&overlay,&e);return overlay.selectedId();};
a.id="free";a.type=AnnotationType::FreeDraw;a.points={{20,20},{200,20},{200,100}};overlay.setAnnotations({a});
report["freeDrawSparseSegmentsHit"]=press({110,20})=="free"&&press({200,60})=="free";
report["freeDrawAwayFromStrokeMisses"]=press({110,70}).isEmpty();
mapper.scale=2;report["freeDrawZoomedSegmentHit"]=press({220,40})=="free";
a.id="arrow";a.type=AnnotationType::Arrow;overlay.setAnnotations({a});report["arrowStillSelectable"]=press({220,40})=="arrow";
auto service=std::make_shared<FakePlayback>();auto*bar=new PlaybackBar(service);
QPointer<QWidget>popup=bar->findChild<QWidget*>("PlaybackBarVolumePopup");report["popupOwnedAndRemainsPopup"]=popup&&popup->isWindow()&&popup->parentWidget()==bar;
auto percent=[&]{for(auto*l:popup->findChildren<QLabel*>())if(l->text().endsWith('%'))return l->text();return QString();};
auto*slider=popup->findChild<QSlider*>();service->setVolume(.25f);report["externalVolumeRefreshesLabel"]=slider->value()==250&&percent()=="25%";
slider->setValue(400);report["sliderRefreshesLabel"]=percent()=="40%";service->setVolume(.6f);report["externalAfterSliderRefreshesLabel"]=percent()=="60%";
auto*speed=bar->findChild<QToolButton*>("PlaybackBarSpeed");speed->setProperty("cgplay.buttonStyleCaptured",true);
speed->setProperty("cgplay.originalText",speed->text());service->setSpeed(50);
const QList<QWidget*>allWidgets{speed};
RESTORE
report["themeRestorePreservesCurrentSpeed"]=speed->text()=="2.0x"&&service->speedMultiplier()==2;
delete bar;QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);report["ownerDeletionReleasesPopup"]=popup.isNull();
const QString title="shot_&copy;.mp4";a.createdTime="Time <Literal> & stamp";a.assignee="Artist <Lead> & QA";a.comment="Comment <literal>";
const QString html=QDir(QString::fromUtf8(argv[1])).filePath("review.html");bool ok=ReviewExport::exportHtml({a},html,title);
QFile file(html);ok=ok&&file.open(QIODevice::ReadOnly);QTextDocument doc;doc.setHtml(QString::fromUtf8(file.readAll()));const QString text=doc.toPlainText();
report["htmlMetadataRemainsLiteral"]=ok&&text.contains(title)&&text.contains(a.createdTime)&&text.contains(a.assignee)&&text.contains(a.comment);
return finish(report);}
'''


class ReviewUiStateRepairs(unittest.TestCase):
    def test_real_controls_and_report(self):
        prior = (ROOT / 'tests/artifacts/review_full_20261031/phase50/probe.cpp').read_text(encoding='utf-8-sig')
        fake = prior[prior.index('class FakePlayback'):prior.index('QString percentText')]
        runtime = (ROOT / 'src/ui/app/ApplicationRuntime.cpp').read_text(encoding='utf-8-sig')
        restore = function(runtime[runtime.index('    const auto applyNamedButtonStyle'):], 'for (QWidget* widget : allWidgets)')
        export = (ROOT / 'src/features/annotation/ReviewExport.cpp').read_text(encoding='utf-8-sig')
        export = 'namespace cgplay {class ReviewExport {public:static bool exportHtml(const QVector<AnnotationItem>&,const QString&,const QString&);};\n' + function(export, 'bool ReviewExport::exportHtml(') + '\n}\n'
        build_and_run(OUT, HEADERS + fake + export + MAIN.replace('RESTORE', restore), sources=[
            'src/features/annotation/ReviewPanel.cpp', 'src/features/annotation/ReviewPanel.h',
            'src/features/annotation/AnnotationOverlay.cpp', 'src/features/annotation/AnnotationOverlay.h',
            'src/features/annotation/AnnotationManager.cpp', 'src/features/annotation/AnnotationManager.h',
            'src/ui/app/PlaybackBar.cpp', 'src/ui/app/PlaybackBar.h', 'src/core/playback/api/PlaybackServiceSignals.h'])


if __name__ == '__main__':
    unittest.main()
