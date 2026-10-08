// CGPlay TimelineWidget - Filmstrip Timeline with Marker + Thumbnail + Frame tracks
#include "TimelineWidget.h"
#include "annotation/ReviewExport.h"
#include "media/MediaProbe.h"
#include "media/ThumbnailService.h"
#include "playback/api/IPlaybackService.h"
#include "playback/api/PlaybackServiceSignals.h"
#include "common/jobs/JobSystem.h"

#include <QtConcurrent>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QElapsedTimer>
#include <QApplication>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QEvent>
#include <QHideEvent>
#include <QMouseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QRegularExpression>
#include <QTimer>
#include <cmath>
#include <algorithm>
#include <memory>

namespace cgplay {

static const QColor kBg(QColor(20, 26, 33, 206));
static const QColor kTrackBg(QColor(13, 18, 24, 180));
static const QColor kBorder(QColor(255, 255, 255, 22));
static const QColor kPlayhead(QColor(0xFF, 0x8A, 0x3D));
static const QColor kInPoint(QColor(0x2E, 0xCC, 0x71));
static const QColor kOutPoint(QColor(0xE7, 0x4C, 0x3C));
static const QColor kTextDim(QColor(0xC8, 0xD2, 0xDF));
static const QColor kTrackEdge(QColor(255, 255, 255, 30));
static constexpr int kMarkerH = 25;
static constexpr int kThumbH = 42;
static constexpr int kFrameH = 12;
static constexpr int kTotalH = kMarkerH + kThumbH + kFrameH + 7;
static constexpr int kThumbnailStartDelayMs = 500;

static QColor themeColor(const char* propertyName, const QColor& fallback)
{
    if (!qApp) return fallback;
    const QColor value(qApp->property(propertyName).toString());
    return value.isValid() ? value : fallback;
}

static QColor themeAlpha(QColor color, int alpha)
{
    color.setAlpha(qBound(0, alpha, 255));
    return color;
}

struct TimelineThumb {
    int frame = 0;
    QImage image;
};

using SharedTimelineThumb = ThumbnailFrame;

static QVector<TimelineThumb> buildTimelineThumbs(
    const QString& path,
    int totalFrames,
    double fps,
    int count,
    const OcioManager::PreviewTransformSettings& previewSettings,
    JobContext* job)
{
    QVector<TimelineThumb> out;
    const auto sharedThumbs = ThumbnailService::buildTimelineThumbnails(
        path,
        totalFrames,
        fps,
        count,
        previewSettings,
        job);
    out.reserve(sharedThumbs.size());
    for (const auto& thumb : sharedThumbs) {
        out.push_back({thumb.frame, thumb.image});
    }
    return out;
}

class TimelineBar : public QWidget {
public:
    int currentFrame = 0;
    int totalFrames = 1;
    int inPoint = -1;
    int outPoint = -1;
    double fps = 24.0;
    struct Marker { int frame; QColor color; QString label; };
    QVector<Marker> markers;
    QVector<TimelineThumb> thumbnails;
    QString thumbStatus;
    bool thumbnailsLoading = false;
    std::function<void(int)> onSeek;

    explicit TimelineBar(QWidget* p = nullptr)
        : QWidget(p)
    {
        setFixedHeight(kTotalH);
        setMouseTracking(true);
        setCursor(Qt::PointingHandCursor);
    }

