#include "ViewerWidget.h"

#include "TlViewport.h"
#include "common/theme/BackdropRenderer.h"
#include "ocio/OcioManager.h"
#include "playback/api/IPlaybackService.h"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QApplication>
#include <QFocusEvent>
#include <QFrame>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QMimeData>
#include <QMenu>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QTimer>
#include <QPixmap>
#include <QPen>
#include <QIcon>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace cgplay {

namespace {

constexpr const char* kViewerBg = "#050505";
constexpr const char* kButtonBorder = "rgba(255,255,255,0.070)";
constexpr const char* kButtonFill = "rgba(12,16,21,0.62)";
constexpr const char* kButtonHover = "#202832";
constexpr const char* kAccent = "#ff8c32";
constexpr const char* kText = "#d8dee7";
constexpr const char* kMutedText = "#c7d0da";

QColor runtimeColor(const char* name, const QColor& fallback)
{
    if (!qApp) return fallback;
    const QColor value(qApp->property(name).toString());
    return value.isValid() ? value : fallback;
}

QString runtimeString(const char* name, const QString& fallback = {})
{
    if (!qApp) return fallback;
    const QString value = qApp->property(name).toString().trimmed();
    return value.isEmpty() ? fallback : value;
}

int runtimeInt(const char* name, int fallback)
{
    if (!qApp) return fallback;
    bool ok = false;
    const int value = qApp->property(name).toInt(&ok);
    return ok ? value : fallback;
}

QColor runtimeThemeColor(const char* name, const QColor& fallback)
{
    const QColor value = runtimeColor(name, fallback);
    return value.isValid() ? value : fallback;
}

QString colorCss(const QColor& color)
{
    return color.name(QColor::HexArgb);
}

QColor blendThemeColor(const QColor& foreground, const QColor& backdrop, int foregroundOpacity)
{
    QColor result = backdrop;
    result.setRed((foreground.red() * foregroundOpacity + backdrop.red() * (100 - foregroundOpacity)) / 100);
    result.setGreen((foreground.green() * foregroundOpacity + backdrop.green() * (100 - foregroundOpacity)) / 100);
    result.setBlue((foreground.blue() * foregroundOpacity + backdrop.blue() * (100 - foregroundOpacity)) / 100);
    result.setAlpha(255);
    return result;
}

QColor contrastingThemeText(const QColor& background)
{
    const int luminance = (background.red() * 299 + background.green() * 587 + background.blue() * 114) / 1000;
    return luminance >= 150 ? QColor(QStringLiteral("#111418")) : QColor(Qt::white);
}

void drawRuntimeBackdrop(QPainter& painter, const QRect& target, const QWidget* surface)
{
    BackdropRenderOptions options;
    options.underlay = runtimeColor("cgplay.viewerColor", QColor(kViewerBg));
    options.readabilityWashAlpha = 24;
    drawApplicationBackdrop(painter, target, surface, options);
}

QString _toolButtonStyle()
{
    const QColor panel = runtimeThemeColor("cgplay.toolbarColor", QColor(kButtonFill));
    const QColor border = runtimeThemeColor("cgplay.borderColor", QColor(QStringLiteral("#4A5662")));
    const QColor accent = runtimeThemeColor("cgplay.accentColor", QColor(kAccent));
    const QColor text = runtimeThemeColor("cgplay.textColor", QColor(kText));
    const QColor hover = blendThemeColor(accent, panel, 22);
    const QColor pressed = blendThemeColor(accent, panel, 72);
    const QColor accentText = contrastingThemeText(pressed);
    return QStringLiteral(
        "QToolButton {"
        " color: %1;"
        " background: %2;"
        " border: 1px solid %3;"
        " border-radius: 8px;"
        " padding: 0;"
        " font-size: 11px;"
        " font-weight: 600;"
        " }"
        "QToolButton:hover {"
        " background: %4;"
        " color: %5;"
        " border-color: %7;"
        " }"
        "QToolButton:pressed {"
        " background: %6;"
        " border-color: %7;"
        " color: %8;"
        " }"
        "QToolButton:checked {"
        " background: %6;"
        " border-color: %7;"
        " color: %8;"
        " }")
        .arg(colorCss(text), colorCss(panel), colorCss(border), colorCss(hover), colorCss(text), colorCss(pressed), colorCss(accent), colorCss(accentText));
}

QString _textToolButtonStyle()
{
    const QColor panel = runtimeThemeColor("cgplay.toolbarColor", QColor(kButtonFill));
    const QColor border = runtimeThemeColor("cgplay.borderColor", QColor(QStringLiteral("#4A5662")));
    const QColor accent = runtimeThemeColor("cgplay.accentColor", QColor(kAccent));
    const QColor text = runtimeThemeColor("cgplay.textColor", QColor(kText));
    const QColor hover = blendThemeColor(accent, panel, 22);
    const QColor pressed = blendThemeColor(accent, panel, 72);
    const QColor accentText = contrastingThemeText(pressed);
    return QStringLiteral(
        "QToolButton {"
        " color: %1;"
        " background: %2;"
        " border: 1px solid %3;"
        " border-radius: 8px;"
        " padding: 0 5px;"
        " font-size: 11px;"
        " font-weight: 700;"
        " }"
        "QToolButton:hover {"
        " background: %4;"
        " color: %5;"
        " border-color: %6;"
        " }"
        "QToolButton:pressed {"
        " background: %7;"
        " border-color: %6;"
        " color: %8;"
        " }"
        "QToolButton:checked {"
        " background: %7;"
        " border-color: %6;"
        " color: %8;"
        " }"
        "QToolButton:disabled {"
        " color: %9;"
        " background: %10;"
        " border-color: %11;"
        " }")
        .arg(colorCss(text), colorCss(panel), colorCss(border), colorCss(hover), colorCss(text), colorCss(accent),
             colorCss(pressed), colorCss(accentText), colorCss(blendThemeColor(text, panel, 60)),
             colorCss(blendThemeColor(panel, QColor(Qt::black), 65)), colorCss(blendThemeColor(border, panel, 55)));
}

QIcon _glyphIcon(std::function<void(QPainter&, const QRectF&)> draw)
{
    auto make = [&](const QColor& color) {
        QPixmap px(24, 24);
        px.fill(Qt::transparent);
        QPainter painter(&px);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QPen pen(color);
        pen.setWidthF(1.5);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        draw(painter, QRectF(3, 3, 18, 18));
        return px;
    };

    QIcon icon;
    icon.addPixmap(make(QColor(kMutedText)), QIcon::Normal, QIcon::Off);
    icon.addPixmap(make(QColor("#EEF3F8")), QIcon::Active, QIcon::Off);
    icon.addPixmap(make(QColor(kAccent)), QIcon::Normal, QIcon::On);
    icon.addPixmap(make(Qt::white), QIcon::Selected, QIcon::On);
    return icon;
}

QIcon _fitIcon()
{
    return _glyphIcon([](QPainter& p, const QRectF& r) {
        p.drawLine(QPointF(r.left()+2, r.top()+7), QPointF(r.left()+2, r.top()+2));
        p.drawLine(QPointF(r.left()+2, r.top()+2), QPointF(r.left()+7, r.top()+2));
        p.drawLine(QPointF(r.right()-2, r.top()+7), QPointF(r.right()-2, r.top()+2));
        p.drawLine(QPointF(r.right()-2, r.top()+2), QPointF(r.right()-7, r.top()+2));
        p.drawLine(QPointF(r.left()+2, r.bottom()-7), QPointF(r.left()+2, r.bottom()-2));
        p.drawLine(QPointF(r.left()+2, r.bottom()-2), QPointF(r.left()+7, r.bottom()-2));
        p.drawLine(QPointF(r.right()-2, r.bottom()-7), QPointF(r.right()-2, r.bottom()-2));
        p.drawLine(QPointF(r.right()-2, r.bottom()-2), QPointF(r.right()-7, r.bottom()-2));
    });
}

QIcon _oneToOneIcon()
{
    return _glyphIcon([](QPainter& p, const QRectF& r) {
        p.drawLine(QPointF(r.left()+4, r.center().y()), QPointF(r.right()-4, r.center().y()));
        p.drawEllipse(QPointF(r.left()+7, r.center().y()), 1.2, 1.2);
        p.drawEllipse(QPointF(r.right()-7, r.center().y()), 1.2, 1.2);
    });
}

QIcon _minusIcon()
{
    return _glyphIcon([](QPainter& p, const QRectF& r) {
        p.drawLine(QPointF(r.left()+4, r.center().y()), QPointF(r.right()-4, r.center().y()));
    });
}

QIcon _plusIcon()
{
    return _glyphIcon([](QPainter& p, const QRectF& r) {
        p.drawLine(QPointF(r.left()+4, r.center().y()), QPointF(r.right()-4, r.center().y()));
        p.drawLine(QPointF(r.center().x(), r.top()+4), QPointF(r.center().x(), r.bottom()-4));
    });
}

QIcon _ocioIcon()
{
    return _glyphIcon([](QPainter& p, const QRectF& r) {
        p.drawRoundedRect(QRectF(r.left()+3, r.top()+4, r.width()-6, r.height()-8), 3, 3);
        p.drawLine(QPointF(r.left()+7, r.center().y()), QPointF(r.right()-7, r.center().y()));
    });
}

QIcon _compareIcon()
{
    return _glyphIcon([](QPainter& p, const QRectF& r) {
        p.drawLine(QPointF(r.left()+5, r.center().y()-4), QPointF(r.right()-5, r.center().y()-4));
        p.drawLine(QPointF(r.left()+5, r.center().y()+4), QPointF(r.right()-5, r.center().y()+4));
        p.drawLine(QPointF(r.left()+5, r.center().y()-7), QPointF(r.left()+5, r.center().y()-1));
        p.drawLine(QPointF(r.right()-5, r.center().y()+1), QPointF(r.right()-5, r.center().y()+7));
    });
}

QIcon _tileIcon()
{
    return _glyphIcon([](QPainter& p, const QRectF& r) {
        const QRectF box(r.left()+4, r.top()+4, r.width()-8, r.height()-8);
        p.drawRoundedRect(box, 2, 2);
        p.drawLine(QPointF(box.center().x(), box.top()), QPointF(box.center().x(), box.bottom()));
        p.drawLine(QPointF(box.left(), box.center().y()), QPointF(box.right(), box.center().y()));
    });
}

QIcon _fullscreenIcon()
{
    return _glyphIcon([](QPainter& p, const QRectF& r) {
        p.drawLine(QPointF(r.left()+4, r.top()+8), QPointF(r.left()+4, r.top()+4));
        p.drawLine(QPointF(r.left()+4, r.top()+4), QPointF(r.left()+8, r.top()+4));
        p.drawLine(QPointF(r.right()-4, r.top()+8), QPointF(r.right()-4, r.top()+4));
        p.drawLine(QPointF(r.right()-4, r.top()+4), QPointF(r.right()-8, r.top()+4));
        p.drawLine(QPointF(r.left()+4, r.bottom()-8), QPointF(r.left()+4, r.bottom()-4));
        p.drawLine(QPointF(r.left()+4, r.bottom()-4), QPointF(r.left()+8, r.bottom()-4));
        p.drawLine(QPointF(r.right()-4, r.bottom()-8), QPointF(r.right()-4, r.bottom()-4));
        p.drawLine(QPointF(r.right()-4, r.bottom()-4), QPointF(r.right()-8, r.bottom()-4));
    });
}

QString _timecodeFromFrame(int frame, double fps)
{
    if (fps <= 0.0) {
        fps = 24.0;
    }
    const int fpsInt = std::max(1, static_cast<int>(fps + 0.5));
    const int ff = frame % fpsInt;
    const int totalSeconds = frame / fpsInt;
    const int ss = totalSeconds % 60;
    const int mm = (totalSeconds / 60) % 60;
    const int hh = totalSeconds / 3600;
    return QStringLiteral("%1:%2:%3:%4")
        .arg(hh, 2, 10, QChar('0'))
        .arg(mm, 2, 10, QChar('0'))
        .arg(ss, 2, 10, QChar('0'))
        .arg(ff, 2, 10, QChar('0'));
}

QToolButton* _makeButton(QWidget* parent, const QString& text, const QString& tooltip, bool checkable = false)
{
    auto* button = new QToolButton(parent);
    button->setText(text);
    button->setToolTip(tooltip);
    button->setCheckable(checkable);
    button->setCursor(Qt::PointingHandCursor);
    button->setAutoRaise(false);
    button->setStyleSheet(_toolButtonStyle());
    button->setFixedSize(32, 30);
    return button;
}

QToolButton* _makeTextButton(QWidget* parent, const QString& text, const QString& tooltip, bool checkable = false)
{
    auto* button = new QToolButton(parent);
    button->setText(text);
    button->setToolTip(tooltip);
    button->setCheckable(checkable);
    button->setCursor(Qt::PointingHandCursor);
    button->setAutoRaise(false);
    button->setStyleSheet(_textToolButtonStyle());
    int width = 40;
    if (text == QStringLiteral("Fit")) {
        width = 38;
    } else if (text == QStringLiteral("1:1")) {
        width = 38;
    } else if (text == QStringLiteral("LUT")) {
        width = 42;
    } else if (text == QStringLiteral("OCIO")) {
        width = 50;
    }
    button->setFixedSize(width, 30);
    return button;
}

} // namespace

