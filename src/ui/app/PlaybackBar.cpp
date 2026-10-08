#include "PlaybackBar.h"
#include "CommandPresentation.h"

#include "playback/api/IPlaybackService.h"

#include <QHBoxLayout>
#include <QApplication>
#include <QActionGroup>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QPainterPath>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QTimer>
#include <QVariant>

namespace cgplay {

static const char* kBg = "rgba(15,20,26,0.90)";
static const char* kBorder = "rgba(255,255,255,0.12)";
static const char* kAccent = "#FF8A3D";
static const char* kText = "#D8DEE7";
static const char* kSec = "#BEC8D5";

namespace {

QColor themeColor(const char* propertyName, const QColor& fallback)
{
    if (!qApp) return fallback;
    const QColor value(qApp->property(propertyName).toString());
    return value.isValid() ? value : fallback;
}

bool lightTheme()
{
    return qApp && qApp->property("cgplay.themeMode").toString().compare(QStringLiteral("light"), Qt::CaseInsensitive) == 0;
}

QString normalizeTranslationMode(const QString& modeId)
{
    const QString normalized = modeId.trimmed().toLower();
    if (normalized == QStringLiteral("high-quality") ||
        normalized == QStringLiteral("manual-high-quality")) {
        return QStringLiteral("high-quality");
    }
    return QStringLiteral("quick-playback");
}

QString translationModeLabel(const QString& modeId)
{
    return normalizeTranslationMode(modeId) == QStringLiteral("high-quality")
        ? QString::fromUtf8(u8"高质")
        : QString::fromUtf8(u8"快速");
}

QString speedLabel(double multiplier)
{
    return QStringLiteral("%1x").arg(multiplier, 0, 'f', multiplier < 1.0 ? 2 : 1);
}

enum class MediaGlyph {
    Prev,
    Play,
    Pause,
    Next,
};

class VolumePopupWidget final : public QWidget
{
public:
    VolumePopupWidget()
        : QWidget(nullptr, Qt::Popup | Qt::FramelessWindowHint)
    {
        setObjectName(QStringLiteral("PlaybackBarVolumePopup"));
        setAttribute(Qt::WA_TranslucentBackground, true);
        setAttribute(Qt::WA_NoSystemBackground, true);
        setAutoFillBackground(false);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(18, 23, 29, 230));
        painter.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 7, 7);
    }
};

class SmoothVolumeSlider : public QSlider
{
public:
    explicit SmoothVolumeSlider(QWidget* parent = nullptr)
        : QSlider(Qt::Horizontal, parent)
    {
        setFocusPolicy(Qt::NoFocus);
        setMouseTracking(true);
        setTracking(true);
        setSingleStep(1);
        setPageStep(25);
    }

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton) {
            _setValueFromPosition(event->position().toPoint());
            event->accept();
            return;
        }
        QSlider::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (event->buttons() & Qt::LeftButton) {
            _setValueFromPosition(event->position().toPoint());
            event->accept();
            return;
        }
        QSlider::mouseMoveEvent(event);
    }

private:
    void _setValueFromPosition(const QPoint& pos)
    {
        QStyleOptionSlider opt;
        initStyleOption(&opt);
        const QRect groove = style()->subControlRect(
            QStyle::CC_Slider, &opt, QStyle::SC_SliderGroove, this);
        const QRect handle = style()->subControlRect(
            QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, this);

        const int sliderMin = groove.x();
        const int sliderMax = groove.right() - handle.width() + 1;
        const int x = qBound(sliderMin, pos.x() - handle.width() / 2, sliderMax);
        const int span = qMax(1, sliderMax - sliderMin);
        const double ratio = static_cast<double>(x - sliderMin) / static_cast<double>(span);
        setSliderPosition(minimum() + qRound(ratio * (maximum() - minimum())));
    }
};

