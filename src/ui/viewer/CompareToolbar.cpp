// CGPlay CompareToolbar.cpp — v2.0 Studio UI
// RV-style A/B comparison toolbar, restyled for dark theme

#include "CompareToolbar.h"
#include "ui/app/CommandPresentation.h"

#include <tlRender/Timeline/CompareOptions.h>

#include <QHBoxLayout>
#include <QApplication>
#include <QColor>
#include <QToolButton>
#include <QSlider>
#include <QCheckBox>
#include <QLabel>
#include <QKeyEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QSettings>
#include <QTimer>
#include <QDebug>

namespace cgplay {

namespace {

constexpr int kWipeCenterChange = 1 << 0;
constexpr int kWipeRotationChange = 1 << 1;
constexpr int kOverlayChange = 1 << 2;
constexpr int kSliderUpdateIntervalMs = 16;

QColor themeColor(const char* property, const QColor& fallback)
{
    const QColor value(qApp ? qApp->property(property).toString() : QString());
    return value.isValid() ? value : fallback;
}

QString cssColor(QColor color, int alpha = -1)
{
    if (alpha >= 0) color.setAlpha(qBound(0, alpha, 255));
    return color.name(QColor::HexArgb);
}

QColor blendColor(const QColor& foreground, const QColor& background, int amount)
{
    const int inverse = 100 - qBound(0, amount, 100);
    const int bounded = 100 - inverse;
    return QColor((foreground.red() * bounded + background.red() * inverse) / 100,
                  (foreground.green() * bounded + background.green() * inverse) / 100,
                  (foreground.blue() * bounded + background.blue() * inverse) / 100);
}

} // namespace

// ─── Private ────────────────────────────────────────────────────────────────────
struct CompareToolbar::Private
{
    QToolButton* btnA          = nullptr;
    QToolButton* btnB          = nullptr;
    QToolButton* btnWipe       = nullptr;
    QToolButton* btnOverlay    = nullptr;
    QToolButton* btnDifference = nullptr;
    QToolButton* btnHorizontal = nullptr;
    QToolButton* btnVertical   = nullptr;
    QToolButton* btnTile       = nullptr;

    QLabel*      lblWipeX      = nullptr;
    QSlider*     wipeXSlider   = nullptr;
    QLabel*      lblWipeY      = nullptr;
    QSlider*     wipeYSlider   = nullptr;
    QLabel*      lblWipeRot    = nullptr;
    QSlider*     wipeRotSlider = nullptr;

    QLabel*      lblOverlay    = nullptr;
    QSlider*     overlaySlider = nullptr;

    QLabel*      shotALabel    = nullptr;
    QLabel*      shotBLabel    = nullptr;
    QString      shotALabelRaw;
    QString      shotBLabelRaw;

    QCheckBox*   chkAutoClearB = nullptr;
    bool         autoClearB    = true;