struct ViewerWidget::Private
{
    QString viewId;
    std::shared_ptr<IPlaybackService> playback;
    std::shared_ptr<OcioManager> ocio;

    QVBoxLayout* rootLayout = nullptr;
    QWidget* toolbar = nullptr;
    QWidget* titleBar = nullptr;
    QLabel* shotLabel = nullptr;
    QLabel* zoomLabel = nullptr;
    QLabel* timecodeLabel = nullptr;

    QToolButton* fitButton = nullptr;
    QToolButton* oneToOneButton = nullptr;
    QToolButton* zoomOutButton = nullptr;
    QToolButton* zoomInButton = nullptr;
    QToolButton* lutButton = nullptr;
    QToolButton* ocioButton = nullptr;
    QToolButton* alphaButton = nullptr;
    QToolButton* compareButton = nullptr;
    QToolButton* tileButton = nullptr;
    QToolButton* fullscreenButton = nullptr;
    QToolButton* moreButton = nullptr;
    QFrame* zoomSeparator = nullptr;
    QFrame* colorSeparator = nullptr;
    QFrame* compareSeparator = nullptr;

    TlViewport* viewport = nullptr;
    QWidget* overlay = nullptr;

    bool panning = false;
    QPoint panGlobalStart;
    double zoom = 1.0;
    bool autoFit = true;
    bool timecodeVisible = false;
    bool alphaChannelVisible = false;
    bool invalidated = false;
    bool overlayRepaintQueued = false;
    QRect lastViewportSignalRect;
    bool hasLastViewportSignalRect = false;
    QElapsedTimer panThrottle;
};