QIcon makeGlyphIcon(MediaGlyph glyph, const QColor& baseColor)
{
    auto render = [&](const QColor& color) {
        QPixmap px(30, 30);
        px.fill(Qt::transparent);
        QPainter p(&px);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(color);

        if (glyph == MediaGlyph::Play) {
            QPainterPath path;
            path.moveTo(8.5, 6.0);
            path.lineTo(23.0, 15.0);
            path.lineTo(8.5, 24.0);
            path.closeSubpath();
            p.drawPath(path);
        } else if (glyph == MediaGlyph::Pause) {
            p.drawRoundedRect(QRectF(7.6, 6.4, 5.8, 17.2), 1.7, 1.7);
            p.drawRoundedRect(QRectF(16.6, 6.4, 5.8, 17.2), 1.7, 1.7);
        } else if (glyph == MediaGlyph::Prev) {
            QPainterPath pathA;
            pathA.moveTo(12.0, 7.0);
            pathA.lineTo(5.0, 14.0);
            pathA.lineTo(12.0, 21.0);
            pathA.closeSubpath();
            QPainterPath pathB;
            pathB.moveTo(21.0, 7.0);
            pathB.lineTo(14.0, 14.0);
            pathB.lineTo(21.0, 21.0);
            pathB.closeSubpath();
            p.drawPath(pathA);
            p.drawPath(pathB);
        } else if (glyph == MediaGlyph::Next) {
            QPainterPath pathA;
            pathA.moveTo(7.0, 7.0);
            pathA.lineTo(14.0, 14.0);
            pathA.lineTo(7.0, 21.0);
            pathA.closeSubpath();
            QPainterPath pathB;
            pathB.moveTo(16.0, 7.0);
            pathB.lineTo(23.0, 14.0);
            pathB.lineTo(16.0, 21.0);
            pathB.closeSubpath();
            p.drawPath(pathA);
            p.drawPath(pathB);
        }
        return px;
    };

    QIcon icon;
    icon.addPixmap(render(baseColor), QIcon::Normal, QIcon::Off);
    icon.addPixmap(render(QColor("#FFFFFF")), QIcon::Active, QIcon::Off);
    icon.addPixmap(render(QColor("#FFFFFF")), QIcon::Selected, QIcon::Off);
    icon.addPixmap(render(QColor("#FFFFFF")), QIcon::Normal, QIcon::On);
    return icon;
}

QPixmap renderVolumeIcon(bool muted)
{
    QPixmap px(24, 24);
    px.fill(Qt::transparent);
    QPainter painter(&px);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(QColor("#F4F7FB"), 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(QColor("#F4F7FB"));
    painter.drawRoundedRect(QRectF(3.5, 9.0, 4.0, 6.0), 1.0, 1.0);
    QPainterPath speaker;
    speaker.moveTo(7.0, 9.0);
    speaker.lineTo(12.0, 5.5);
    speaker.lineTo(12.0, 18.5);
    speaker.lineTo(7.0, 15.0);
    speaker.closeSubpath();
    painter.drawPath(speaker);
    if (muted) {
        painter.setPen(QPen(QColor("#FF5B64"), 2.8, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(QPointF(14.5, 8.0), QPointF(20.5, 16.0));
        painter.drawLine(QPointF(20.5, 8.0), QPointF(14.5, 16.0));
    } else {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor("#F4F7FB"), 1.7, Qt::SolidLine, Qt::RoundCap));
        painter.drawArc(QRectF(11.5, 7.5, 6.0, 9.0), -55 * 16, 110 * 16);
        painter.drawArc(QRectF(11.0, 5.0, 10.0, 14.0), -50 * 16, 100 * 16);
    }
    return px;
}

QIcon playStateIcon(bool playing, const QColor& color = QColor("#FFF8F1"))
{
    return makeGlyphIcon(playing ? MediaGlyph::Pause : MediaGlyph::Play, color);
}

} // namespace

