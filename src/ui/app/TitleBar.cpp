// CGPlay TitleBar — 42px frameless, gradient glass, status indicators
#include "TitleBar.h"
#include <QHBoxLayout>
#include <QPainter>
#include <QMouseEvent>
#include <QGraphicsDropShadowEffect>
#include <QWindow>

namespace cgplay {

static const char* kBg     = "#111418";
static const char* kBorder = "#252B33";
static const char* kText   = "#D8DEE7";
static const char* kSec    = "#9AA4B2";
static const char* kAccent = "#FF8A3D";

TitleBar::TitleBar(QWidget* parent) : QWidget(parent)
{
    setFixedHeight(42);
    _setupUI();
}

QPushButton* TitleBar::_makeMenu(const QString& text)
{
    auto* b=new QPushButton(text,this); b->setFlat(true); b->setCursor(Qt::PointingHandCursor);
    b->setFixedHeight(42);
    b->setStyleSheet(QString(
        "QPushButton{color:%1;background:transparent;border:none;font-family:'Segoe UI';font-size:12px;font-weight:400;padding:0 12px;}"
        "QPushButton:hover{color:%2;background:rgba(255,138,61,0.08);}"
    ).arg(kSec,kText));
    return b;
}

QLabel* TitleBar::_makeInfo(const QString& text, const QString& color)
{
    auto* l=new QLabel(text,this);
    l->setStyleSheet(QString("color:%1;font-family:'Segoe UI';font-size:11px;background:transparent;padding:0 6px;").arg(color));
    return l;
}

void TitleBar::_setupUI()
{
    auto* layout=new QHBoxLayout(this); layout->setContentsMargins(0,0,0,0); layout->setSpacing(0);

    // Logo — gradient text
    auto* logo=new QLabel("CGPlay",this);
    logo->setStyleSheet(QString("color:%1;font-family:'Segoe UI';font-size:14px;font-weight:700;padding:0 16px;background:transparent;").arg(kAccent));
    layout->addWidget(logo);

    _fileBtn=_makeMenu(QString::fromUtf8("文件"));
    _viewBtn=_makeMenu(QString::fromUtf8("视图"));
    _windowBtn=_makeMenu(QString::fromUtf8("窗口"));
    _ocioBtn=_makeMenu(QString::fromUtf8("颜色"));
    _audioBtn=_makeMenu(QString::fromUtf8("音频"));
    _helpBtn=_makeMenu(QString::fromUtf8("帮助"));
    layout->addWidget(_fileBtn); layout->addWidget(_viewBtn); layout->addWidget(_windowBtn);
    layout->addWidget(_ocioBtn); layout->addWidget(_audioBtn); layout->addWidget(_helpBtn);
    layout->addStretch();

    // Status indicators — right side
    _lblFPS      = _makeInfo("-- FPS", kText);
    _lblCodec    = _makeInfo("--", kAccent);
    _lblBitDepth = _makeInfo("--", kAccent);
    _lblDecoder  = _makeInfo("--", kAccent);
    _lblGPU      = _makeInfo("--", kSec);
    _lblRes      = _makeInfo("3840×2160", kText);

    auto sep=[&](){ auto* s=new QLabel(QString::fromUtf8("·"),this);
        s->setStyleSheet("color:rgba(255,255,255,0.15);font-size:14px;padding:0 2px;background:transparent;"); layout->addWidget(s); };

    sep(); layout->addWidget(_lblFPS);
    sep(); layout->addWidget(_lblCodec);
    sep(); layout->addWidget(_lblBitDepth);
    sep(); layout->addWidget(_lblDecoder);
    sep(); layout->addWidget(_lblGPU);
    sep(); layout->addWidget(_lblRes);
    _lblRes->setText("--");

    // Window controls
    auto mkWin=[&](const QString& t,const QString& tip,const QString& hov)->QPushButton*{
        auto* b=new QPushButton(t,this);b->setFixedSize(46,32);b->setFlat(true);b->setCursor(Qt::PointingHandCursor);
        b->setStyleSheet(QString("QPushButton{color:%1;background:transparent;border:none;font-size:14px;} QPushButton:hover{background:%2;}").arg(kSec,hov));
        b->setToolTip(tip); return b;
    };
    _minBtn=mkWin(QString::fromUtf8("─"),QString::fromUtf8("最小化"),"rgba(255,255,255,0.05)");
    _maxBtn=mkWin(QString::fromUtf8("□"),QString::fromUtf8("最大化"),"rgba(255,255,255,0.05)");
    _closeBtn=mkWin(QString::fromUtf8("✕"),QString::fromUtf8("关闭"),"#E81123");
    layout->addWidget(_minBtn); layout->addWidget(_maxBtn); layout->addWidget(_closeBtn);

    connect(_minBtn,&QPushButton::clicked,this,[this]{if(auto* w=window())w->showMinimized();});
    connect(_maxBtn,&QPushButton::clicked,this,[this]{if(auto* w=window())w->isMaximized()?w->showNormal():w->showMaximized();});
    connect(_closeBtn,&QPushButton::clicked,this,[this]{if(auto* w=window())w->close();});
}

void TitleBar::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    QLinearGradient g(0,0,width(),height());
    g.setColorAt(0.0,QColor(0x11,0x14,0x18));
    g.setColorAt(0.5,QColor(0x13,0x16,0x1A));
    g.setColorAt(1.0,QColor(0x11,0x14,0x18));
    p.fillRect(rect(),g);
    p.setPen(QPen(QColor(0x25,0x2B,0x33),1));
    p.drawLine(0,height()-1,width(),height()-1);
}

void TitleBar::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(e);
        return;
    }
    if (auto* handle = window()->windowHandle(); handle && handle->startSystemMove()) {
        _manualDragging = false;
        e->accept();
        return;
    }
    _manualDragging = true;
    _dragPos = e->globalPosition().toPoint() - window()->geometry().topLeft();
    e->accept();
}

void TitleBar::mouseMoveEvent(QMouseEvent* e)
{
    if (_manualDragging && (e->buttons() & Qt::LeftButton)) {
        window()->move(e->globalPosition().toPoint() - _dragPos);
        e->accept();
        return;
    }
    QWidget::mouseMoveEvent(e);
}

void TitleBar::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton) {
        _manualDragging = false;
    }
    QWidget::mouseReleaseEvent(e);
}
void TitleBar::mouseDoubleClickEvent(QMouseEvent*){if(auto* w=window())w->isMaximized()?w->showNormal():w->showMaximized();}

} // namespace cgplay