ViewerWidget::ViewerWidget(
    std::shared_ptr<IPlaybackService> playback,
    std::shared_ptr<OcioManager> ocio,
    QWidget* parent)
    : QWidget(parent)
    , _p(std::make_unique<Private>())
{
    static quint64 sNextViewId = 1;
    _p->playback = std::move(playback);
    _p->ocio = std::move(ocio);
    _p->viewId = QStringLiteral("view-%1").arg(sNextViewId++);

    setAcceptDrops(true);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAttribute(Qt::WA_OpaquePaintEvent);

    _setupUI();
    _applyRuntimeTheme();
    _setupViewport();
    _connectPlayback();
    _applyOcioOptions();

    connect(this, &ViewerWidget::zoomChanged, this, [this](double zoom) {
        if (_p->zoomLabel) {
            _p->zoomLabel->setText(QStringLiteral("%1%").arg(static_cast<int>(zoom * 100.0 + 0.5)));
        }
    });

    if (_p->fitButton) {
        connect(_p->fitButton, &QToolButton::clicked, this, &ViewerWidget::fitToWindow);
    }
    if (_p->oneToOneButton) {
        connect(_p->oneToOneButton, &QToolButton::clicked, this, &ViewerWidget::zoom1to1);
    }
    if (_p->zoomOutButton) {
        connect(_p->zoomOutButton, &QToolButton::clicked, this, &ViewerWidget::zoomOut);
    }
    if (_p->zoomInButton) {
        connect(_p->zoomInButton, &QToolButton::clicked, this, &ViewerWidget::zoomIn);
    }
    if (_p->lutButton && _p->ocio) {
        connect(_p->lutButton, &QToolButton::clicked, this, [this] {
            _p->ocio->showSettings(this);
        });
    }
    if (_p->compareButton) {
        connect(_p->compareButton, &QToolButton::clicked, this, &ViewerWidget::compareRequested);
    }
    if (_p->tileButton) {
        connect(_p->tileButton, &QToolButton::clicked, this, &ViewerWidget::compareTileRequested);
    }
    if (_p->fullscreenButton) {
        connect(_p->fullscreenButton, &QToolButton::clicked, this, &ViewerWidget::fullscreenRequested);
    }
    if (_p->ocioButton && _p->ocio) {
        connect(_p->ocioButton, &QToolButton::toggled, _p->ocio.get(), &OcioManager::setEnabled);
        connect(_p->ocio.get(), &OcioManager::enabledChanged, this, [this](bool) {
            _applyOcioOptions();
        });
    }
    if (_p->alphaButton) {
        connect(_p->alphaButton, &QToolButton::toggled, this, &ViewerWidget::setAlphaChannelVisible);
    }
}

ViewerWidget::~ViewerWidget()
{
    invalidateView();
}

void ViewerWidget::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event && (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange)) {
        _applyRuntimeTheme();
    }
}

void ViewerWidget::_applyRuntimeTheme()
{
    if (!_p) return;
    const QColor toolbar = runtimeThemeColor("cgplay.toolbarColor", QColor(kButtonFill));
    const QColor panel = runtimeThemeColor("cgplay.panelColor", toolbar);
    const QColor viewer = runtimeThemeColor("cgplay.viewerColor", QColor(kViewerBg));
    const QColor border = runtimeThemeColor("cgplay.borderColor", QColor(QStringLiteral("#303B47")));
    const QColor text = runtimeThemeColor("cgplay.textColor", QColor(kText));
    if (_p->toolbar) {
        QColor toolbarSurface = toolbar;
        toolbarSurface.setAlpha(qRound(
            qBound(0, runtimeInt("cgplay.toolbarOpacity", 92), 100) * 255.0 / 100.0));
        _p->toolbar->setStyleSheet(QStringLiteral("#viewerChrome{background:%1;border-bottom:1px solid %2;}")
            .arg(colorCss(toolbarSurface), colorCss(border)));
    }
    if (_p->shotLabel) {
        _p->shotLabel->setStyleSheet(QStringLiteral("color:%1;font-size:12px;font-weight:600;background:transparent;").arg(colorCss(text)));
    }
    if (auto* titleChip = findChild<QWidget*>(QStringLiteral("viewerTitleChip"))) {
        titleChip->setStyleSheet(QStringLiteral("QWidget#viewerTitleChip{background:%1;border:1px solid %2;border-radius:8px;}")
            .arg(colorCss(blendThemeColor(toolbar, viewer, 82)), colorCss(runtimeThemeColor("cgplay.accentColor", QColor(kAccent)))));
    }
    if (auto* closeGlyph = findChild<QLabel*>(QStringLiteral("viewerCloseGlyph"))) {
        closeGlyph->setStyleSheet(QStringLiteral("color:%1;font-size:12px;font-weight:600;background:transparent;")
            .arg(colorCss(runtimeThemeColor("cgplay.accentColor", QColor(kAccent)))));
    }
    if (_p->zoomLabel) {
        _p->zoomLabel->setStyleSheet(QStringLiteral("color:%1;font-size:11px;font-weight:700;background:transparent;").arg(colorCss(text)));
    }
    if (_p->timecodeLabel) {
        _p->timecodeLabel->setStyleSheet(QStringLiteral(
            "QLabel{color:%1;background:%2;border:1px solid %3;border-radius:8px;padding:6px 12px;font-family:Consolas,'Courier New',monospace;font-size:16px;font-weight:700;}")
            .arg(colorCss(text), colorCss(blendThemeColor(runtimeThemeColor("cgplay.timelineColor", panel), viewer, 86)), colorCss(border)));
    }
    for (auto* separator : findChildren<QFrame*>(QStringLiteral("viewerToolbarSeparator"))) {
        separator->setStyleSheet(QStringLiteral("background:%1;border:0;").arg(colorCss(border)));
    }
    for (auto* button : findChildren<QToolButton*>()) {
        if (button->property("cgplay.customStyleApplied").toBool()) continue;
        const bool textButton = !button->text().isEmpty();
        button->setStyleSheet(textButton ? _textToolButtonStyle() : _toolButtonStyle());
    }
    update();
    if (_p->viewport) _p->viewport->update();
}