PlaybackBar::PlaybackBar(std::shared_ptr<IPlaybackService> playback, QWidget* parent)
    : QWidget(parent)
    , _playback(std::move(playback))
{
    setFixedHeight(54);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(14, 6, 14, 7);
    layout->setSpacing(6);
    layout->setAlignment(Qt::AlignVCenter);

    _btnPrev = _makeBtn(QString(), QString::fromUtf8("上一帧"), 30);
    _btnPrev->setIcon(makeGlyphIcon(MediaGlyph::Prev, QColor("#F3F7FC")));
    _btnPrev->setIconSize(QSize(18, 18));
    _btnPrev->setObjectName(QStringLiteral("PlaybackBarPreviousFrame"));
    _btnPrev->setProperty("commandId", QStringLiteral("playback.previousFrame"));
    layout->addWidget(_btnPrev);

    _btnPlay = _makeBtn(QString(), QString::fromUtf8("播放 / 暂停"), 36, true);
    _btnPlay->setIcon(playStateIcon(false));
    _btnPlay->setIconSize(QSize(22, 22));
    _btnPlay->setObjectName(QStringLiteral("PlaybackBarPlayToggle"));
    _btnPlay->setProperty("commandId", QStringLiteral("playback.toggle"));
    layout->addWidget(_btnPlay);

    _btnNext = _makeBtn(QString(), QString::fromUtf8("下一帧"), 30);
    _btnNext->setIcon(makeGlyphIcon(MediaGlyph::Next, QColor("#F3F7FC")));
    _btnNext->setIconSize(QSize(18, 18));
    _btnNext->setObjectName(QStringLiteral("PlaybackBarNextFrame"));
    _btnNext->setProperty("commandId", QStringLiteral("playback.nextFrame"));
    layout->addWidget(_btnNext);

    layout->addStretch();

    _lblFrame = new QLabel("00:00:00:00", this);
    _lblFrame->setAlignment(Qt::AlignCenter);
    _lblFrame->setMinimumWidth(132);
    _lblFrame->setStyleSheet(QString(
        "QLabel{color:%1;background:rgba(9,13,18,0.60);border:1px solid rgba(255,138,61,0.38);"
        "border-radius:7px;padding:5px 16px;font-size:12px;font-family:'Consolas','Segoe UI';font-weight:800;letter-spacing:0.25px;}")
        .arg(kAccent));
    layout->addWidget(_lblFrame);

    layout->addStretch();

    _lblFPS = new QLabel("24 FPS", this);
    _lblFPS->setAlignment(Qt::AlignCenter);
    _lblFPS->setMinimumWidth(60);
    _lblFPS->setStyleSheet(QString(
        "QLabel{color:%1;background:rgba(17,23,30,0.66);border:1px solid rgba(255,255,255,0.10);"
        "border-radius:7px;padding:4px 9px;font-size:10px;font-weight:650;}")
        .arg(kSec));
    layout->addWidget(_lblFPS);

    _btnTranslation = _makeBtn(QString::fromUtf8("译"), QString::fromUtf8("启动翻译"), 30);
    _btnTranslation->setObjectName(QStringLiteral("PlaybackBarTranslationToggle"));
    _btnTranslation->setProperty("commandId", QStringLiteral("translation.toggle"));
    _btnTranslation->setFixedSize(40, 32);
    _btnTranslation->setCheckable(true);
    _btnTranslation->setToolButtonStyle(Qt::ToolButtonTextOnly);
    layout->addWidget(_btnTranslation);
    connect(_btnTranslation, &QToolButton::toggled, this, &PlaybackBar::translationVisibilityToggled);

    _btnTranslationMode = _makeBtn(QString(), QString::fromUtf8(u8"翻译模式"), 30);
    _btnTranslationMode->setObjectName(QStringLiteral("PlaybackBarTranslationMode"));
    _btnTranslationMode->setProperty("commandId", QStringLiteral("translation.mode"));
    _btnTranslationMode->setFixedSize(40, 32);
    _btnTranslationMode->setToolButtonStyle(Qt::ToolButtonTextOnly);
    _btnTranslationMode->setPopupMode(QToolButton::InstantPopup);
    auto* modeMenu = new QMenu(_btnTranslationMode);
    auto* modeGroup = new QActionGroup(modeMenu);
    modeGroup->setExclusive(true);
    _actQuickMode = modeMenu->addAction(QString::fromUtf8(u8"快速播放"));
    _actQuickMode->setObjectName(QStringLiteral("PlaybackBarTranslationModeQuick"));
    _actQuickMode->setCheckable(true);
    _actQuickMode->setData(QStringLiteral("quick-playback"));
    modeGroup->addAction(_actQuickMode);
    _actHighQualityMode = modeMenu->addAction(QString::fromUtf8(u8"高质量翻译"));
    _actHighQualityMode->setObjectName(QStringLiteral("PlaybackBarTranslationModeHighQuality"));
    _actHighQualityMode->setCheckable(true);
    _actHighQualityMode->setData(QStringLiteral("high-quality"));
    modeGroup->addAction(_actHighQualityMode);
    connect(modeGroup, &QActionGroup::triggered, this, [this](QAction* action) {
        if (!action) {
            return;
        }
        const QString normalized = normalizeTranslationMode(action->data().toString());
        if (_translationMode == normalized) {
            _updateTranslationModeButton();
            return;
        }
        _translationMode = normalized;
        _updateTranslationModeButton();
        Q_EMIT translationModeChanged(_translationMode);
    });
    _btnTranslationMode->setMenu(modeMenu);
    layout->addWidget(_btnTranslationMode);
    _updateTranslationModeButton();

    _btnSpeed = _makeBtn(speedLabel(_speedMultiplier), QString::fromUtf8(u8"鎾斁鍊嶉€熷垏鎹㈣彍鍗�"), 48);
    _btnSpeed->setObjectName(QStringLiteral("PlaybackBarSpeed"));
    _btnSpeed->setProperty("commandId", QStringLiteral("playback.setSpeed"));
    _btnSpeed->setFixedSize(40, 32);
    _btnSpeed->setToolTip(QString::fromUtf8(u8"播放倍速"));
    _btnSpeed->setToolButtonStyle(Qt::ToolButtonTextOnly);
    _btnSpeed->setPopupMode(QToolButton::InstantPopup);
    auto* speedMenu = new QMenu(_btnSpeed);
    const QList<double> speedOptions = {0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 4.0, 8.0};
    for (const double multiplier : speedOptions) {
        auto* action = speedMenu->addAction(speedLabel(multiplier));
        connect(action, &QAction::triggered, this, [this, multiplier] {
            _speedMultiplier = multiplier;
            if (_playback) {
                const double mediaFps = _playback->fps() > 0.0 ? _playback->fps() : 24.0;
                _playback->setSpeed(mediaFps * multiplier);
            }
            if (_btnSpeed) {
                _btnSpeed->setText(speedLabel(_speedMultiplier));
                updateCommandPresentationState(_btnSpeed, QStringLiteral("播放倍速"),
                    QStringLiteral("当前：%1").arg(speedLabel(_speedMultiplier)));
            }
        });
    }
    _btnSpeed->setMenu(speedMenu);
    layout->addWidget(_btnSpeed);

    _btnLoop = _makeBtn("LP", QString::fromUtf8("循环模式"), 30);
    _btnLoop->setFixedSize(40, 32);
    for (QToolButton* compactTextButton : {_btnTranslation, _btnTranslationMode, _btnSpeed, _btnLoop}) {
        compactTextButton->setStyleSheet(compactTextButton->styleSheet() + QStringLiteral(
            "QToolButton{font-size:12px;font-weight:700;padding:0px 3px;}"));
    }
    _btnLoop->setObjectName(QStringLiteral("PlaybackBarLoop"));
    _btnLoop->setProperty("commandId", QStringLiteral("playback.loop"));
    layout->addWidget(_btnLoop);

    _btnMute = _makeBtn(QString(), QString::fromUtf8("静音"), 29);
    const bool initiallyMuted = _playback && _playback->isMuted();
    _btnMute->setIcon(QIcon(renderVolumeIcon(initiallyMuted)));
    _btnMute->setIconSize(QSize(18, 18));
    _btnMute->setObjectName(QStringLiteral("PlaybackBarMute"));
    _btnMute->setProperty("commandId", QStringLiteral("audio.openVolume"));
    _btnMute->setProperty("cgplay.preserveStateIcon", true);
    // The main speaker button only opens the volume popup. Mute selection is
    // represented by the popup action, so this button must not take on the
    // global checked/accent background.
    _btnMute->setCheckable(false);
    layout->addWidget(_btnMute);

    auto* volumePopup = new VolumePopupWidget;
    volumePopup->setProperty("cgplay.commandPresentation.owner", QVariant::fromValue<QObject*>(this));
    volumePopup->setStyleSheet(QStringLiteral(
        "QToolButton#PlaybackBarMutePopup{background:transparent;color:#F4F7FB;border:0;"
        "padding:6px 10px;text-align:left;}"
        "QToolButton#PlaybackBarMutePopup:checked{color:#FFFFFF;background:rgba(255,138,61,51);}"));
    auto* volumeLayout = new QHBoxLayout(volumePopup);
    volumeLayout->setContentsMargins(8, 5, 8, 5);
    volumeLayout->setSpacing(8);
    auto* volumeLabel = new QLabel(QStringLiteral("100%"), volumePopup);
    volumeLabel->setMinimumWidth(38);
    volumeLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    volumeLabel->setStyleSheet(QStringLiteral("color:#F4F7FB;font-size:11px;font-weight:700;"));

    _volSlider = new SmoothVolumeSlider(volumePopup);
    _volSlider->setRange(0, 1000);
    _volSlider->setValue(1000);
    _volSlider->setFixedWidth(150);
    _volSlider->setMinimumHeight(28);
    _volSlider->setStyleSheet(QString(
        "QSlider::groove:horizontal{background:rgba(255,255,255,36);height:3px;border-radius:2px;}"
        "QSlider::handle:horizontal{background:qradialgradient(cx:0.35,cy:0.35,radius:0.8,fx:0.35,fy:0.35,stop:0 #F3F7FC,stop:1 rgba(255,255,255,102));width:14px;height:14px;margin:-6px 0;border-radius:7px;border:1px solid rgba(255,255,255,61);}"
        "QSlider::sub-page:horizontal{background:qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 rgba(255,140,50,230),stop:1 rgba(139,190,232,224));border-radius:2px;}")
        .arg(kBorder, kAccent));
    volumeLayout->addWidget(_volSlider);
    volumeLayout->addWidget(volumeLabel);
    auto* mutePopupButton = new QToolButton(volumePopup);
    mutePopupButton->setObjectName(QStringLiteral("PlaybackBarMutePopup"));
    mutePopupButton->setProperty("commandId", QStringLiteral("audio.toggleMute"));
    mutePopupButton->setText(QString::fromUtf8(u8"静音"));
    mutePopupButton->setCheckable(true);
    mutePopupButton->setChecked(initiallyMuted);
    volumeLayout->addWidget(mutePopupButton);
    connect(_btnMute, &QToolButton::clicked, this, [this, volumePopup, mutePopupButton] {
        mutePopupButton->setChecked(_playback && _playback->isMuted());
        volumePopup->adjustSize();
        const QPoint global = _btnMute->mapToGlobal(
            QPoint(_btnMute->width() - volumePopup->width(), -volumePopup->height() - 4));
        volumePopup->move(global);
        volumePopup->show();
        volumePopup->raise();
    });

    if (_playback) {
        _volumeApplyTimer = new QTimer(this);
        _volumeApplyTimer->setSingleShot(true);
        _volumeApplyTimer->setTimerType(Qt::PreciseTimer);
        connect(_volumeApplyTimer, &QTimer::timeout, this, &PlaybackBar::_applyPendingVolume);

        connect(_btnPlay, &QToolButton::clicked, this, [this] { _playback->togglePlay(); });
        connect(_btnPrev, &QToolButton::clicked, this, [this] { _playback->prevFrame(); });
        connect(_btnNext, &QToolButton::clicked, this, [this] { _playback->nextFrame(); });
        connect(mutePopupButton, &QToolButton::clicked, this, [this, mutePopupButton] {
            _playback->setMute(mutePopupButton->isChecked());
        });
        connect(_playback->signalProxy(), &PlaybackServiceSignals::playbackStateChanged, this, &PlaybackBar::_onStateChanged);

        connect(_volSlider, &QSlider::valueChanged, this, &PlaybackBar::_queueVolumeValue);
        connect(_volSlider, &QSlider::sliderReleased, this, &PlaybackBar::_applyPendingVolume);
        connect(_volSlider, &QSlider::valueChanged, this, [volumeLabel](int value) {
            volumeLabel->setText(QStringLiteral("%1%").arg(qRound(value / 10.0)));
        });

        connect(_playback->signalProxy(), &PlaybackServiceSignals::volumeChanged, this, [this](float value) {
            if (_volSlider->isSliderDown()) {
                return;
            }
            _pendingVolumeValue = qBound(0, qRound(value * 1000.0f), 1000);
            if (_volSlider->value() != _pendingVolumeValue) {
                _volSlider->blockSignals(true);
                _volSlider->setValue(_pendingVolumeValue);
                _volSlider->blockSignals(false);
            }
        });

        connect(_playback->signalProxy(), &PlaybackServiceSignals::muteChanged, this, [this](bool muted) {
            const QIcon stateIcon(renderVolumeIcon(muted));
            _btnMute->setIcon(stateIcon);
            _btnMute->setProperty("cgplay.originalIcon", QVariant::fromValue(stateIcon));
            _btnMute->setProperty("cgplay.lastStyledIconKey", QVariant());
            updateCommandPresentationState(_btnMute, QStringLiteral("打开音量控制"),
                muted ? QStringLiteral("当前已静音；单击打开音量") : QStringLiteral("单击打开音量"));
        });
        connect(_playback->signalProxy(), &PlaybackServiceSignals::muteChanged, mutePopupButton, &QToolButton::setChecked);

        connect(_playback->signalProxy(), &PlaybackServiceSignals::currentFrameChanged, this, [this](int frame, int) {
            double fps = _playback->fps();
            if (fps <= 0.0) {
                fps = 24.0;
            }

            const int fpsInt = qMax(1, static_cast<int>(fps + 0.5));
            const int ff = frame % fpsInt;
            const int totalSeconds = frame / fpsInt;
            const int ss = totalSeconds % 60;
            const int mm = (totalSeconds / 60) % 60;
            const int hh = totalSeconds / 3600;
            const QString frameText = QStringLiteral("%1:%2:%3:%4")
                                          .arg(hh, 2, 10, QChar('0'))
                                          .arg(mm, 2, 10, QChar('0'))
                                          .arg(ss, 2, 10, QChar('0'))
                                          .arg(ff, 2, 10, QChar('0'));
            if (_lblFrame->text() != frameText) {
                _lblFrame->setText(frameText);
            }
            const QString fpsText = QString("%1 FPS").arg(fps, 0, 'f', 0);
            if (_lblFPS->text() != fpsText) {
                _lblFPS->setText(fpsText);
            }
        });

        connect(_btnLoop, &QToolButton::clicked, this, [this] {
            static int mode = 0;
            mode = (mode + 1) % 3;
            if (_playback) {
                _playback->setLoop(mode);
            }
            _btnLoop->setText(mode == 0 ? "LP" : (mode == 1 ? "1" : "AB"));
            updateCommandPresentationState(_btnLoop, QStringLiteral("循环模式"),
                mode == 0 ? QStringLiteral("当前：循环播放") :
                (mode == 1 ? QStringLiteral("当前：单次播放") : QStringLiteral("当前：往返播放")));
        });
    }
    updateCommandPresentationState(_btnSpeed, QStringLiteral("播放倍速"),
        QStringLiteral("当前：%1").arg(speedLabel(_speedMultiplier)));
    updateCommandPresentationState(_btnLoop, QStringLiteral("循环模式"), QStringLiteral("当前：循环播放"));
    updateCommandPresentationState(_btnMute, QStringLiteral("打开音量控制"),
        initiallyMuted ? QStringLiteral("当前已静音；单击打开音量") : QStringLiteral("单击打开音量"));
}

