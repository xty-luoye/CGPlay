#pragma once
// CGPlay PlaylistModel.h
// Qt item model for the shot/clip list

#include <QAbstractListModel>
#include <QString>
#include <QIcon>
#include <QPointer>
#include "ocio/OcioManager.h"
#include <vector>
#include <memory>

namespace cgplay {

struct MediaInfo;
class JobHandle;

// ─── ShotItem ─────────────────────────────────────────────────────────────────
struct ShotItem
{
    QString path;         // full path to file / sequence root
    QString name;         // display name
    int     firstFrame = 0;
    int     lastFrame  = 0;
    QString format;       // "EXR","DPX","MOV",…
    QIcon   thumbnail;    // optional thumbnail
    QColor  statusColor = QColor(128, 128, 128); // grey = not reviewed
    int     width      = 0;
    int     height     = 0;

    // OTIO metadata
    double  fps        = 24.0;
    QString trackKind  = "Video";   // "Video" / "Audio"
    QString trackName;              // Original track name from OTIO

    bool isValid() const { return !path.isEmpty(); }
    int  frameCount() const { return lastFrame - firstFrame + 1; }
};

// ─── PlaylistModel ────────────────────────────────────────────────────────────
class PlaylistModel : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Roles {
        PathRole         = Qt::UserRole + 1,
        NameRole,
        FirstFrameRole,
        LastFrameRole,
        FormatRole,
        StatusColorRole,
        WidthRole,
        HeightRole,
        FrameCountRole,
    };

    // Status color management
    void setStatusColor(int index, const QColor& color);
    QColor statusColorAt(int index) const;

    explicit PlaylistModel(QObject* parent = nullptr);
    ~PlaylistModel() override;

    // QAbstractListModel overrides
    int      rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    bool     dropMimeData(const QMimeData* data, Qt::DropAction action,
                          int row, int col, const QModelIndex& parent) override;
    QStringList mimeTypes() const override;
    Qt::DropActions supportedDropActions() const override;

    // Playlist ops
    void addPath(const QString& path);
    void addPath(const QString& path, double fpsOverride);
    void addPath(const QString& path, const MediaInfo& mediaInfo, double fpsOverride = 0.0);
    void addPaths(const QStringList& paths);
    void addShot(ShotItem shot);       // Direct shot insertion (for OTIO import)
    void removeShotAt(int index);
    void clear();
    void moveUp(int index);
    void moveDown(int index);
    void sort(bool ascending = true);
    void rebuildThumbnails(const OcioManager::PreviewTransformSettings& previewSettings = {});

    // Public format detection (used by OTIO importer)
    static QString _detectFormatPublic(const QString& path);

    const ShotItem& shotAt(int index) const;
    int             currentIndex() const;
    void            setCurrentIndex(int index);

    // ── v1.4 Session 序列化 ────────────────────────────────────────────
    QJsonObject toJson() const;
    void        fromJson(const QJsonObject& obj);
    int         shotCount() const { return static_cast<int>(_shots.size()); }

Q_SIGNALS:
    void currentChanged(int index, const ShotItem& shot);

private:
    void _queueThumbnail(const QString& path);
    void _scheduleThumbnail(const QString& path);
    void _cancelThumbnailJobs();
    static ShotItem _buildShotItem(const QString& path, const OcioManager::PreviewTransformSettings& previewSettings = {}, double fpsOverride = 0.0, const MediaInfo* mediaInfo = nullptr);
    static QString  _detectFormat(const QString& path);
    static QString  _displayName(const QString& path);

    std::vector<ShotItem> _shots;
    int                   _currentIndex = -1;
    OcioManager::PreviewTransformSettings _previewSettings;
    QVector<QPointer<JobHandle>> _thumbnailJobs;
    quint64 _thumbnailGeneration = 0;
};

} // namespace cgplay