QString ViewerWidget::viewId() const
{
    return _p->viewId;
}

IPlaybackService* ViewerWidget::playbackService() const
{
    return _p->playback.get();
}

QWidget* ViewerWidget::overlayParentWidget() const
{
    return _p->viewport;
}

QRect ViewerWidget::overlayGeometry() const
{
    return _p->viewport ? _p->viewport->rect() : QRect();
}

void ViewerWidget::attachOverlay(QWidget* overlay)
{
    if (!overlay || !_p->viewport) {
        return;
    }
    if (_p->overlay && _p->overlay != overlay) {
        detachOverlay(_p->overlay);
    }
    _p->overlay = overlay;
    overlay->setParent(_p->viewport);
    overlay->installEventFilter(this);
    requestOverlayRefresh();
}

void ViewerWidget::detachOverlay(QWidget* overlay)
{
    if (!overlay) {
        return;
    }
    if (_p->overlay == overlay) {
        _p->overlay->removeEventFilter(this);
        _p->overlay = nullptr;
    }
    overlay->setParent(nullptr);
}

void ViewerWidget::requestOverlayRefresh()
{
    _refreshOverlayGeometry();
}

QPointF ViewerWidget::imageToWidget(const QPointF& point, int imageWidth, int imageHeight) const
{
    if (!_p->viewport) {
        return point;
    }
    return _p->viewport->imageToWidget(point.x(), point.y(), imageWidth, imageHeight);
}

QPointF ViewerWidget::widgetToImage(const QPointF& point, int imageWidth, int imageHeight) const
{
    if (!_p->viewport) {
        return point;
    }
    return _p->viewport->widgetToImage(point.x(), point.y(), imageWidth, imageHeight);
}

QRectF ViewerWidget::visibleVideoRect() const
{
    if (!_p->viewport) {
        return QRectF();
    }
    const ftk::V2I vp = _p->viewport->viewPos();
    const double z = _p->viewport->zoom();
    return QRectF(vp.x, vp.y, _mediaW * z, _mediaH * z);
}

double ViewerWidget::zoom() const
{
    return _p->viewport ? _p->viewport->zoom() : 1.0;
}

QPointF ViewerWidget::viewPos() const
{
    if (!_p->viewport) {
        return {};
    }
    const ftk::V2I vp = _p->viewport->viewPos();
    return QPointF(vp.x, vp.y);
}

TlViewport* ViewerWidget::viewport() const
{
    return _p->viewport;
}

void ViewerWidget::invalidateView()
{
    if (_p->viewId.isEmpty() || _p->invalidated) {
        return;
    }
    _p->invalidated = true;
    Q_EMIT viewInvalidated(_p->viewId);
}