PlaybackBar::~PlaybackBar() = default;

void PlaybackBar::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    _updateResponsiveLayout();
}

void PlaybackBar::_updateResponsiveLayout()
{
    const int availableWidth = width();
    if (_lblFPS) {
        _lblFPS->setVisible(availableWidth >= 660);
    }
    if (_lblFrame) {
        _lblFrame->setMinimumWidth(availableWidth < 580 ? 104 : 132);
    }
}

void PlaybackBar::setTranslationVisible(bool visible)
{
    if (!_btnTranslation) {
        return;
    }
    const QSignalBlocker blocker(_btnTranslation);
    _btnTranslation->setChecked(visible);
    refreshCommandPresentation(_btnTranslation);
}

bool PlaybackBar::translationVisible() const
{
    return _btnTranslation && _btnTranslation->isChecked();
}

void PlaybackBar::setTranslationMode(const QString& modeId)
{
    const QString normalized = normalizeTranslationMode(modeId);
    if (_translationMode == normalized) {
        _updateTranslationModeButton();
        return;
    }
    _translationMode = normalized;
    _updateTranslationModeButton();
}

QString PlaybackBar::translationMode() const
{
    return _translationMode;
}

QToolButton* PlaybackBar::_makeBtn(const QString& text, const QString& tip, int size, bool accent)
{
    auto* button = new QToolButton(this);
    button->setText(text);
    button->setToolTip(tip);
    button->setFocusPolicy(Qt::NoFocus);
    button->setFixedSize(qMax(32, size), qMax(32, size));
    button->setCursor(Qt::PointingHandCursor);
    button->setAutoRaise(false);
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    button->setStyleSheet(QString(
        "QToolButton{color:%1;background:%2;border:1px solid %3;border-radius:7px;font-size:10px;font-weight:700;padding:0px;}"
        "QToolButton:hover{background:%4;color:#FFFFFF;border-color:%5;}"
        "QToolButton:pressed{background:rgba(255,140,50,0.30);color:#FFFFFF;border-color:%5;}"
        "QToolButton:checked{background:rgba(255,140,50,0.30);color:#FFFFFF;border-color:%5;}")
        .arg(accent ? "#FFF6EE" : "#F3F7FC")
        .arg(accent ? "qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 rgba(157,86,30,1.0),stop:1 rgba(91,48,18,1.0))" : "qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 rgba(43,52,63,0.98),stop:1 rgba(25,32,41,0.98))")
        .arg(accent ? "rgba(255,140,50,0.78)" : "rgba(255,255,255,0.18)")
        .arg(accent ? "qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 rgba(178,99,38,1.0),stop:1 rgba(112,61,24,1.0))" : "rgba(48,58,70,1.0)")
        .arg(kAccent));
    return button;
}