    int frameToX(int f) const { return (totalFrames > 1) ? (int)((double)f / (totalFrames - 1) * width()) : 0; }
    int xToFrame(int x) const { return (totalFrames > 1) ? (int)((double)x / width() * (totalFrames - 1)) : 0; }
    void invalidateStaticLayer()
    {
        _staticLayerDirty = true;
    }
    void updateFrameVisual(int frame, int total, double fpsValue)
    {
        const int nextTotal = std::max(1, total);
        const double nextFps = fpsValue > 1.0 ? fpsValue : 24.0;
        if (totalFrames != nextTotal || std::abs(fps - nextFps) > 0.001) {
            totalFrames = nextTotal;
            fps = nextFps;
            currentFrame = frame;
            invalidateStaticLayer();
            update();
            return;
        }
        if (currentFrame != frame) {
            const QRect dirtyRect = _frameDirtyRect(currentFrame, frame);
            currentFrame = frame;
            update(dirtyRect);
        }
    }
    void setInOutPointsState(int inFrame, int outFrame)
    {
        if (inPoint == inFrame && outPoint == outFrame) {
            return;
        }
        inPoint = inFrame;
        outPoint = outFrame;
        invalidateStaticLayer();
        update();
    }
    void clearTransientLabels()
    {
        if (markers.isEmpty()) {
            return;
        }
        markers.clear();
        invalidateStaticLayer();
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        _ensureStaticLayer();

        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        if (!_staticLayerCache.isNull()) {
            p.drawPixmap(0, 0, _staticLayerCache);
        } else {
            _paintStaticLayer(p);
        }
        _paintDynamicLayer(p);
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton) {
            _lastSeekFrame = xToFrame(e->pos().x());
            _seekThrottle.restart();
            if (onSeek) onSeek(_lastSeekFrame);
            e->accept();
        }
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (e->buttons() & Qt::LeftButton) {
            const int frame = xToFrame(e->pos().x());
            if (frame != _lastSeekFrame && (!_seekThrottle.isValid() || _seekThrottle.elapsed() >= 12)) {
                _lastSeekFrame = frame;
                _seekThrottle.restart();
                if (onSeek) onSeek(frame);
            }
            e->accept();
        }
    }

    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton) {
            const int frame = xToFrame(e->pos().x());
            _lastSeekFrame = frame;
            if (onSeek) onSeek(frame);
            e->accept();
            return;
        }
        QWidget::mouseReleaseEvent(e);
    }

    void leaveEvent(QEvent* e) override
    {
        clearTransientLabels();
        QWidget::leaveEvent(e);
    }

    void hideEvent(QHideEvent* e) override
    {
        clearTransientLabels();
        QWidget::hideEvent(e);
    }