void ViewerWidget::_setupUI()
{
    _p->rootLayout = new QVBoxLayout(this);
    _p->rootLayout->setContentsMargins(0, 0, 0, 0);
    _p->rootLayout->setSpacing(0);

    _p->toolbar = new QWidget(this);
    _p->toolbar->setObjectName(QStringLiteral("viewerChrome"));
    _p->toolbar->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    _p->toolbar->setFixedHeight(76);
    _p->toolbar->setAttribute(Qt::WA_StyledBackground, true);
    _p->toolbar->setStyleSheet(QStringLiteral("#viewerChrome{background:%1;border-bottom:1px solid %2;}")
        .arg(colorCss(runtimeThemeColor("cgplay.toolbarColor", QColor(kButtonFill))),
             colorCss(runtimeThemeColor("cgplay.borderColor", QColor(QStringLiteral("#303B47"))))));

    auto* chromeLayout = new QVBoxLayout(_p->toolbar);
    chromeLayout->setContentsMargins(12, 4, 12, 5);
    chromeLayout->setSpacing(3);

    _p->titleBar = new QWidget(_p->toolbar);
    _p->titleBar->setAttribute(Qt::WA_StyledBackground, true);
    _p->titleBar->setStyleSheet(QStringLiteral("background:transparent;"));
    auto* titleLayout = new QHBoxLayout(_p->titleBar);
    titleLayout->setContentsMargins(0, 0, 0, 0);
    titleLayout->setSpacing(0);

    auto* titleChip = new QWidget(_p->titleBar);
    titleChip->setObjectName(QStringLiteral("viewerTitleChip"));
    titleChip->setFixedHeight(28);
    titleChip->setAttribute(Qt::WA_StyledBackground, true);
    titleChip->setStyleSheet(QStringLiteral("QWidget#viewerTitleChip{background:%1;border:1px solid %2;border-radius:8px;}")
        .arg(colorCss(blendThemeColor(runtimeThemeColor("cgplay.toolbarColor", QColor(kButtonFill)),
                                      runtimeThemeColor("cgplay.backgroundColor", QColor(kViewerBg)), 82)),
             colorCss(runtimeThemeColor("cgplay.accentColor", QColor(kAccent)))));
    auto* chipLayout = new QHBoxLayout(titleChip);
    chipLayout->setContentsMargins(12, 0, 10, 0);
    chipLayout->setSpacing(10);

    _p->shotLabel = new QLabel(QStringLiteral("viewer"), titleChip);
    _p->shotLabel->setStyleSheet(
        QStringLiteral("color:%1;font-size:12px;font-weight:600;background:transparent;")
            .arg(colorCss(runtimeThemeColor("cgplay.textColor", QColor(kText)))));
    chipLayout->addWidget(_p->shotLabel);

    auto* closeGlyph = new QLabel(QStringLiteral("x"), titleChip);
    closeGlyph->setObjectName(QStringLiteral("viewerCloseGlyph"));
    closeGlyph->setStyleSheet(
        QStringLiteral("color:%1;font-size:12px;font-weight:600;background:transparent;")
            .arg(colorCss(runtimeThemeColor("cgplay.accentColor", QColor(kAccent)))));
    chipLayout->addWidget(closeGlyph);

    titleLayout->addWidget(titleChip, 0, Qt::AlignLeft);
    titleLayout->addStretch();
    chromeLayout->addWidget(_p->titleBar);

    auto* toolbarRow = new QWidget(_p->toolbar);
    toolbarRow->setAttribute(Qt::WA_StyledBackground, true);
    toolbarRow->setStyleSheet(QStringLiteral("background:transparent;"));
    toolbarRow->setFixedHeight(34);
    auto* toolbarLayout = new QHBoxLayout(toolbarRow);
    toolbarLayout->setContentsMargins(0, 2, 0, 2);
    toolbarLayout->setSpacing(7);
    toolbarLayout->addStretch();

    _p->fitButton = _makeTextButton(_p->toolbar, QStringLiteral("Fit"), QString::fromUtf8("适配窗口 (F)"));
    _p->oneToOneButton = _makeTextButton(_p->toolbar, QStringLiteral("1:1"), QString::fromUtf8("1:1 原始比例 (1)"));
    _p->fitButton->setProperty("commandId", QStringLiteral("view.fitToWindow"));
    _p->oneToOneButton->setProperty("commandId", QStringLiteral("view.zoom1to1"));
    _p->zoomOutButton = _makeButton(_p->toolbar, QStringLiteral("-"), QString::fromUtf8("缩小"));
    _p->zoomInButton = _makeButton(_p->toolbar, QStringLiteral("+"), QString::fromUtf8("放大"));
    _p->zoomOutButton->setObjectName(QStringLiteral("ViewerZoomOut"));
    _p->zoomOutButton->setProperty("commandId", QStringLiteral("view.zoomOut"));
    _p->zoomInButton->setObjectName(QStringLiteral("ViewerZoomIn"));
    _p->zoomInButton->setProperty("commandId", QStringLiteral("view.zoomIn"));
    _p->lutButton = _makeTextButton(
        _p->toolbar,
        QStringLiteral("LUT"),
        QString::fromUtf8("打开 LUT / OCIO 设置"));
    _p->lutButton->setObjectName(QStringLiteral("ViewerLutSettings"));
    _p->lutButton->setEnabled(static_cast<bool>(_p->ocio));
    _p->ocioButton = _makeTextButton(_p->toolbar, QStringLiteral("OCIO"), QString::fromUtf8("OCIO 色彩管理"), true);
    _p->alphaButton = _makeTextButton(_p->toolbar, QStringLiteral("A"), QString::fromUtf8("查看 Alpha 通道"), true);
    _p->compareButton = _makeButton(_p->toolbar, QString::fromUtf8("⇄"), QString::fromUtf8("切换对比"));
    _p->tileButton = _makeButton(_p->toolbar, QString::fromUtf8("▦"), QString::fromUtf8("平铺对比"));
    _p->fullscreenButton = _makeButton(_p->toolbar, QString::fromUtf8("⤢"), QString::fromUtf8("全屏 (F11)"));
    _p->lutButton->setProperty("commandId", QStringLiteral("ocio.settings"));
    _p->ocioButton->setProperty("commandId", QStringLiteral("ocio.enable"));
    _p->alphaButton->setProperty("commandId", QStringLiteral("ocio.alpha"));
    _p->tileButton->setProperty("commandId", QStringLiteral("compare.tile"));
    _p->fullscreenButton->setProperty("commandId", QStringLiteral("view.fullscreen"));
    // The compare button opens the panel; it does not toggle A/B media.
    for (auto* button : {_p->fitButton, _p->oneToOneButton, _p->zoomOutButton, _p->zoomInButton,
                         _p->lutButton, _p->ocioButton, _p->alphaButton, _p->compareButton,
                         _p->tileButton, _p->fullscreenButton})
        button->setAccessibleName(button->toolTip());
    _p->zoomOutButton->setText(QString());
    _p->zoomOutButton->setIcon(_minusIcon());
    _p->zoomOutButton->setIconSize(QSize(18, 18));
    _p->zoomInButton->setText(QString());
    _p->zoomInButton->setIcon(_plusIcon());
    _p->zoomInButton->setIconSize(QSize(18, 18));
    _p->compareButton->setText(QString());
    _p->compareButton->setIcon(_compareIcon());
    _p->compareButton->setIconSize(QSize(18, 18));
    _p->tileButton->setText(QString());
    _p->tileButton->setIcon(_tileIcon());
    _p->tileButton->setIconSize(QSize(18, 18));
    _p->fullscreenButton->setText(QString());
    _p->fullscreenButton->setIcon(_fullscreenIcon());
    _p->fullscreenButton->setIconSize(QSize(18, 18));

    _p->zoomLabel = new QLabel(QStringLiteral("100%"), toolbarRow);
    _p->zoomLabel->setStyleSheet(QStringLiteral("color:%1;font-size:11px;font-weight:700;background:transparent;")
                                     .arg(colorCss(runtimeThemeColor("cgplay.textColor", QColor(kText)))));
    _p->zoomLabel->setAlignment(Qt::AlignCenter);
    _p->zoomLabel->setFixedWidth(48);

    const auto addSeparator = [toolbarRow, toolbarLayout]() {
        auto* separator = new QFrame(toolbarRow);
        separator->setObjectName(QStringLiteral("viewerToolbarSeparator"));
        separator->setFrameShape(QFrame::VLine);
        separator->setFixedSize(1, 20);
        separator->setStyleSheet(QStringLiteral("background:%1;border:0;")
            .arg(colorCss(runtimeThemeColor("cgplay.borderColor", QColor(QStringLiteral("#303B47"))))));
        toolbarLayout->addWidget(separator);
        return separator;
    };

    toolbarLayout->addWidget(_p->fitButton);
    toolbarLayout->addWidget(_p->oneToOneButton);
    toolbarLayout->addWidget(_p->zoomOutButton);
    toolbarLayout->addWidget(_p->zoomInButton);
    toolbarLayout->addWidget(_p->zoomLabel);
    _p->zoomSeparator = addSeparator();
    toolbarLayout->addWidget(_p->lutButton);
    toolbarLayout->addWidget(_p->ocioButton);
    toolbarLayout->addWidget(_p->alphaButton);
    _p->colorSeparator = addSeparator();
    toolbarLayout->addWidget(_p->compareButton);
    toolbarLayout->addWidget(_p->tileButton);
    _p->compareSeparator = addSeparator();
    toolbarLayout->addWidget(_p->fullscreenButton);
    _p->moreButton = _makeButton(_p->toolbar, QStringLiteral("..."), QStringLiteral("\u66f4\u591a\u64ad\u653e\u5668\u63a7\u5236"));
    _p->moreButton->setPopupMode(QToolButton::InstantPopup);
    auto* moreMenu = new QMenu(_p->moreButton);
    auto* lutAction = moreMenu->addAction(QStringLiteral("LUT \u8bbe\u7f6e"));
    auto* ocioAction = moreMenu->addAction(QStringLiteral("OCIO"));
    ocioAction->setCheckable(true);
    auto* alphaAction = moreMenu->addAction(QStringLiteral("Alpha \u901a\u9053"));
    alphaAction->setCheckable(true);
    moreMenu->addSeparator();
    auto* compareAction = moreMenu->addAction(QStringLiteral("\u6bd4\u8f83"));
    auto* tileAction = moreMenu->addAction(QStringLiteral("\u5e73\u94fa\u6bd4\u8f83"));
    auto* fullscreenAction = moreMenu->addAction(QStringLiteral("\u5168\u5c4f"));
    // These mirror actions must not enter the plugin dispatcher's commandId
    // action lookup or acquire duplicate QAction shortcuts.
    lutAction->setProperty("cgplay.commandPresentation.id", QStringLiteral("ocio.settings"));
    ocioAction->setProperty("cgplay.commandPresentation.id", QStringLiteral("ocio.enable"));
    alphaAction->setProperty("cgplay.commandPresentation.id", QStringLiteral("ocio.alpha"));
    tileAction->setProperty("cgplay.commandPresentation.id", QStringLiteral("compare.tile"));
    fullscreenAction->setProperty("cgplay.commandPresentation.id", QStringLiteral("view.fullscreen"));
    connect(lutAction, &QAction::triggered, this, [this] { if (_p->lutButton) _p->lutButton->click(); });
    connect(ocioAction, &QAction::triggered, this, [this](bool checked) { if (_p->ocioButton) _p->ocioButton->setChecked(checked); });
    connect(alphaAction, &QAction::triggered, this, [this](bool checked) { if (_p->alphaButton) _p->alphaButton->setChecked(checked); });
    connect(compareAction, &QAction::triggered, this, [this] { if (_p->compareButton) _p->compareButton->click(); });
    connect(tileAction, &QAction::triggered, this, [this] { if (_p->tileButton) _p->tileButton->click(); });
    connect(fullscreenAction, &QAction::triggered, this, [this] { if (_p->fullscreenButton) _p->fullscreenButton->click(); });
    connect(moreMenu, &QMenu::aboutToShow, this, [this, ocioAction, alphaAction] {
        ocioAction->setChecked(_p->ocioButton && _p->ocioButton->isChecked());
        alphaAction->setChecked(_p->alphaButton && _p->alphaButton->isChecked());
    });
    _p->moreButton->setMenu(moreMenu);
    _p->moreButton->hide();
    toolbarLayout->addWidget(_p->moreButton);
    toolbarLayout->addStretch();
    chromeLayout->addWidget(toolbarRow);

    _p->rootLayout->addWidget(_p->toolbar, 0);

    _p->timecodeLabel = new QLabel(QStringLiteral("00:00:00:00"), this);
    _p->timecodeLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    _p->timecodeLabel->setStyleSheet(
        QStringLiteral(
            "QLabel {"
            " color: %1;"
            " background: %2;"
            " border: 1px solid %3;"
            " border-radius: 8px;"
            " padding: 6px 12px;"
            " font-family: Consolas, 'Courier New', monospace;"
            " font-size: 16px;"
            " font-weight: 700;"
            " }")
            .arg(colorCss(runtimeThemeColor("cgplay.textColor", QColor(Qt::white))),
                 colorCss(blendThemeColor(runtimeThemeColor("cgplay.timelineColor", QColor(kButtonFill)),
                                           runtimeThemeColor("cgplay.viewerColor", QColor(kViewerBg)), 86)),
                 colorCss(runtimeThemeColor("cgplay.borderColor", QColor(QStringLiteral("#303B47"))))));
    _p->timecodeLabel->hide();
    _updateResponsiveChrome();
}