    int currentMode = 0;
    std::vector<QToolButton*> modeButtons;
    QTimer* sliderEmitTimer = nullptr;
    int pendingSliderChanges = 0;
};

// ─── Constructor ────────────────────────────────────────────────────────────────
CompareToolbar::CompareToolbar(QWidget* parent)
    : QWidget(parent)
    , _p(std::make_unique<Private>())
{
    setFocusPolicy(Qt::StrongFocus);
    setAcceptDrops(true);
    _buildUI();
}

CompareToolbar::~CompareToolbar() = default;

tl::CompareOptions CompareToolbar::compareOptions() const
{
    tl::CompareOptions opts;
    switch (_p->currentMode) {
    case 0: opts.compare = tl::Compare::A;          break;
    case 1: opts.compare = tl::Compare::B;          break;
    case 2: opts.compare = tl::Compare::Wipe;       break;
    case 3: opts.compare = tl::Compare::Overlay;    break;
    case 4: opts.compare = tl::Compare::Difference; break;
    case 5: opts.compare = tl::Compare::Horizontal; break;
    case 6: opts.compare = tl::Compare::Vertical;   break;
    case 7: opts.compare = tl::Compare::Tile;       break;
    default: opts.compare = tl::Compare::A;         break;
    }

    if (_p->wipeXSlider) {
        opts.wipeCenter.x = _p->wipeXSlider->value() / 1000.f;
        opts.wipeCenter.y = _p->wipeYSlider->value() / 1000.f;
    }
    if (_p->wipeRotSlider) {
        opts.wipeRotation = _p->wipeRotSlider->value() / 10.f;
    }
    if (_p->overlaySlider) {
        opts.overlay = _p->overlaySlider->value() / 1000.f;
    }

    return opts;
}

bool CompareToolbar::isCompareActive() const { return _p->currentMode != 0; }
int  CompareToolbar::compareMode()    const { return _p->currentMode; }

void CompareToolbar::setShotALabel(const QString& label)
{
    _p->shotALabelRaw = label;
    if (_p->shotALabel) _p->shotALabel->setText(QStringLiteral("A: %1").arg(label));
}

void CompareToolbar::setShotBLabel(const QString& label)
{
    _p->shotBLabelRaw = label;
    if (_p->shotBLabel) _p->shotBLabel->setText(QStringLiteral("B: %1").arg(label));
}

QString CompareToolbar::shotALabel() const
{
    return _p->shotALabelRaw;
}

QString CompareToolbar::shotBLabel() const
{
    return _p->shotBLabelRaw;
}

bool CompareToolbar::isAutoClearB() const { return _p->autoClearB; }

void CompareToolbar::setAutoClearB(bool on)
{
    _p->autoClearB = on;
    if (_p->chkAutoClearB) {
        _p->chkAutoClearB->blockSignals(true);
        _p->chkAutoClearB->setChecked(on);
        _p->chkAutoClearB->blockSignals(false);
    }
    QSettings s("CGPlay", "CGPlay");
    s.setValue("compare/autoClearB", on);
    Q_EMIT autoClearBToggled(on);
}

void CompareToolbar::toggleAutoClearB() { setAutoClearB(!_p->autoClearB); }

// ─── Build UI v2.0 ─────────────────────────────────────────────────────────────
void CompareToolbar::_buildUI()
{
    auto* mainLayout = new QHBoxLayout(this);
    mainLayout->setContentsMargins(6, 2, 6, 2);
    mainLayout->setSpacing(4);

    auto makeModeBtn = [&](const QString& text, const QString& tip) -> QToolButton* {
        auto* b = new QToolButton(this);
        b->setText(text);
        b->setToolTip(tip);
        b->setFocusPolicy(Qt::NoFocus);
        b->setCheckable(true);
        b->setAutoExclusive(true);
        b->setFixedSize(34, 28);
        b->setCursor(Qt::PointingHandCursor);
        return b;
    };

    // ── Shot labels ─────────────────────────────────────────────────────────
    _p->shotALabel = new QLabel(QString::fromUtf8("A：--"), this);
    _p->shotALabel->setText(QStringLiteral("A: -"));
    _p->shotALabel->setToolTip(QString::fromUtf8("拖入文件以设置 A 镜头"));
    _p->shotALabel->setAcceptDrops(true);
    _p->shotALabel->setToolTip(QStringLiteral("拖入文件以设置 A 镜头"));
    mainLayout->addWidget(_p->shotALabel);

    _p->shotBLabel = new QLabel(QString::fromUtf8("B：--"), this);
    _p->shotBLabel->setText(QStringLiteral("B: -"));
    _p->shotBLabel->setToolTip(QString::fromUtf8("拖入文件以设置 B 镜头"));
    _p->shotBLabel->setAcceptDrops(true);
    _p->shotBLabel->setToolTip(QStringLiteral("拖入文件以设置 B 镜头"));
    mainLayout->addWidget(_p->shotBLabel);

    // Separator
    auto* sep1 = new QLabel("|", this);
    mainLayout->addWidget(sep1);

    // ── Mode buttons ─────────────────────────────────────────────────────────
    _p->btnA          = makeModeBtn("A", QString::fromUtf8("A 镜头"));
    _p->btnB          = makeModeBtn("B", QString::fromUtf8("B 镜头"));
    _p->btnWipe       = makeModeBtn("W", QString::fromUtf8("擦除对比"));
    _p->btnOverlay    = makeModeBtn("N", QString::fromUtf8("叠加对比"));
    _p->btnDifference = makeModeBtn("D", QString::fromUtf8("差异对比"));
    _p->btnHorizontal = makeModeBtn("H", QString::fromUtf8("水平分屏"));
    _p->btnVertical   = makeModeBtn("V", QString::fromUtf8("垂直分屏"));
    _p->btnTile       = makeModeBtn("T", QString::fromUtf8("平铺对比"));

    _p->btnA->setToolTip(QStringLiteral("显示 A 镜头"));
    _p->btnB->setToolTip(QStringLiteral("显示 B 镜头"));
    _p->btnWipe->setToolTip(QStringLiteral("擦除对比"));
    _p->btnOverlay->setToolTip(QStringLiteral("叠加对比"));
    _p->btnDifference->setToolTip(QStringLiteral("差异对比"));
    _p->btnHorizontal->setToolTip(QStringLiteral("水平分屏"));
    _p->btnVertical->setToolTip(QStringLiteral("垂直分屏"));
    _p->btnTile->setToolTip(QStringLiteral("平铺对比"));
    // B directly selects mode 1; it is deliberately NOT compare.toggleB.
    const QList<QPair<QToolButton*, QString>> commandButtons{
        {_p->btnA, QStringLiteral("compare.modeA")},
        {_p->btnWipe, QStringLiteral("compare.wipe")},
        {_p->btnOverlay, QStringLiteral("compare.overlay")},
        {_p->btnDifference, QStringLiteral("compare.difference")},
        {_p->btnHorizontal, QStringLiteral("compare.horizontal")},
        {_p->btnVertical, QStringLiteral("compare.vertical")},
        {_p->btnTile, QStringLiteral("compare.tile")}};
    for (const auto& entry : commandButtons) entry.first->setProperty("commandId", entry.second);
    for (auto* button : {_p->btnA, _p->btnB, _p->btnWipe, _p->btnOverlay,
                         _p->btnDifference, _p->btnHorizontal, _p->btnVertical, _p->btnTile})
        button->setAccessibleName(button->toolTip());

    _p->btnA->setChecked(true);

    _p->modeButtons = {
        _p->btnA, _p->btnB, _p->btnWipe, _p->btnOverlay,
        _p->btnDifference, _p->btnHorizontal, _p->btnVertical, _p->btnTile
    };

    mainLayout->addWidget(_p->btnA);
    mainLayout->addWidget(_p->btnB);
    mainLayout->addWidget(_p->btnWipe);
    mainLayout->addWidget(_p->btnOverlay);
    mainLayout->addWidget(_p->btnDifference);
    mainLayout->addWidget(_p->btnHorizontal);
    mainLayout->addWidget(_p->btnVertical);
    mainLayout->addWidget(_p->btnTile);

    // Separator
    auto* sep2 = new QLabel("|", this);
    mainLayout->addWidget(sep2);

    // ── Sliders ──────────────────────────────────────────────────────────────
    auto makeSlider = [&](const QString& label) -> std::pair<QLabel*, QSlider*> {
        auto* lbl = new QLabel(label, this);
        mainLayout->addWidget(lbl);

        auto* sl = new QSlider(Qt::Horizontal, this);
        sl->setFocusPolicy(Qt::NoFocus);
        sl->setRange(0, 1000);
        sl->setValue(500);
        sl->setFixedWidth(50);
        mainLayout->addWidget(sl);
        return {lbl, sl};
    };

    std::tie(_p->lblWipeX, _p->wipeXSlider) = makeSlider("WX");
    std::tie(_p->lblWipeY, _p->wipeYSlider) = makeSlider("WY");
    std::tie(_p->lblWipeRot, _p->wipeRotSlider) = makeSlider("R");

    _p->wipeRotSlider->setRange(-1800, 1800);
    _p->wipeRotSlider->setValue(0);

    std::tie(_p->lblOverlay, _p->overlaySlider) = makeSlider("Mix");

    // Separator
    auto* sep3 = new QLabel("|", this);
    mainLayout->addWidget(sep3);

    // ── Auto-clear B ─────────────────────────────────────────────────────────
    _p->chkAutoClearB = new QCheckBox(QString::fromUtf8("自动"), this);
    _p->chkAutoClearB->setFocusPolicy(Qt::NoFocus);
    _p->chkAutoClearB->setText(QStringLiteral("自动清除 B"));
    _p->chkAutoClearB->setProperty("commandId", QStringLiteral("compare.autoClearB"));
    updateCommandPresentationState(_p->chkAutoClearB, QStringLiteral("自动清除 B"),
        QStringLiteral("载入新的 A 镜头时自动清除 B"));
    {
        QSettings s("CGPlay", "CGPlay");
        _p->autoClearB = s.value("compare/autoClearB", true).toBool();
    }
    _p->chkAutoClearB->setChecked(_p->autoClearB);
    connect(_p->chkAutoClearB, &QCheckBox::toggled, this, [this](bool checked) {
        _p->autoClearB = checked;
        QSettings s("CGPlay", "CGPlay");
        s.setValue("compare/autoClearB", checked);
        Q_EMIT autoClearBToggled(checked);
    });
    mainLayout->addWidget(_p->chkAutoClearB);

    mainLayout->addStretch();

    // ── Mode connections ──────────────────────────────────────────────────────
    auto connectMode = [&](QToolButton* btn, int mode) {
        connect(btn, &QToolButton::clicked, this, [this, mode] {
            setCompareMode(mode);
        });
    };
    connectMode(_p->btnA,          0);
    connectMode(_p->btnB,          1);
    connectMode(_p->btnWipe,       2);
    connectMode(_p->btnOverlay,    3);
    connectMode(_p->btnDifference, 4);
    connectMode(_p->btnHorizontal, 5);
    connectMode(_p->btnVertical,   6);
    connectMode(_p->btnTile,       7);

    _p->sliderEmitTimer = new QTimer(this);
    _p->sliderEmitTimer->setSingleShot(true);
    _p->sliderEmitTimer->setTimerType(Qt::PreciseTimer);
    _p->sliderEmitTimer->setInterval(kSliderUpdateIntervalMs);
    connect(_p->sliderEmitTimer, &QTimer::timeout, this, &CompareToolbar::_flushSliderChanges);

    // Keep live feedback while coalescing repeated viewport redraw requests.
    connect(_p->wipeXSlider, &QSlider::valueChanged, this, [this](int v) {
        _p->lblWipeX->setText(QString("WX:%1").arg(v / 1000.f, 0, 'f', 2));
        _scheduleSliderChanges(kWipeCenterChange);
    });
    connect(_p->wipeYSlider, &QSlider::valueChanged, this, [this](int v) {
        _p->lblWipeY->setText(QString("WY:%1").arg(v / 1000.f, 0, 'f', 2));
        _scheduleSliderChanges(kWipeCenterChange);
    });
    connect(_p->wipeRotSlider, &QSlider::valueChanged, this, [this](int v) {
        const float deg = v / 10.f;
        _p->lblWipeRot->setText(QStringLiteral("R:%1 deg").arg(deg, 0, 'f', 0));
        _scheduleSliderChanges(kWipeRotationChange);
    });
    connect(_p->overlaySlider, &QSlider::valueChanged, this, [this](int v) {
        const float val = v / 1000.f;
        _p->lblOverlay->setText(QString("Mix:%1").arg(val, 0, 'f', 2));
        _scheduleSliderChanges(kOverlayChange);
    });

    const auto commitSlider = [this]() {
        _scheduleSliderChanges(0, true);
    };
    connect(_p->wipeXSlider, &QSlider::sliderReleased, this, commitSlider);
    connect(_p->wipeYSlider, &QSlider::sliderReleased, this, commitSlider);
    connect(_p->wipeRotSlider, &QSlider::sliderReleased, this, commitSlider);
    connect(_p->overlaySlider, &QSlider::sliderReleased, this, commitSlider);

    for (auto* button : findChildren<QToolButton*>(QString(), Qt::FindDirectChildrenOnly)) {
        button->setFocusPolicy(Qt::NoFocus);
    }
    for (auto* slider : findChildren<QSlider*>(QString(), Qt::FindDirectChildrenOnly)) {
        slider->setFocusPolicy(Qt::NoFocus);
    }
    for (auto* checkBox : findChildren<QCheckBox*>(QString(), Qt::FindDirectChildrenOnly)) {
        checkBox->setFocusPolicy(Qt::NoFocus);
    }

    const auto applyRuntimeTheme = [this, sep1, sep2, sep3] {
        const QColor toolbar = themeColor("cgplay.toolbarColor", QColor(QStringLiteral("#0D1520")));
        const QColor panel = themeColor("cgplay.panelColor", QColor(QStringLiteral("#101824")));
        const QColor text = themeColor("cgplay.textColor", QColor(QStringLiteral("#D8DEE7")));
        const QColor border = themeColor("cgplay.borderColor", QColor(255, 255, 255, 22));
        const QColor accent = themeColor("cgplay.accentColor", QColor(QStringLiteral("#FF8A3D")));
        const QColor muted = blendColor(text, panel, 58);
        const QColor hover = blendColor(accent, panel, 18);
        setStyleSheet(QStringLiteral("background:%1;").arg(cssColor(toolbar)));
        const QString buttonStyle = QStringLiteral(
            "QToolButton{background:%1;color:%2;border:1px solid %3;border-radius:4px;font-size:11px;font-weight:700;}"
            "QToolButton:hover{background:%4;color:%5;border-color:%6;}"
            "QToolButton:checked{background:%7;color:%8;border-color:%7;}")
            .arg(cssColor(panel), cssColor(muted), cssColor(border), cssColor(hover), cssColor(text),
                 cssColor(blendColor(accent, border, 55)), cssColor(accent), cssColor(toolbar));
        for (auto* button : _p->modeButtons) button->setStyleSheet(buttonStyle);
        const QString shotStyle = QStringLiteral(
            "color:%1;font-size:10px;font-weight:700;background:%2;padding:3px 8px;min-width:60px;"
            "border:1px solid %3;border-radius:4px;")
            .arg(cssColor(accent), cssColor(panel), cssColor(blendColor(accent, border, 45)));
        _p->shotALabel->setStyleSheet(shotStyle);
        _p->shotBLabel->setStyleSheet(shotStyle);
        for (auto* separator : {sep1, sep2, sep3}) {
            separator->setStyleSheet(QStringLiteral("color:%1;font-size:12px;background:transparent;")
                .arg(cssColor(border)));
        }
        const QString labelStyle = QStringLiteral(
            "color:%1;font-size:10px;font-weight:600;background:transparent;").arg(cssColor(text));
        const QString sliderStyle = QStringLiteral(
            "QSlider::groove:horizontal{height:3px;background:%1;border-radius:2px;}"
            "QSlider::handle:horizontal{width:8px;height:12px;background:%2;border-radius:6px;margin:-4px 0;}"
            "QSlider::sub-page:horizontal{background:%2;border-radius:2px;}")
            .arg(cssColor(blendColor(border, panel, 45)), cssColor(accent));
        for (auto* label : {_p->lblWipeX, _p->lblWipeY, _p->lblWipeRot, _p->lblOverlay}) label->setStyleSheet(labelStyle);
        for (auto* slider : {_p->wipeXSlider, _p->wipeYSlider, _p->wipeRotSlider, _p->overlaySlider}) slider->setStyleSheet(sliderStyle);
        _p->chkAutoClearB->setStyleSheet(QStringLiteral(
            "QCheckBox{color:%1;font-size:11px;background:transparent;spacing:4px;}"
            "QCheckBox::indicator{width:14px;height:14px;}"
            "QCheckBox::indicator:unchecked{background:%2;border:1px solid %3;border-radius:3px;}"
            "QCheckBox::indicator:checked{background:%4;border:1px solid %4;border-radius:3px;}")
            .arg(cssColor(text), cssColor(panel), cssColor(border), cssColor(accent)));
    };
    applyRuntimeTheme();
    if (qApp) connect(qApp, &QApplication::paletteChanged, this,
                      [applyRuntimeTheme](const QPalette&) { applyRuntimeTheme(); });
}

void CompareToolbar::_scheduleSliderChanges(int changes, bool immediate)
{
    _p->pendingSliderChanges |= changes;
    if (immediate) {
        if (_p->sliderEmitTimer) {
            _p->sliderEmitTimer->stop();
        }
        _flushSliderChanges();
    } else if (_p->sliderEmitTimer && !_p->sliderEmitTimer->isActive()) {
        _p->sliderEmitTimer->start();
    }
}

void CompareToolbar::_flushSliderChanges()
{
    const int changes = _p->pendingSliderChanges;
    if (!changes) {
        return;
    }
    _p->pendingSliderChanges = 0;

    if (changes & kWipeCenterChange) {
        Q_EMIT wipeCenterChanged(wipeCenterX(), wipeCenterY());
    }
    if (changes & kWipeRotationChange) {
        Q_EMIT wipeRotationChanged(wipeRotation());
    }
    if (changes & kOverlayChange) {
        Q_EMIT overlayChanged(overlayAmount());
    }
    Q_EMIT compareOptionsChanged(compareOptions());
}

// ─── Slots ───────────────────────────────────────────────────────────────────────
void CompareToolbar::setCompareMode(int mode)
{
    if (mode < 0 || mode > 7) return;
    _p->currentMode = mode;

    if (mode >= 0 && mode < static_cast<int>(_p->modeButtons.size()))
        _p->modeButtons[mode]->setChecked(true);

    Q_EMIT compareModeChanged(mode);
    Q_EMIT compareOptionsChanged(compareOptions());
}

void CompareToolbar::setWipeCenter(float x, float y)
{
    if (_p->wipeXSlider) {
        _p->wipeXSlider->blockSignals(true);
        _p->wipeXSlider->setValue(static_cast<int>(x * 1000.f));
        _p->wipeXSlider->blockSignals(false);
    }
    if (_p->wipeYSlider) {
        _p->wipeYSlider->blockSignals(true);
        _p->wipeYSlider->setValue(static_cast<int>(y * 1000.f));
        _p->wipeYSlider->blockSignals(false);
    }
}

void CompareToolbar::setWipeRotation(float degrees)
{
    if (_p->wipeRotSlider) {
        _p->wipeRotSlider->blockSignals(true);
        _p->wipeRotSlider->setValue(static_cast<int>(degrees * 10.f));
        _p->wipeRotSlider->blockSignals(false);
    }
}

void CompareToolbar::setOverlay(float value)
{
    if (_p->overlaySlider) {
        _p->overlaySlider->blockSignals(true);
        _p->overlaySlider->setValue(static_cast<int>(value * 1000.f));
        _p->overlaySlider->blockSignals(false);
    }
}

void CompareToolbar::toggleCompare()
{
    if (_p->currentMode == 0)
        setCompareMode(1);
    else
        setCompareMode(0);
}

void CompareToolbar::keyPressEvent(QKeyEvent* event)
{
    if (event->modifiers() != Qt::NoModifier) {
        QWidget::keyPressEvent(event);
        return;
    }
    switch (event->key()) {
    case Qt::Key_A: setCompareMode(0); return;
    case Qt::Key_B: toggleCompare();   return;
    case Qt::Key_W: setCompareMode(2); return;
    case Qt::Key_N: setCompareMode(3); return;
    case Qt::Key_D: setCompareMode(4); return;
    case Qt::Key_H: setCompareMode(5); return;
    case Qt::Key_V: setCompareMode(6); return;
    case Qt::Key_T: setCompareMode(7); return;
    case Qt::Key_Escape: setCompareMode(0); return;
    default: break;
    }
    QWidget::keyPressEvent(event);
}

void CompareToolbar::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
    else
        QWidget::dragEnterEvent(event);
}