private:
    QRect _frameDirtyRect(int oldFrame, int newFrame) const
    {
        if (width() <= 0 || height() <= 0 || totalFrames <= 1) {
            return rect();
        }
        const int clampedOld = std::clamp(oldFrame, 0, totalFrames - 1);
        const int clampedNew = std::clamp(newFrame, 0, totalFrames - 1);
        const int left = std::max(0, std::min(frameToX(clampedOld), frameToX(clampedNew)) - 10);
        const int right = std::min(width() - 1, std::max(frameToX(clampedOld), frameToX(clampedNew)) + 10);
        return QRect(left, 0, std::max(1, right - left + 1), height());
    }

    void _ensureStaticLayer() const
    {
        const QString themeSignature = themeColor("cgplay.timelineColor", kTrackBg).name(QColor::HexArgb)
            + QLatin1Char('|') + themeColor("cgplay.panelColor", kBg).name(QColor::HexArgb)
            + QLatin1Char('|') + themeColor("cgplay.textColor", kTextDim).name(QColor::HexArgb)
            + QLatin1Char('|') + themeColor("cgplay.borderColor", kBorder).name(QColor::HexArgb)
            + QLatin1Char('|') + themeColor("cgplay.accentColor", kPlayhead).name(QColor::HexArgb)
            + QLatin1Char('|') + QString::number(
                qApp ? qBound(0, qApp->property("cgplay.timelineOpacity").toInt(), 100) : 96);
        if (_themeSignature != themeSignature) {
            _themeSignature = themeSignature;
            _staticLayerDirty = true;
        }
        if (!_staticLayerDirty && _staticLayerCache.size() == size()) {
            return;
        }
        if (width() <= 0 || height() <= 0) {
            _staticLayerCache = QPixmap();
            _staticLayerDirty = false;
            return;
        }

        QPixmap buffer(size());
        buffer.fill(Qt::transparent);
        QPainter painter(&buffer);
        painter.setRenderHint(QPainter::Antialiasing);
        _paintStaticLayer(painter);
        _staticLayerCache = buffer;
        _staticLayerDirty = false;
    }

    void _paintStaticLayer(QPainter& p) const
    {
        const int w = width();
        QColor panel = themeColor("cgplay.panelColor", kBg);
        QColor timeline = themeColor("cgplay.timelineColor", kTrackBg);
        const int timelineOpacity = qApp ? qBound(0, qApp->property("cgplay.timelineOpacity").toInt(), 100) : 96;
        panel.setAlpha(qRound(timelineOpacity * 255.0 / 100.0));
        timeline.setAlpha(qRound(timelineOpacity * 255.0 / 100.0));
        const QColor text = themeColor("cgplay.textColor", kTextDim);
        const QColor border = themeColor("cgplay.borderColor", kBorder);
        const QColor accent = themeColor("cgplay.accentColor", kPlayhead);
        auto fx = [&](int f) { return (totalFrames > 1) ? (int)((double)f / (totalFrames - 1) * w) : 0; };
        QLinearGradient bg(0, 0, 0, height());
        bg.setColorAt(0.0, panel.lighter(106));
        bg.setColorAt(0.42, timeline);
        bg.setColorAt(1.0, timeline.darker(108));
        p.fillRect(rect(), bg);

        const int y1 = 0;
        QLinearGradient markerBg(0, y1, 0, y1 + kMarkerH);
        markerBg.setColorAt(0.0, panel);
        markerBg.setColorAt(1.0, timeline);
        p.fillRect(QRect(0, y1, w, kMarkerH), markerBg);
        p.fillRect(QRect(0, y1, w, 1), themeAlpha(text, 18));
        p.setPen(QPen(themeAlpha(border, 150), 1));
        p.drawLine(0, y1 + kMarkerH - 1, w, y1 + kMarkerH - 1);

        const double fpsValue = fps > 1.0 ? fps : 24.0;
        const int majorTickCount = std::max(5, std::min(9, w / 120));
        const int majorStepFrames = std::max(1, totalFrames / majorTickCount);
        p.setPen(QPen(themeAlpha(border, 130), 1));
        QFont tickFont("Segoe UI");
        tickFont.setPixelSize(10);
        tickFont.setWeight(QFont::DemiBold);
        p.setFont(tickFont);
        for (int f = 0; f < totalFrames; f += majorStepFrames) {
            const int x = fx(f);
            p.drawLine(x, y1 + kMarkerH - 12, x, y1 + kMarkerH - 1);
            const int minorStep = std::max(1, majorStepFrames / 8);
            p.setPen(QPen(themeAlpha(border, 100), 1));
            for (int mf = f + minorStep; mf < std::min(totalFrames, f + majorStepFrames); mf += minorStep) {
                const int mx = fx(mf);
                p.drawLine(mx, y1 + kMarkerH - 6, mx, y1 + kMarkerH - 1);
            }
            const int sec = static_cast<int>(f / fpsValue);
            const int ss = sec % 60;
            const int mm = (sec / 60) % 60;
            const int hh = sec / 3600;
            const int ff = static_cast<int>(f - std::floor(sec * fpsValue));
            p.setPen(text);
            p.drawText(
                QRect(x - 38, y1 + 2, 76, 13),
                Qt::AlignHCenter | Qt::AlignTop,
                QStringLiteral("%1:%2:%3:%4")
                    .arg(hh, 2, 10, QChar('0'))
                    .arg(mm, 2, 10, QChar('0'))
                    .arg(ss, 2, 10, QChar('0'))
                    .arg(ff, 2, 10, QChar('0')));
            p.setPen(QPen(themeAlpha(border, 120), 1));
        }

        for (const auto& m : markers) {
            const int mx = fx(m.frame);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(m.color.red(), m.color.green(), m.color.blue(), 52));
            p.drawEllipse(QPoint(mx, y1 + 7), 7, 7);
            p.setBrush(m.color);
            p.drawEllipse(QPoint(mx, y1 + 6), 4, 4);
            p.drawRoundedRect(QRect(mx - 1, y1 + 8, 2, kMarkerH - 14), 1, 1);
        }
        if (inPoint >= 0) {
            const int ax = fx(inPoint);
            p.fillRect(QRect(ax - 1, y1, 2, kMarkerH), kInPoint);
        }
        if (outPoint >= 0) {
            const int bx = fx(outPoint);
            p.fillRect(QRect(bx - 1, y1, 2, kMarkerH), kOutPoint);
        }

        const int y2 = kMarkerH + 1;
        QLinearGradient thumbBg(0, y2, 0, y2 + kThumbH);
        thumbBg.setColorAt(0.0, timeline.lighter(106));
        thumbBg.setColorAt(0.48, timeline);
        thumbBg.setColorAt(1.0, timeline.darker(112));
        p.fillRect(QRect(0, y2, w, kThumbH), thumbBg);
        p.fillRect(QRect(0, y2 + 1, w, 1), themeAlpha(border, 90));
        p.setPen(QPen(themeAlpha(border, 135), 1));
        p.drawLine(0, y2 + kThumbH - 1, w, y2 + kThumbH - 1);
        if (!thumbnails.isEmpty()) {
            const int n = thumbnails.size();
            const int gap = 2;
            const int slotW = std::max(38, (w - (n + 1) * gap) / std::max(1, n));
            for (int i = 0; i < n; ++i) {
                const auto& th = thumbnails[i];
                const int x = gap + i * (slotW + gap);
                const QRect tr(x, y2 + 4, std::min(slotW, 70), kThumbH - 7);
                QPainterPath clip;
                clip.addRoundedRect(tr, 3, 3);
                p.save();
                p.setClipPath(clip);
                p.drawImage(tr, th.image);
                QLinearGradient thumbShade(0, tr.top(), 0, tr.bottom());
                thumbShade.setColorAt(0.0, themeAlpha(text, 8));
                thumbShade.setColorAt(0.60, themeAlpha(QColor(Qt::black), 0));
                thumbShade.setColorAt(1.0, themeAlpha(QColor(Qt::black), 26));
                p.fillRect(tr, thumbShade);
                p.restore();
                p.setPen(QPen(themeAlpha(border, 170), 1));
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(tr, 3, 3);
            }
        } else {
            const int gap = 3;
            const int nt = std::max(8, std::min(18, w / 48));
            const int slotW = std::max(34, (w - (nt + 1) * gap) / std::max(1, nt));
            for (int i = 0; i < nt; ++i) {
                const int x = gap + i * (slotW + gap);
                const QRect tr(x, y2 + 5, slotW, kThumbH - 10);
                QPainterPath clip;
                clip.addRoundedRect(tr, 3, 3);
                QLinearGradient slot(0, tr.top(), 0, tr.bottom());
                const int shimmer = (i % 5) * 5;
                slot.setColorAt(0.0, themeAlpha(timeline.lighter(106 + shimmer), 150));
                slot.setColorAt(0.55, themeAlpha(timeline, 158));
                slot.setColorAt(1.0, themeAlpha(timeline.darker(112), 186));
                p.fillPath(clip, slot);
                p.setPen(QPen(themeAlpha(border, 120), 1));
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(tr, 3, 3);
                p.setPen(Qt::NoPen);
                p.setBrush(themeAlpha(accent, i % 3 == 0 ? 22 : 10));
                for (int h = 0; h < 2; ++h) {
                    const int hy = tr.y() + 6 + h * std::max(8, tr.height() - 12);
                    p.drawRoundedRect(QRect(tr.x() + 3, hy, 3, 4), 1, 1);
                    p.drawRoundedRect(QRect(tr.right() - 6, hy, 3, 4), 1, 1);
                }
            }
            if (!thumbStatus.isEmpty()) {
                p.setPen(themeAlpha(text, 150));
                QFont statusFont("Microsoft YaHei");
                statusFont.setPixelSize(10);
                p.setFont(statusFont);
                p.drawText(QRect(0, y2 + kThumbH - 17, w, 13), Qt::AlignCenter, thumbStatus);
            }
        }

        const int y3 = y2 + kThumbH + 1;
        QLinearGradient frameBg(0, y3, 0, y3 + kFrameH);
        frameBg.setColorAt(0.0, timeline);
        frameBg.setColorAt(1.0, timeline.darker(118));
        p.fillRect(QRect(0, y3, w, kFrameH), frameBg);
        p.setPen(QPen(themeAlpha(border, 125), 1));
        p.drawLine(0, y3, w, y3);
        const int te = std::max(1, totalFrames / std::max(1, w / 64));
        for (int f = 0; f < totalFrames; f += te) {
            const int x = fx(f);
            const int wave = 2 + ((f * 37) % std::max(2, kFrameH - 3));
            p.setPen(QPen(themeAlpha(border, 110), 1));
            p.drawLine(x, y3 + kFrameH - wave, x, y3 + kFrameH - 2);
            if (x % 5 == 0) {
                p.setPen(QPen(themeAlpha(accent, 80), 1));
                p.drawLine(x, y3 + kFrameH - wave, x, y3 + kFrameH - 2);
            }
        }
    }

    void _paintDynamicLayer(QPainter& p) const
    {
        if (totalFrames <= 1 || currentFrame < 0) {
            return;
        }

        const int px = frameToX(currentFrame);
        const int y1 = 0;
        const int y2 = kMarkerH + 1;
        if (currentFrame > 0) {
            const QColor accent = themeColor("cgplay.accentColor", kPlayhead);
            p.fillRect(QRect(0, y2, px, kThumbH), themeAlpha(accent, 18));
        }
        const QColor accent = themeColor("cgplay.accentColor", kPlayhead);
        p.setPen(QPen(themeAlpha(accent, 60), 4));
        p.drawLine(px, 0, px, height());
        p.setPen(QPen(accent, 2));
        p.drawLine(px, 0, px, height());
        p.setPen(Qt::NoPen);
        p.setBrush(themeAlpha(accent, 60));
        p.drawEllipse(QPoint(px, y1 + 6), 7, 7);
        p.setBrush(accent);
        p.drawEllipse(QPoint(px, y1 + 6), 4, 4);
        p.setBrush(themeAlpha(accent, 60));
        p.drawRoundedRect(QRect(px - 2, y2 + 2, 4, kThumbH - 3), 2, 2);
    }

    mutable QPixmap _staticLayerCache;
    mutable bool _staticLayerDirty = true;
    mutable QString _themeSignature;
    QElapsedTimer _seekThrottle;
    int _lastSeekFrame = -1;
};