void ViewerWidget::_updateResponsiveChrome()
{
    if (!_p) return;
    const bool compact = width() > 0 && width() < 640;
    for (auto* button : {_p->lutButton, _p->ocioButton, _p->alphaButton,
                         _p->compareButton, _p->tileButton, _p->fullscreenButton}) {
        if (button) button->setVisible(!compact);
    }
    for (auto* separator : {_p->zoomSeparator, _p->colorSeparator, _p->compareSeparator}) {
        if (separator) separator->setVisible(!compact);
    }
    if (_p->moreButton) _p->moreButton->setVisible(compact);
}

void ViewerWidget::_setupViewport()
{
    if (!_p->playback) {
        return;
    }

    const auto context = _p->playback->context();
    const auto style = _p->playback->style();
    if (!context || !style) {
        return;
    }

    _p->viewport = new TlViewport(context, style, this);
    _p->viewport->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    _p->viewport->setMinimumSize(1, 1);
    _p->viewport->setStyleSheet(QStringLiteral("background:%1;").arg(kViewerBg));
    _p->rootLayout->addWidget(_p->viewport, 1);
    _p->viewport->installEventFilter(this);

    if (const auto player = _p->playback->player()) {
        _p->viewport->setPlayer(player);
        _p->viewport->setFrameView(true);
        _p->autoFit = true;

        const auto& info = player->getIOInfo();
        const bool hasVideo = !info.video.empty();
        _p->viewport->setVisible(true);
        if (hasVideo) {
            _mediaW = info.video[0].size.w;
            _mediaH = info.video[0].size.h;
            Q_EMIT resolutionChanged(_mediaW, _mediaH);
        }
        _refreshOverlayGeometry();
    } else {
        // Keep the black viewport as a layout surface even before media is
        // opened.  Hiding it makes Qt shrink ViewerWidget to the 76px chrome
        // size and vertically center the toolbar in the player area.
        _p->viewport->setVisible(true);
    }

    connect(
        _p->viewport,
        &TlViewport::viewPosAndZoomChanged,
        this,
        [this](const ftk::V2I&, double zoom) {
            _p->zoom = zoom;
            _p->autoFit = _p->viewport ? _p->viewport->hasFrameView() : false;
            Q_EMIT zoomChanged(zoom);
            if (!_p->viewId.isEmpty()) {
                Q_EMIT transformChanged(_p->viewId);
            }
            _scheduleOverlayRepaint();
        });

    connect(
        _p->viewport,
        &TlViewport::viewportResized,
        this,
        [this] {
            const QRect rect = overlayGeometry();
            if (!_p->viewId.isEmpty()) {
                if (!_p->hasLastViewportSignalRect || _p->lastViewportSignalRect != rect) {
                    _p->lastViewportSignalRect = rect;
                    _p->hasLastViewportSignalRect = true;
                    Q_EMIT viewportResizedForView(_p->viewId);
                }
            }
            _refreshOverlayGeometry();
        });

    if (!_p->viewId.isEmpty()) {
        Q_EMIT coordinateMapperChanged(_p->viewId);
    }

}