void CompareToolbar::dropEvent(QDropEvent* event)
{
    const auto urls = event->mimeData()->urls();
    if (urls.isEmpty()) { QWidget::dropEvent(event); return; }
    QString path = urls.first().toLocalFile();
    QPoint dropPos = event->position().toPoint();

    if (_p->shotALabel) {
        QRect rA = _p->shotALabel->geometry();
        rA.adjust(-4, -4, 4, 4);
        if (rA.contains(dropPos)) {
            Q_EMIT shotAFileDropped(path);
            event->acceptProposedAction();
            return;
        }
    }
    if (_p->shotBLabel) {
        QRect rB = _p->shotBLabel->geometry();
        rB.adjust(-4, -4, 4, 4);
        if (rB.contains(dropPos)) {
            Q_EMIT shotBFileDropped(path);
            event->acceptProposedAction();
            return;
        }
    }
    Q_EMIT shotAFileDropped(path);
    event->acceptProposedAction();
}

float CompareToolbar::wipeCenterX() const
{
    return _p->wipeXSlider ? _p->wipeXSlider->value() / 1000.f : 0.5f;
}

float CompareToolbar::wipeCenterY() const
{
    return _p->wipeYSlider ? _p->wipeYSlider->value() / 1000.f : 0.5f;
}

float CompareToolbar::wipeRotation() const
{
    return _p->wipeRotSlider ? _p->wipeRotSlider->value() / 10.f : 0.f;
}

float CompareToolbar::overlayAmount() const
{
    return _p->overlaySlider ? _p->overlaySlider->value() / 1000.f : 0.5f;
}

} // namespace cgplay