struct TimelineWidget::Private {
    std::shared_ptr<IPlaybackService> playback;
    TimelineBar* bar = nullptr;
    QLabel* lblFrame = nullptr;
    int curFrame = 0;
    int total = 1;
    QString mediaPath;
    int firstDisplayFrame = -1;
    int lastDisplayFrame = -1;
    int thumbSerial = 0;
    int thumbTotal = 0;
    QFutureWatcher<QVector<TimelineThumb>>* thumbWatcher = nullptr;
    std::shared_ptr<JobContext> thumbJob;
    OcioManager::PreviewTransformSettings previewSettings;
    OcioManager::PreviewTransformSettings thumbRequestSettings;
    QString thumbMediaPath;
    double thumbFps = 0.0;
};

TimelineWidget::TimelineWidget(std::shared_ptr<IPlaybackService> pb, QWidget* parent)
    : QWidget(parent)
    , _p(std::make_unique<Private>())
{
    _p->playback = std::move(pb);
    setFixedHeight(kTotalH + 15);
    setAcceptDrops(true);
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(
        "TimelineWidget{background:qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 rgba(24,31,39,0.80),stop:0.55 rgba(15,20,27,0.76),stop:1 rgba(9,13,18,0.88));"
        "border-top:1px solid rgba(255,255,255,0.075);}");
    _buildUI();
    if (_p->playback) {
        if (auto* playbackSignals = _p->playback->signalProxy()) {
            connect(playbackSignals, &PlaybackServiceSignals::currentFrameChanged, this, &TimelineWidget::_onFrameChanged);
            connect(playbackSignals, &PlaybackServiceSignals::inOutPointsChanged, this,
                    [this](int i, int o) { setInOutPoints(i, o); });
        }
    }
}