void ViewerWidget::_connectPlayback()
{
    if (!_p->playback) {
        return;
    }

#if CGPLAY_HAS_TLRENDER
    connect(
        _p->playback->signalProxy(),
        &PlaybackServiceSignals::playerReady,
        this,
        [this](const std::shared_ptr<tl::Player>& player) {
            if (!player || !_p->viewport) {
                return;
            }

            _p->viewport->setPlayer(player);
            _p->viewport->setFrameView(true);
            _p->autoFit = true;

            const auto& info = player->getIOInfo();
            const bool hasVideo = !info.video.empty();
            _p->viewport->setVisible(true);
            if (hasVideo) {
                _mediaW = info.video[0].size.w;
                _mediaH = info.video[0].size.h;
                Q_EMIT resolutionChanged(_mediaW, _mediaH);
            }

            if (_p->timecodeLabel) {
                _p->timecodeLabel->setText(_timecodeFromFrame(_p->playback->currentFrame(), _p->playback->fps()));
                _p->timecodeLabel->setVisible(_p->timecodeVisible);
            }
            _refreshOverlayGeometry();
        });
#endif

    connect(
        _p->playback->signalProxy(),
        &PlaybackServiceSignals::fileOpened,
        this,
        [this](const QString& path) {
            if (!_p->shotLabel) {
                return;
            }
            const QFileInfo info(path);
            _p->shotLabel->setText(info.fileName().isEmpty() ? QStringLiteral("viewer") : info.fileName());
            _p->shotLabel->adjustSize();
        });

    connect(
        _p->playback->signalProxy(),
        &PlaybackServiceSignals::fileClosed,
        this,
        &ViewerWidget::clearMedia);

    connect(
        _p->playback->signalProxy(),
        &PlaybackServiceSignals::currentFrameChanged,
        this,
        [this](int frame, int) {
            if (!_p->timecodeLabel) {
                return;
            }
            if (_p->timecodeVisible) {
                const QString timecode = _timecodeFromFrame(frame, _p->playback->fps());
                if (_p->timecodeLabel->text() != timecode) {
                    _p->timecodeLabel->setText(timecode);
                }
                if (!_p->timecodeLabel->isVisible()) {
                    _p->timecodeLabel->show();
                }
            } else if (_p->timecodeLabel->isVisible()) {
                _p->timecodeLabel->hide();
            }
        });

    connect(
        _p->playback->signalProxy(),
        &PlaybackServiceSignals::playbackStateChanged,
        this,
        [this](int) {
            if (_p->timecodeLabel && _p->timecodeVisible && !_p->timecodeLabel->isVisible()) {
                _p->timecodeLabel->show();
            } else if (_p->timecodeLabel && !_p->timecodeVisible && _p->timecodeLabel->isVisible()) {
                _p->timecodeLabel->hide();
            }
        });

    if (_p->ocio) {
        connect(
            _p->ocio.get(),
            &OcioManager::optionsChanged,
            this,
            [this](const tl::OCIOOptions&) {
                _applyOcioOptions();
            });
    }
}

void ViewerWidget::_applyOcioOptions()
{
    if (_p->ocioButton && _p->ocio) {
        const QSignalBlocker blocker(_p->ocioButton);
        _p->ocioButton->setChecked(_p->ocio->isEnabled());
    }

    if (_p->viewport && _p->ocio) {
        _p->viewport->setOCIOOptions(_p->ocio->currentOptions());
    }

    if (_p->viewport) {
        tl::DisplayOptions displayOptions;
        displayOptions.channels = _p->alphaChannelVisible
            ? ftk::ChannelDisplay::Alpha
            : ftk::ChannelDisplay::Color;
        _p->viewport->setDisplayOptions({ displayOptions, displayOptions });
    }
}

void ViewerWidget::_refreshOverlayGeometry()
{
    if (_p->overlay && _p->viewport) {
        const QRect rect = overlayGeometry();
        if (_p->overlay->geometry() != rect) {
            _p->overlay->setGeometry(rect);
            _p->overlay->raise();
        }
        _scheduleOverlayRepaint();
    }

    if (_p->timecodeLabel && _p->viewport) {
        const QRect viewportRect = _p->viewport->geometry();
        const QSize labelSize = _p->timecodeLabel->sizeHint();
        const int x = viewportRect.x() + (viewportRect.width() - labelSize.width()) / 2;
        const int y = viewportRect.bottom() - labelSize.height() - 14;
        const QRect labelRect(x, y, labelSize.width(), labelSize.height());
        if (_p->timecodeLabel->geometry() != labelRect) {
            _p->timecodeLabel->setGeometry(labelRect);
        }
        _p->timecodeLabel->raise();
    }
}

void ViewerWidget::_scheduleOverlayRepaint()
{
    if (!_p->overlay || _p->overlayRepaintQueued) {
        return;
    }

    _p->overlayRepaintQueued = true;
    QTimer::singleShot(0, this, [this] {
        _p->overlayRepaintQueued = false;
        if (_p->overlay) {
            _p->overlay->update();
        }
    });
}

void ViewerWidget::clearMedia()
{
    if (_p->viewport) {
        _p->viewport->clearPlayer();
        _p->viewport->setFrameView(true);
        _p->viewport->setVisible(true);
    }
    _mediaW = 1920;
    _mediaH = 1080;
    if (_p->shotLabel) {
        _p->shotLabel->setText(QStringLiteral("viewer"));
        _p->shotLabel->adjustSize();
    }
    if (_p->timecodeLabel) {
        _p->timecodeLabel->setText(QStringLiteral("00:00:00:00"));
        _p->timecodeLabel->setVisible(_p->timecodeVisible);
    }
    Q_EMIT resolutionChanged(0, 0);
    _refreshOverlayGeometry();
}

void ViewerWidget::focusInEvent(QFocusEvent* event)
{
    QWidget::focusInEvent(event);
    if (!_p->viewId.isEmpty()) {
        Q_EMIT viewActivated(_p->viewId);
    }
}

void ViewerWidget::fitToWindow()
{
    if (!_p->viewport) {
        return;
    }
    _p->viewport->setFrameView(true);
    _p->autoFit = true;
    _refreshOverlayGeometry();
}

void ViewerWidget::zoom1to1()
{
    if (!_p->viewport) {
        return;
    }
    _p->viewport->setFrameView(false);
    _p->viewport->resetZoom();
    _p->autoFit = false;
}

void ViewerWidget::zoomIn()
{
    if (!_p->viewport) {
        return;
    }
    _p->viewport->setFrameView(false);
    _p->viewport->zoomIn();
    _p->autoFit = false;
}

void ViewerWidget::zoomOut()
{
    if (!_p->viewport) {
        return;
    }
    _p->viewport->setFrameView(false);
    _p->viewport->zoomOut();
    _p->autoFit = false;
}

void ViewerWidget::setZoom(double zoom)
{
    if (!_p->viewport) {
        return;
    }
    const double clampedZoom = std::clamp(zoom, 0.05, 32.0);
    _p->viewport->setFrameView(false);
    _p->viewport->setZoom(clampedZoom);
    _p->autoFit = false;
}

void ViewerWidget::setChromeVisible(bool visible)
{
    if (_p->toolbar) {
        _p->toolbar->setVisible(visible);
    }
    _refreshOverlayGeometry();
}

void ViewerWidget::setTimecodeVisible(bool visible)
{
    _p->timecodeVisible = visible;
    if (_p->timecodeLabel) {
        _p->timecodeLabel->setVisible(visible);
    }
    _refreshOverlayGeometry();
}

void ViewerWidget::setAlphaChannelVisible(bool visible)
{
    _p->alphaChannelVisible = visible;
    if (_p->alphaButton && _p->alphaButton->isChecked() != visible) {
        const QSignalBlocker blocker(_p->alphaButton);
        _p->alphaButton->setChecked(visible);
    }
    _applyOcioOptions();
    Q_EMIT alphaChannelChanged(visible);
}

void ViewerWidget::toggleAlphaChannel()
{
    setAlphaChannelVisible(!_p->alphaChannelVisible);
}

