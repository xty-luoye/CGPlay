#pragma once
// CGPlay PlaylistPanel.h
// Left-side dock panel: list view + toolbar + search + A/B shot marking

#include <QWidget>
#include <QString>
#include <memory>

namespace cgplay {

class IPlaybackService;
class PlaylistModel;

class PlaylistPanel : public QWidget
{
    Q_OBJECT
public:
    explicit PlaylistPanel(
        std::shared_ptr<IPlaybackService> playback,
        QWidget* parent = nullptr);
    ~PlaylistPanel() override;

    PlaylistModel* model() const;
    int            currentShotIndex() const;

Q_SIGNALS:
    void shotActivated(const QString& path);
    void shotASelected(const QString& path);
    void shotBSelected(const QString& path);
    void statusChanged(int index, const QColor& color);
    void sequenceImportRequested(const QString& path);
    void shotsRemoved(const QStringList& paths);
    void playlistCleared(const QStringList& paths);

protected:
    void paintEvent(QPaintEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private Q_SLOTS:
    void _onAddFiles();
    void _onRemove();
    void _onClear();
    void _onSearch(const QString& text);
    void _onItemActivated(const QModelIndex& index);
    void _onSortAsc();
    void _onSortDesc();
    void _onSetShotA();
    void _onSetShotB();
    void _onCycleStatus();
    void _onImportOtio();
    void _onExportOtio();

private:
    void _setupUI();
    void _navigate(int direction);

    struct Private;
    std::unique_ptr<Private> _p;
};

} // namespace cgplay