TimelineWidget::~TimelineWidget()
{
    if (_p->thumbJob) {
        _p->thumbJob->cancel();
    }
}

void TimelineWidget::_buildUI()
{
    auto* vlay = new QVBoxLayout(this);
    vlay->setContentsMargins(0, 0, 0, 0);
    vlay->setSpacing(0);
    _p->bar = new TimelineBar(this);
    _p->bar->onSeek = [this](int f) { if (_p->playback) _p->playback->seekToFrame(f); };
    _p->bar->totalFrames = 1;
    _p->bar->currentFrame = 0;
    _p->bar->thumbStatus = QStringLiteral("打开媒体后显示真实缩略图");
    _p->bar->totalFrames = 1;
    _p->bar->currentFrame = 0;
    _p->bar->thumbStatus = QString::fromUtf8("打开媒体后显示真实缩略图");
    vlay->addWidget(_p->bar);

    auto* lr = new QHBoxLayout();
    lr->setContentsMargins(10, 2, 10, 2);
    lr->setSpacing(10);
    lr->addStretch();
    _p->lblFrame = new QLabel("0 / 0", this);
    _p->lblFrame->setAlignment(Qt::AlignCenter);
    _p->lblFrame->setMinimumWidth(84);
    _p->lblFrame->setStyleSheet(
        QString("QLabel{color:%1;font-size:12px;font-weight:700;padding:2px 10px;"
                "background:rgba(16,21,27,0.90);border:1px solid rgba(255,140,50,0.40);"
                "border-radius:6px;}").arg(kPlayhead.name()));
    lr->addWidget(_p->lblFrame);
    vlay->addLayout(lr);
    _p->lblFrame->setText("0 / 0");
}