bool ViewerWidget::isTimecodeVisible() const
{
    return _p->timecodeVisible;
}

void ViewerWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    _updateResponsiveChrome();
    _refreshOverlayGeometry();
}

void ViewerWidget::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
        return;
    }
    QWidget::dragEnterEvent(event);
}

void ViewerWidget::dropEvent(QDropEvent* event)
{
    const auto urls = event->mimeData()->urls();
    if (!urls.isEmpty()) {
        Q_EMIT droppedFile(urls.constFirst().toLocalFile());
        event->acceptProposedAction();
        return;
    }
    QWidget::dropEvent(event);
}

void ViewerWidget::wheelEvent(QWheelEvent* event)
{
    if (!_p->viewport) {
        QWidget::wheelEvent(event);
        return;
    }

    const QPoint focus = _p->viewport->mapFrom(this, event->position().toPoint());
    if (!_applyWheelZoom(event, focus)) {
        QWidget::wheelEvent(event);
    }
}

bool ViewerWidget::_applyWheelZoom(QWheelEvent* event, const QPoint& focus)
{
    if (!event || !_p->viewport) return false;
    double steps = event->angleDelta().y() / 120.0;
    if (qFuzzyIsNull(steps)) steps = event->pixelDelta().y() / 120.0;
    if (event->inverted()) steps = -steps;
    if (qFuzzyIsNull(steps)) return false;

    _p->viewport->setFrameView(false);
    const double factor = std::pow(1.1, steps);
    const double nextZoom = std::clamp(_p->viewport->zoom() * factor, 0.05, 32.0);
    _p->viewport->setZoom(nextZoom, ftk::V2I(focus.x(), focus.y()));
    _p->autoFit = false;
    event->accept();
    return true;
}

void ViewerWidget::mousePressEvent(QMouseEvent* event)
{
    if (!_p->viewport) {
        QWidget::mousePressEvent(event);
        return;
    }

    if (event->button() == Qt::MiddleButton ||
        (event->button() == Qt::LeftButton && (event->modifiers() & Qt::AltModifier))) {
        _p->viewport->setFrameView(false);
        _p->panning = true;
        _p->panGlobalStart = event->globalPosition().toPoint();
        _p->panThrottle.restart();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton && event->modifiers() == Qt::NoModifier) {
        if (!_p->viewId.isEmpty()) {
            Q_EMIT viewActivated(_p->viewId);
        }
        Q_EMIT viewportClicked();
        event->accept();
        return;
    }

    QWidget::mousePressEvent(event);
}

void ViewerWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (!_p->panning || !_p->viewport) {
        QWidget::mouseMoveEvent(event);
        return;
    }

    const QPoint globalNow = event->globalPosition().toPoint();
    const QPoint delta = globalNow - _p->panGlobalStart;
    if (delta.isNull() || (_p->panThrottle.isValid() && _p->panThrottle.elapsed() < 8)) {
        event->accept();
        return;
    }
    const ftk::V2I current = _p->viewport->viewPos();
    _p->viewport->setViewPosAndZoom(ftk::V2I(current.x + delta.x(), current.y + delta.y()), _p->viewport->zoom());
    _p->panGlobalStart = globalNow;
    _p->panThrottle.restart();
    _p->autoFit = false;
    event->accept();
}

void ViewerWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (_p->panning &&
        (event->button() == Qt::MiddleButton || event->button() == Qt::LeftButton)) {
        _p->panning = false;
        unsetCursor();
        _refreshOverlayGeometry();
        event->accept();
        return;
    }

    QWidget::mouseReleaseEvent(event);
}

bool ViewerWidget::eventFilter(QObject* obj, QEvent* event)
{
    const bool watchingViewport = (obj == _p->viewport);
    const bool watchingOverlay = (obj == _p->overlay);
    if (!watchingViewport && !watchingOverlay) {
        return QWidget::eventFilter(obj, event);
    }

    switch (event->type()) {
    case QEvent::Wheel:
    {
        auto* wheelEvent = static_cast<QWheelEvent*>(event);
        if (!_p->viewport) {
            break;
        }

        QPoint focus = wheelEvent->position().toPoint();
        if (watchingOverlay && _p->overlay) {
            focus = _p->viewport->mapFromGlobal(_p->overlay->mapToGlobal(focus));
        }
        return _applyWheelZoom(wheelEvent, focus);
    }

    case QEvent::MouseButtonPress:
    {
        auto* mouseEvent = static_cast<QMouseEvent*>(event);
        if (mouseEvent->button() == Qt::MiddleButton ||
            (mouseEvent->button() == Qt::LeftButton && (mouseEvent->modifiers() & Qt::AltModifier))) {
            _p->viewport->setFrameView(false);
            _p->panning = true;
            _p->panGlobalStart = mouseEvent->globalPosition().toPoint();
            if (auto* widget = qobject_cast<QWidget*>(obj)) {
                widget->setCursor(Qt::ClosedHandCursor);
            }
            setCursor(Qt::ClosedHandCursor);
            mouseEvent->accept();
            return true;
        }

        if (mouseEvent->button() == Qt::LeftButton && mouseEvent->modifiers() == Qt::NoModifier) {
            const bool drawingMode =
                watchingOverlay &&
                _p->overlay &&
                _p->overlay->property("cgplay.annotationDrawingMode").toBool();
            if (!drawingMode) {
                if (!_p->viewId.isEmpty()) {
                    Q_EMIT viewActivated(_p->viewId);
                }
                Q_EMIT viewportClicked();
            }
        }
        break;
    }

    case QEvent::MouseMove:
    {
        auto* mouseEvent = static_cast<QMouseEvent*>(event);
        if (!_p->panning || !_p->viewport) {
            break;
        }

        const QPoint globalNow = mouseEvent->globalPosition().toPoint();
        const QPoint delta = globalNow - _p->panGlobalStart;
        const ftk::V2I current = _p->viewport->viewPos();
        _p->viewport->setViewPosAndZoom(
            ftk::V2I(current.x + delta.x(), current.y + delta.y()),
            _p->viewport->zoom());
        _p->panGlobalStart = globalNow;
        _p->autoFit = false;
        _refreshOverlayGeometry();
        mouseEvent->accept();
        return true;
    }

    case QEvent::MouseButtonRelease:
    {
        auto* mouseEvent = static_cast<QMouseEvent*>(event);
        if (_p->panning &&
            (mouseEvent->button() == Qt::MiddleButton || mouseEvent->button() == Qt::LeftButton)) {
            _p->panning = false;
            if (auto* widget = qobject_cast<QWidget*>(obj)) {
                widget->unsetCursor();
            }
            unsetCursor();
            _refreshOverlayGeometry();
            mouseEvent->accept();
            return true;
        }
        break;
    }

    default:
        break;
    }

    return QWidget::eventFilter(obj, event);
}

void ViewerWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    drawRuntimeBackdrop(painter, rect(), this);
}

} // namespace cgplay
