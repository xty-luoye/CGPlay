#pragma once
// CGPlay PlaybackBar.h — Phase 7: 52px playback bar below Timeline
#include <QWidget>
#include <QToolButton>
#include <QLabel>
#include <QSlider>
#include <QString>
#include <memory>

class QTimer;
class QAction;
class QResizeEvent;

namespace cgplay {

class IPlaybackService;

class PlaybackBar : public QWidget
{
    Q_OBJECT
public:
    explicit PlaybackBar(std::shared_ptr<IPlaybackService> playback,
                         QWidget* parent = nullptr);
    ~PlaybackBar() override;

    QToolButton* playBtn() const { return _btnPlay; }
    void setTranslationVisible(bool visible);
    bool translationVisible() const;
    void setTranslationMode(const QString& modeId);
    QString translationMode() const;

Q_SIGNALS:
    void translationVisibilityToggled(bool visible);
    void translationModeChanged(const QString& modeId);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    QToolButton* _makeBtn(const QString& text, const QString& tip, int size, bool accent=false);
    void _updateTranslationModeButton();
    void _onStateChanged(int state);
    void _queueVolumeValue(int sliderValue);
    void _applyPendingVolume();
    void _updateResponsiveLayout();

    std::shared_ptr<IPlaybackService> _playback;

    QToolButton* _btnPrev     = nullptr;
    QToolButton* _btnPlay     = nullptr;
    QToolButton* _btnNext     = nullptr;
    QToolButton* _btnTranslation = nullptr;
    QToolButton* _btnTranslationMode = nullptr;
    QToolButton* _btnSpeed  = nullptr;
    QAction*     _actQuickMode = nullptr;
    QAction*     _actHighQualityMode = nullptr;
    QToolButton* _btnLoop     = nullptr;
    QToolButton* _btnMute     = nullptr;
    QLabel*      _lblFPS      = nullptr;
    QLabel*      _lblFrame    = nullptr;
    QSlider*     _volSlider   = nullptr;
    QTimer*      _volumeApplyTimer = nullptr;
    int          _pendingVolumeValue = 1000;
    QString      _translationMode = QStringLiteral("quick-playback");
    double       _speedMultiplier = 1.0;
};

} // namespace cgplay