void TimelineWidget::_onFrameChanged(int frame, int total)
{
    if (_p->mediaPath.isEmpty()) {
        _p->curFrame = 0;
        _p->total = 1;
        if (_p->bar) {
            _p->bar->updateFrameVisual(0, 1, 24.0);
        }
        if (_p->lblFrame && _p->lblFrame->text() != QStringLiteral("0 / 0")) {
            _p->lblFrame->setText(QStringLiteral("0 / 0"));
        }
        return;
    }

    _p->curFrame = frame;
    _p->total = std::max(1, total);
    if (_p->bar) {
        _p->bar->updateFrameVisual(frame, _p->total, _p->playback ? _p->playback->fps() : 24.0);
    }
    if (!_p->mediaPath.isEmpty() && _p->thumbTotal != _p->total) {
        const int serial = _p->thumbSerial;
        QTimer::singleShot(kThumbnailStartDelayMs, this, [this, serial] {
            if (serial == _p->thumbSerial) _startThumbnailBuild();
        });
    }
    const int displayTotal = std::max(1, total);
    const int displayFrame = std::clamp(frame + 1, 1, displayTotal);
    if (_p->lblFrame) {
        const QString nextText = QStringLiteral("%1 / %2").arg(displayFrame).arg(displayTotal);
        if (_p->lblFrame->text() != nextText) {
            _p->lblFrame->setText(nextText);
        }
    }
}

void TimelineWidget::setMediaPath(const QString& path)
{
    ++_p->thumbSerial;
    if (_p->thumbWatcher) {
        if (_p->thumbJob) {
            _p->thumbJob->cancel();
            _p->thumbJob.reset();
        }
        _p->thumbWatcher->disconnect(this);
        _p->thumbWatcher->deleteLater();
        _p->thumbWatcher = nullptr;
    }

    _p->mediaPath = path;
    _p->thumbMediaPath = path;
    _p->firstDisplayFrame = -1;
    _p->lastDisplayFrame = -1;
    const auto sequence = MediaProbe::isStillImagePath(path)
        ? MediaProbe::collectSequenceFiles(path)
        : QVector<SequenceFrame>{};
    if (!sequence.isEmpty()) {
        _p->firstDisplayFrame = sequence.front().frameNumber;
        _p->lastDisplayFrame = sequence.back().frameNumber;
    }
    _p->thumbTotal = 0;
    _p->thumbFps = 0.0;
    _p->thumbRequestSettings = {};
    if (_p->bar) {
        _p->bar->clearTransientLabels();
        _p->bar->thumbnails.clear();
        _p->bar->currentFrame = 0;
        _p->bar->totalFrames = 1;
        _p->bar->thumbnailsLoading = !path.isEmpty();
        _p->bar->thumbStatus = path.isEmpty() ? QStringLiteral("打开媒体后显示真实缩略图") : QStringLiteral("正在生成时间轴缩略图...");
        _p->bar->invalidateStaticLayer();
        _p->bar->update();
    }
    if (path.isEmpty() && _p->lblFrame) {
        _p->lblFrame->setText("0 / 0");
    }
    const int serial = _p->thumbSerial;
    QTimer::singleShot(kThumbnailStartDelayMs, this, [this, serial] {
        if (serial == _p->thumbSerial) _startThumbnailBuild();
    });
}

void TimelineWidget::setPreviewTransformSettings(const OcioManager::PreviewTransformSettings& settings)
{
    if (_p->previewSettings.enabled == settings.enabled &&
        _p->previewSettings.configPath == settings.configPath &&
        _p->previewSettings.input == settings.input &&
        _p->previewSettings.display == settings.display &&
        _p->previewSettings.view == settings.view) {
        return;
    }
    _p->previewSettings = settings;
    if (!_p->mediaPath.isEmpty()) {
        const int serial = _p->thumbSerial;
        QTimer::singleShot(kThumbnailStartDelayMs, this, [this, serial] {
            if (serial == _p->thumbSerial) _startThumbnailBuild();
        });
    }
}