void PlaybackBar::_updateTranslationModeButton()
{
    if (_actQuickMode) {
        _actQuickMode->setChecked(_translationMode == QStringLiteral("quick-playback"));
    }
    if (_actHighQualityMode) {
        _actHighQualityMode->setChecked(_translationMode == QStringLiteral("high-quality"));
    }
    if (!_btnTranslationMode) {
        return;
    }
    _btnTranslationMode->setText(translationModeLabel(_translationMode));
    updateCommandPresentationState(_btnTranslationMode, QStringLiteral("翻译模式"),
        _translationMode == QStringLiteral("high-quality")
            ? QString::fromUtf8(u8"高质量翻译：手动策略边界，当前播放不等待、不暂停")
            : QString::fromUtf8(u8"快速播放：优先显示 quick 字幕，增强层逐句回退"));
}

void PlaybackBar::_onStateChanged(int state)
{
    const bool playing = state != 0;
    updateCommandPresentationState(_btnPlay, playing ? QStringLiteral("暂停") : QStringLiteral("播放"));
    QColor iconColor = themeColor("cgplay.accentColor", QColor("#FFF8F1"));
    const QColor configured(_btnPlay ? _btnPlay->property("cgplay.buttonIconColor").toString() : QString());
    if (configured.isValid()) iconColor = configured;
    _btnPlay->setIcon(playStateIcon(playing, iconColor));
    _btnPlay->setIconSize(QSize(playing ? 24 : 22, playing ? 24 : 22));
}

