#pragma once
// CGPlay CompareToolbar.h
// Toolbar for A/B comparison: A/B, Wipe, Overlay, Difference, Horizontal, Vertical, Tile

#include <QWidget>
#include <memory>

namespace tl {
    struct CompareOptions;
}

namespace cgplay {

class CompareToolbar : public QWidget
{
    Q_OBJECT
public:
    explicit CompareToolbar(QWidget* parent = nullptr);
    ~CompareToolbar() override;

    // Get current compare options
    tl::CompareOptions compareOptions() const;

    // Current modes
    bool isCompareActive() const;   // true if not just "A"
    int  compareMode() const;       // 0=A, 1=B, 2=Wipe, 3=Overlay, 4=Difference, 5=H, 6=V, 7=Tile

    // Shot labels
    void setShotALabel(const QString& label);
    void setShotBLabel(const QString& label);
    QString shotALabel() const;
    QString shotBLabel() const;

    // Auto-clear B when new A is loaded
    bool isAutoClearB() const;
    void setAutoClearB(bool on);
    void toggleAutoClearB();

    // ── v1.4 Session 序列化 ────────────────────────────────────────────
    float wipeCenterX() const;
    float wipeCenterY() const;
    float wipeRotation() const;
    float overlayAmount() const;

public Q_SLOTS:
    void setCompareMode(int mode);
    void setWipeCenter(float x, float y);
    void setWipeRotation(float degrees);
    void setOverlay(float value);
    void toggleCompare();

Q_SIGNALS:
    void compareOptionsChanged(const tl::CompareOptions& opts);
    void compareModeChanged(int mode);
    void wipeCenterChanged(float x, float y);
    void wipeRotationChanged(float degrees);
    void overlayChanged(float value);
    void shotAFileDropped(const QString& path);  // file dropped on A label
    void shotBFileDropped(const QString& path);  // file dropped on B label
    void autoClearBToggled(bool enabled);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void _buildUI();
    void _emitOptions();
    void _scheduleSliderChanges(int changes, bool immediate = false);
    void _flushSliderChanges();

    struct Private;
    std::unique_ptr<Private> _p;
};

} // namespace cgplay