bool TimelineWidget::_thumbnailRequestMatchesCurrent(
    const QString& path,
    int total,
    double fps,
    const OcioManager::PreviewTransformSettings& settings) const
{
    return _p->thumbMediaPath == path &&
           _p->thumbTotal == std::max(1, total) &&
           std::abs(_p->thumbFps - (fps > 0.0 ? fps : 24.0)) < 0.001 &&
           _p->thumbRequestSettings.enabled == settings.enabled &&
           _p->thumbRequestSettings.configPath == settings.configPath &&
           _p->thumbRequestSettings.input == settings.input &&
           _p->thumbRequestSettings.display == settings.display &&
           _p->thumbRequestSettings.view == settings.view;
}

void TimelineWidget::_startThumbnailBuild()
{
    if (_p->mediaPath.isEmpty()) return;
    const int total = std::max(1, _p->total);
    const double fps = _p->playback ? _p->playback->fps() : 24.0;
    if (_thumbnailRequestMatchesCurrent(_p->mediaPath, total, fps, _p->previewSettings)) {
        return;
    }
    const int serial = ++_p->thumbSerial;
    _p->thumbTotal = total;
    _p->thumbMediaPath = _p->mediaPath;
    _p->thumbFps = fps > 0.0 ? fps : 24.0;
    _p->thumbRequestSettings = _p->previewSettings;
    if (_p->thumbWatcher) {
        if (_p->thumbJob) {
            _p->thumbJob->cancel();
            _p->thumbJob.reset();
        }
        _p->thumbWatcher->disconnect(this);
        _p->thumbWatcher->deleteLater();
        _p->thumbWatcher = nullptr;
    }
    if (_p->bar) {
        _p->bar->clearTransientLabels();
        _p->bar->thumbnails.clear();
        _p->bar->thumbnailsLoading = true;
        _p->bar->invalidateStaticLayer();
        _p->bar->thumbStatus = QStringLiteral("正在生成时间轴缩略图...");
        _p->bar->update();
    }
    auto* watcher = new QFutureWatcher<QVector<TimelineThumb>>(this);
    auto job = std::make_shared<JobContext>(3 * 60 * 1000);
    _p->thumbWatcher = watcher;
    _p->thumbJob = job;
    connect(watcher, &QFutureWatcher<QVector<TimelineThumb>>::finished, this, [this, watcher, job, serial] {
        const auto thumbs = watcher->result();
        if (serial == _p->thumbSerial && _p->bar) {
            _p->bar->thumbnails = thumbs;
            _p->bar->thumbnailsLoading = false;
            _p->bar->thumbStatus = thumbs.isEmpty()
                ? (ReviewExport::isStillImage(_p->mediaPath)
                    ? QStringLiteral("此图片/序列暂不可生成时间轴缩略图")
                    : QStringLiteral("无法生成时间轴缩略图：未找到 ffmpeg 或媒体不可提取"))
                : QString();
            _p->bar->invalidateStaticLayer();
            _p->bar->update();
        }
        watcher->deleteLater();
        if (_p->thumbWatcher == watcher) _p->thumbWatcher = nullptr;
        if (_p->thumbJob == job) _p->thumbJob.reset();
    });
    const QString mediaPath = _p->mediaPath;
    const auto previewSettings = _p->previewSettings;
    watcher->setFuture(QtConcurrent::run([mediaPath, total, fps, previewSettings, job] {
        return buildTimelineThumbs(mediaPath, total, fps, 22, previewSettings, job.get());
    }));
}

void TimelineWidget::setInOutPoints(int i, int o)
{
    if (_p->bar) {
        _p->bar->setInOutPointsState(i, o);
    }
}

void TimelineWidget::resizeEvent(QResizeEvent* e) { QWidget::resizeEvent(e); }
void TimelineWidget::dragEnterEvent(QDragEnterEvent* e) { if (e->mimeData()->hasUrls()) e->acceptProposedAction(); }
void TimelineWidget::dropEvent(QDropEvent* e)
{
    const auto u = e->mimeData()->urls();
    if (!u.isEmpty()) Q_EMIT droppedFile(u.first().toLocalFile());
}

} // namespace cgplay