void PlaybackBar::_queueVolumeValue(int sliderValue)
{
    _pendingVolumeValue = qBound(0, sliderValue, 1000);
    if (!_volumeApplyTimer) {
        _applyPendingVolume();
        return;
    }
    if (!_volumeApplyTimer->isActive()) {
        _volumeApplyTimer->start(16);
    }
}

void PlaybackBar::_applyPendingVolume()
{
    if (!_playback) {
        return;
    }
    _playback->setVolume(_pendingVolumeValue / 1000.0f);
}

void PlaybackBar::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    const bool light = lightTheme();
    QColor toolbar = themeColor("cgplay.toolbarColor", QColor(24, 31, 39));
    QColor timeline = themeColor("cgplay.timelineColor", QColor(14, 19, 26));
    const int toolbarOpacity = qApp
        ? qBound(0, qApp->property("cgplay.toolbarOpacity").toInt(), 100)
        : 92;
    toolbar.setAlpha(qRound(toolbarOpacity * 255.0 / 100.0));
    timeline.setAlpha(qRound(toolbarOpacity * 255.0 / 100.0));
    const QColor accent = themeColor("cgplay.accentColor", QColor(0xFF, 0x8A, 0x3D));
    const QColor border = themeColor("cgplay.borderColor", QColor(255, 255, 255, 24));
    const QColor text = themeColor("cgplay.textColor", QColor(0xD8, 0xDE, 0xE7));
    QLinearGradient glass(0, 0, 0, height());
    glass.setColorAt(0.0, toolbar.lighter(light ? 105 : 108));
    glass.setColorAt(0.52, toolbar);
    glass.setColorAt(1.0, timeline);
    painter.fillRect(rect(), glass);
    if (!light) {
        QLinearGradient warm(0, 0, width(), 0);
        warm.setColorAt(0.0, QColor(accent.red(), accent.green(), accent.blue(), 10));
        warm.setColorAt(0.35, QColor(accent.red(), accent.green(), accent.blue(), 2));
        warm.setColorAt(1.0, QColor(accent.red(), accent.green(), accent.blue(), 7));
        painter.fillRect(rect(), warm);
    }
    painter.setPen(QPen(light ? toolbar.darker(120) : QColor(border.red(), border.green(), border.blue(), 120), 1));
    painter.drawLine(0, 0, width(), 0);
    painter.setPen(QPen(light ? timeline.darker(115) : QColor(text.red(), text.green(), text.blue(), 42), 1));
    painter.drawLine(0, height() - 1, width(), height() - 1);
}

} // namespace cgplay
