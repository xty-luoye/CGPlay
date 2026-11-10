"""R84/R85 real report writers and both hidden export controls."""
import unittest
from tests.native_review_probe import build_and_run
from tests.test_review_metadata_repairs import ROOT,function
OUT=ROOT/'tests/artifacts/fix_review_20261101/stage27'
MAIN=r'''
#include "features/annotation/ReviewExport.h"
#include "features/annotation/AnnotationItem.h"
#include "services/ai/api/AIProviderTypes.h"
#include <QApplication>
#include <QPushButton>
#include <QHBoxLayout>
#include <QTemporaryDir>
#include <QSaveFile>
#include <QDateTime>
#include <QDebug>
using namespace cgplay;
namespace cgplay{
EXPORT_METHODS
}
class Workspace:public QWidget{public:QPushButton*_exportReportButton=nullptr,*_toolsExportReportButton=nullptr;int calls=0;AIResponse _lastResponse;void _exportReviewReport(const AIResponse&){++calls;}void setup(){auto*_toolsBodyFrame=new QWidget(this);auto*composerCard=new QWidget(this);auto*actionRow=new QHBoxLayout(_toolsBodyFrame);auto*buttonRow=new QHBoxLayout(composerCard);auto zh=[](const char8_t*t){return QString::fromUtf8(reinterpret_cast<const char*>(t));};
BUTTONS
CONNECTIONS
}void busy(bool busy){
BUSY
}};
int main(int argc,char**argv){QApplication app(argc,argv);QDir dir(QString::fromUtf8(argv[1]));QJsonObject result;AIResponse response;response.providerId="provider<";response.model="model&";response.success=true;response.rawText="ALPHA <script>alert(1)</script>";response.reviewSummary.plainText="SUMMARY";AIAnalysisFinding f;f.frame=15;f.title="Unapplied finding";f.description="detail<";response.findings.append(f);f.title="Summary finding";response.reviewSummary.findings.append(f);AnnotationItem ann;ann.comment="Existing annotation";
auto read=[](QString p){QFile file(p);file.open(QIODevice::ReadOnly);return file.readAll();};const QString json=dir.filePath("report.json"),html=dir.filePath("report.html");bool ok=ReviewExport::exportAIReport(response,{ann},json,"title");auto root=QJsonDocument::fromJson(read(json)).object();result["jsonKeepsResponseAndAnnotations"]=ok&&root["ai_report"].toObject()["rawText"].toString()==response.rawText&&root["annotations"].toArray().size()==1&&root["ai_report"].toObject()["findings"].toArray().size()==1&&root["ai_report"].toObject()["reviewSummary"].toObject()["findings"].toArray().size()==1;
ok=ReviewExport::exportAIReport(response,{ann},html,"title");auto text=read(html);result["htmlIncludesGeneratedSummaryAndUnappliedFindings"]=ok&&text.contains("SUMMARY")&&text.contains("Unapplied finding")&&text.contains("Existing annotation")&&text.contains("Summary finding");result["htmlEscapesAIContent"]=text.contains("&lt;script&gt;")&&!text.contains("<script>");response.rawText="BETA";ok=ReviewExport::exportAIReport(response,{},json,"title");root=QJsonDocument::fromJson(read(json)).object();result["differentResponseChangesActualExport"]=ok&&root["ai_report"].toObject()["rawText"].toString()=="BETA"&&root["annotations"].toArray().isEmpty();
result["unwritableExportFails"]=!ReviewExport::exportAIReport(response,{},dir.path(),"title");Workspace workspace;workspace.setup();workspace._exportReportButton->click();workspace._toolsExportReportButton->click();result["bothEntrypointsExport"]=workspace.calls==2;workspace.busy(true);workspace._exportReportButton->click();workspace._toolsExportReportButton->click();result["busyDisablesBoth"]=workspace.calls==2&&!workspace._exportReportButton->isEnabled()&&!workspace._toolsExportReportButton->isEnabled();workspace.busy(false);workspace._toolsExportReportButton->click();result["bothRestoreEnabledState"]=workspace.calls==3&&workspace._exportReportButton->isEnabled();return finish(result);}
'''
class AIReportRepairs(unittest.TestCase):
 def test_real_report_and_controls(self):
    source=(ROOT/'src/features/annotation/ReviewExport.cpp').read_text(encoding='utf-8-sig');methods='\n'.join(function(source,s) for s in ['bool ReviewExport::exportJson(', 'bool ReviewExport::exportAIReport(', 'bool ReviewExport::exportHtml('])
    source=(ROOT/'src/ui/app/AIAgentWorkspace.cpp').read_text(encoding='utf-8-sig');buttons=[];connections=[];busy=[]
    for name in ['_toolsExportReportButton','_exportReportButton']:
      a=source.index('    '+name+' = new QPushButton');b=source.index('\n',source.index('->addWidget('+name+');',a));buttons.append(source[a:b])
      a=source.index('    connect('+name+', &QPushButton::clicked');b=source.index('\n    });',a)+len('\n    });');connections.append(source[a:b])
      busy.append(function(source,'if ('+name+')'))
    build_and_run(OUT,MAIN.replace('EXPORT_METHODS',methods).replace('BUTTONS','\n'.join(buttons)).replace('CONNECTIONS','\n'.join(connections)).replace('BUSY','\n'.join(busy)))
if __name__=='__main__':unittest.main()
