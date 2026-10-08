// CGPlay PlaylistModel.cpp

#include "PlaylistModel.h"
#include "common/jobs/JobSystem.h"
#include "media/MediaProbe.h"
#include "media/ThumbnailService.h"

#include <QFileInfo>
#include <QMimeData>
#include <QPixmap>
#include <QTimer>
#include <QUrl>
#include <QStringList>
#include <QRegularExpression>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <algorithm>

namespace cgplay {

namespace {

constexpr int kThumbnailStartDelayMs = 500;

bool previewSettingsEqual(
    const OcioManager::PreviewTransformSettings& a,
    const OcioManager::PreviewTransformSettings& b)
{
    return a.enabled == b.enabled &&
           a.configPath == b.configPath &&
           a.input == b.input &&
           a.display == b.display &&
           a.view == b.view;
}

} // namespace

// ─── Constructor / Destructor ────────────────────────────────────────────────
PlaylistModel::PlaylistModel(QObject* parent)
    : QAbstractListModel(parent)
{}

PlaylistModel::~PlaylistModel()
{
    _cancelThumbnailJobs();
}

// ─── QAbstractListModel ───────────────────────────────────────────────────────
int PlaylistModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(_shots.size());
}

QVariant PlaylistModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= static_cast<int>(_shots.size()))
        return {};

    const auto& shot = _shots[index.row()];

    switch (role) {
    case Qt::DisplayRole:
        return QString("[%1] %2").arg(shot.format).arg(shot.name);
    case NameRole:
        return shot.name;
    case Qt::DecorationRole:
        if (!shot.thumbnail.isNull()) return shot.thumbnail;
        break;
    case PathRole:
        return shot.path;
    case FirstFrameRole:
        return shot.firstFrame;
    case LastFrameRole:
        return shot.lastFrame;
    case FormatRole:
        return shot.format;
    case StatusColorRole:
        return shot.statusColor;
    case WidthRole:
        return shot.width;
    case HeightRole:
        return shot.height;
    case FrameCountRole:
        return shot.frameCount();
    case Qt::UserRole + 10:
        return shot.fps;
    case Qt::ToolTipRole:
        return QString("%1\n%2  frames %3–%4")
            .arg(shot.path)
            .arg(shot.format)
            .arg(shot.firstFrame)
            .arg(shot.lastFrame);
    case Qt::BackgroundRole:
        if (index.row() == _currentIndex)
            return QColor(42, 100, 180, 120);
        break;
    default:
        break;
    }
    return {};
}

Qt::ItemFlags PlaylistModel::flags(const QModelIndex& index) const
{
    Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDropEnabled;
    if (index.isValid()) f |= Qt::ItemIsDragEnabled;
    return f;
}

QStringList PlaylistModel::mimeTypes() const
{
    return {"text/uri-list"};
}

Qt::DropActions PlaylistModel::supportedDropActions() const
{
    return Qt::CopyAction | Qt::MoveAction;
}

bool PlaylistModel::dropMimeData(const QMimeData* data, Qt::DropAction /*action*/,
                                  int /*row*/, int /*col*/, const QModelIndex& /*parent*/)
{
    if (!data->hasUrls()) return false;
    QStringList paths;
    for (const auto& url : data->urls()) {
        if (url.isLocalFile()) paths << url.toLocalFile();
    }
    if (!paths.isEmpty()) addPaths(paths);
    return true;
}

// ─── Playlist operations ──────────────────────────────────────────────────────
void PlaylistModel::addPath(const QString& path)
{
    addPath(path, 0.0);
}

void PlaylistModel::addPath(const QString& path, double fpsOverride)
{
    ShotItem item = _buildShotItem(path, _previewSettings, fpsOverride);
    if (!item.isValid()) return;

    beginInsertRows({}, static_cast<int>(_shots.size()), static_cast<int>(_shots.size()));
    _shots.push_back(std::move(item));
    endInsertRows();
    _queueThumbnail(path);
}

void PlaylistModel::addPath(const QString& path, const MediaInfo& mediaInfo, double fpsOverride)
{
    ShotItem item = _buildShotItem(path, _previewSettings, fpsOverride, &mediaInfo);
    if (!item.isValid()) return;

    beginInsertRows({}, static_cast<int>(_shots.size()), static_cast<int>(_shots.size()));
    _shots.push_back(std::move(item));
    endInsertRows();
    _queueThumbnail(path);
}

void PlaylistModel::addPaths(const QStringList& paths)
{
    for (const auto& p : paths) addPath(p);
}

void PlaylistModel::rebuildThumbnails(const OcioManager::PreviewTransformSettings& previewSettings)
{
    const bool hasMissingThumbnails = std::any_of(
        _shots.begin(),
        _shots.end(),
        [](const ShotItem& shot) { return shot.thumbnail.isNull(); });
    if (previewSettingsEqual(_previewSettings, previewSettings) && !hasMissingThumbnails) {
        return;
    }

    _cancelThumbnailJobs();
    _previewSettings = previewSettings;
    if (_shots.empty()) {
        return;
    }

    for (auto& shot : _shots) {
        shot.thumbnail = {};
    }
    Q_EMIT dataChanged(index(0), index(static_cast<int>(_shots.size()) - 1), {Qt::DecorationRole});
    for (const auto& shot : _shots) {
        _scheduleThumbnail(shot.path);
    }
}

void PlaylistModel::addShot(ShotItem shot)
{
    if (!shot.isValid()) return;
    beginInsertRows({}, static_cast<int>(_shots.size()), static_cast<int>(_shots.size()));
    _shots.push_back(std::move(shot));
    endInsertRows();
}

void PlaylistModel::removeShotAt(int index)
{
    if (index < 0 || index >= static_cast<int>(_shots.size())) return;
    beginRemoveRows({}, index, index);
    _shots.erase(_shots.begin() + index);
    endRemoveRows();
    if (_currentIndex >= static_cast<int>(_shots.size()))
        _currentIndex = static_cast<int>(_shots.size()) - 1;
}

void PlaylistModel::clear()
{
    _cancelThumbnailJobs();
    beginResetModel();
    _shots.clear();
    _currentIndex = -1;
    endResetModel();
}

void PlaylistModel::moveUp(int index)
{
    if (index <= 0 || index >= static_cast<int>(_shots.size())) return;
    beginMoveRows({}, index, index, {}, index - 1);
    std::swap(_shots[index], _shots[index - 1]);
    endMoveRows();
}

void PlaylistModel::moveDown(int index)
{
    if (index < 0 || index >= static_cast<int>(_shots.size()) - 1) return;
    beginMoveRows({}, index + 1, index + 1, {}, index);
    std::swap(_shots[index], _shots[index + 1]);
    endMoveRows();
}

void PlaylistModel::sort(bool ascending)
{
    beginResetModel();
    std::sort(_shots.begin(), _shots.end(),
              [ascending](const ShotItem& a, const ShotItem& b) {
                  return ascending ? (a.name < b.name) : (a.name > b.name);
              });
    endResetModel();
}

const ShotItem& PlaylistModel::shotAt(int index) const
{
    static ShotItem empty;
    if (index < 0 || index >= static_cast<int>(_shots.size())) return empty;
    return _shots[index];
}

int PlaylistModel::currentIndex() const { return _currentIndex; }

void PlaylistModel::setStatusColor(int index, const QColor& color)
{
    if (index < 0 || index >= static_cast<int>(_shots.size())) return;
    _shots[index].statusColor = color;
    Q_EMIT dataChanged(this->index(index), this->index(index), {StatusColorRole});
}

QColor PlaylistModel::statusColorAt(int index) const
{
    if (index < 0 || index >= static_cast<int>(_shots.size())) return {};
    return _shots[index].statusColor;
}

void PlaylistModel::setCurrentIndex(int index)
{
    if (index < 0 || index >= static_cast<int>(_shots.size())) return;
    int prev = _currentIndex;
    _currentIndex = index;

    // Notify view to repaint old and new rows
    if (prev >= 0) Q_EMIT dataChanged(this->index(prev), this->index(prev));
    Q_EMIT dataChanged(this->index(index), this->index(index));
    Q_EMIT currentChanged(index, _shots[index]);
}

// ─── Helpers ─────────────────────────────────────────────────────────────────
ShotItem PlaylistModel::_buildShotItem(
    const QString& path,
    const OcioManager::PreviewTransformSettings& previewSettings,
    double fpsOverride,
    const MediaInfo* mediaInfo)
{
    ShotItem item;
    item.path   = path;
    const cgplay::MediaInfo probedMedia = mediaInfo
        ? cgplay::MediaInfo{}
        : cgplay::MediaProbe::probe(path, fpsOverride);
    const cgplay::MediaInfo& media = mediaInfo ? *mediaInfo : probedMedia;
    item.format = media.formatLabel.isEmpty() ? _detectFormat(path) : media.formatLabel;
    item.name = media.name.isEmpty() ? _displayName(path) : media.name;
    item.firstFrame = media.firstFrame;
    item.lastFrame = media.lastFrame >= media.firstFrame
        ? media.lastFrame
        : media.firstFrame + std::max(1, media.effectiveFrameCount()) - 1;
    item.width = media.width;
    item.height = media.height;
    item.fps = media.fps > 0.0 ? media.fps : 24.0;

    const QString lowerPath = path.toLower();
    if (lowerPath.startsWith(":/cgplay/")) {
        QPixmap px(path);
        if (!px.isNull()) {
            item.thumbnail = QIcon(px);
        }
    }

    return item;
}

void PlaylistModel::_queueThumbnail(const QString& path)
{
    const quint64 generation = _thumbnailGeneration;
    QTimer::singleShot(kThumbnailStartDelayMs, this, [this, path, generation] {
        if (generation == _thumbnailGeneration) _scheduleThumbnail(path);
    });
}

void PlaylistModel::_scheduleThumbnail(const QString& path)
{
    if (path.isEmpty() || !QFileInfo::exists(path)) return;
    for (const auto& job : _thumbnailJobs) {
        if (job && job->property("cgplay.thumbnailPath").toString() == path) return;
    }

    const auto shotIt = std::find_if(_shots.cbegin(), _shots.cend(), [&path](const ShotItem& shot) {
        return shot.path == path && shot.thumbnail.isNull();
    });
    if (shotIt == _shots.cend()) return;

    MediaInfo media;
    media.path = shotIt->path;
    media.name = shotIt->name;
    media.formatLabel = shotIt->format;
    media.firstFrame = shotIt->firstFrame;
    media.lastFrame = shotIt->lastFrame;
    media.frameCount = std::max(1, shotIt->frameCount());
    media.width = shotIt->width;
    media.height = shotIt->height;
    media.fps = shotIt->fps;
    media.exists = true;
    media.isStillImage = MediaProbe::isStillImagePath(path);
    media.isVideo = MediaProbe::isVideoPath(path);
    const auto previewSettings = _previewSettings;
    const quint64 generation = _thumbnailGeneration;
    auto* job = JobRunner::start(
        QStringLiteral("playlist.thumbnail"),
        this,
        30000,
        [path, previewSettings, media](JobContext& context) {
            const QImage image = ThumbnailService::makePlaylistImage(
                path, previewSettings, &media, &context);
            if (context.isCancellationRequested()) return JobOutcome::canceled();
            if (context.hasTimedOut()) return JobOutcome::timedOut();
            return image.isNull()
                ? JobOutcome::failure(QStringLiteral("thumbnail-failed"), QStringLiteral("Unable to generate thumbnail"))
                : JobOutcome::success(QVariant::fromValue(image));
        });
    job->setProperty("cgplay.thumbnailPath", path);
    _thumbnailJobs.push_back(job);
    connect(job, &JobHandle::finished, this,
        [this, job, path, generation](const JobOutcome& outcome) {
            _thumbnailJobs.erase(
                std::remove_if(_thumbnailJobs.begin(), _thumbnailJobs.end(),
                    [job](const QPointer<JobHandle>& candidate) {
                        return candidate.isNull() || candidate == job;
                    }),
                _thumbnailJobs.end());
            job->deleteLater();
            if (generation != _thumbnailGeneration || !outcome.succeeded()) return;
            const QImage image = outcome.value.value<QImage>();
            if (image.isNull()) return;
            for (int row = 0; row < static_cast<int>(_shots.size()); ++row) {
                ShotItem& shot = _shots[static_cast<size_t>(row)];
                if (shot.path != path || !shot.thumbnail.isNull()) continue;
                shot.thumbnail = QIcon(QPixmap::fromImage(image));
                Q_EMIT dataChanged(index(row), index(row), {Qt::DecorationRole});
            }
        });
}

void PlaylistModel::_cancelThumbnailJobs()
{
    ++_thumbnailGeneration;
    for (const auto& job : _thumbnailJobs) {
        if (job) job->cancel();
    }
    _thumbnailJobs.clear();
}

QString PlaylistModel::_detectFormat(const QString& path)
{
    static const QStringList exrExts  {"exr"};
    static const QStringList dpxExts  {"dpx"};
    static const QStringList imgExts  {"tif","tiff","png","jpg","jpeg","tga"};

    QString ext = QFileInfo(path).suffix().toLower();
    if (exrExts.contains(ext))  return "EXR";
    if (dpxExts.contains(ext))  return "DPX";
    if (MediaProbe::isVideoPath(path)) return "视频";
    if (imgExts.contains(ext))  return "图片";
    return "未知";
}

QString PlaylistModel::_detectFormatPublic(const QString& path)
{
    return _detectFormat(path);
}

QString PlaylistModel::_displayName(const QString& path)
{
    QFileInfo fi(path);
    // Strip frame number suffix for sequences
    static QRegularExpression seqRe(R"((.+?)\.\d+\.[a-zA-Z]+$)");
    auto m = seqRe.match(fi.fileName());
    if (m.hasMatch()) return m.captured(1);
    return fi.completeBaseName();
}

// ─── v1.4 Session 序列化 ────────────────────────────────────────────────
QJsonObject PlaylistModel::toJson() const
{
    QJsonObject root;
    root["current_index"] = _currentIndex;

    QJsonArray arr;
    for (const auto& s : _shots) {
        QJsonObject o;
        o["path"]        = s.path;
        o["name"]        = s.name;
        o["first_frame"] = s.firstFrame;
        o["last_frame"]  = s.lastFrame;
        o["format"]      = s.format;
        o["width"]       = s.width;
        o["height"]      = s.height;
        o["fps"]         = s.fps;
        o["track_kind"]  = s.trackKind;
        o["track_name"]  = s.trackName;
        if (s.statusColor.isValid())
            o["status_color"] = s.statusColor.name(QColor::HexArgb);
        arr.append(o);
    }
    root["shots"] = arr;
    return root;
}

void PlaylistModel::fromJson(const QJsonObject& root)
{
    _cancelThumbnailJobs();
    beginResetModel();
    _shots.clear();
    _currentIndex = root["current_index"].toInt(-1);

    const QJsonArray arr = root["shots"].toArray();
    for (const auto& v : arr) {
        QJsonObject o = v.toObject();
        ShotItem s;
        s.path       = o["path"].toString();
        s.name       = o["name"].toString();
        s.firstFrame = o["first_frame"].toInt(0);
        s.lastFrame  = o["last_frame"].toInt(0);
        s.format     = o["format"].toString("未知");
        s.width      = o["width"].toInt(0);
        s.height     = o["height"].toInt(0);
        s.fps        = o["fps"].toDouble(24.0);
        s.trackKind  = o["track_kind"].toString("Video");
        s.trackName  = o["track_name"].toString();
        if (o.contains("status_color"))
            s.statusColor = QColor(o["status_color"].toString());
        if (QFileInfo::exists(s.path)) {
            const cgplay::MediaInfo media = cgplay::MediaProbe::probe(s.path, s.fps > 0.0 ? s.fps : 0.0);
            s.name = media.name.isEmpty() ? (s.name.isEmpty() ? _displayName(s.path) : s.name) : media.name;
            s.format = media.formatLabel.isEmpty() ? _detectFormat(s.path) : media.formatLabel;
            s.width = media.width;
            s.height = media.height;
            s.fps = media.fps > 0.0 ? media.fps : 24.0;
            s.firstFrame = media.firstFrame;
            s.lastFrame = media.lastFrame >= media.firstFrame
                ? media.lastFrame
                : media.firstFrame + std::max(1, media.effectiveFrameCount()) - 1;
        }
        _shots.push_back(s);
    }
    endResetModel();
    for (const auto& shot : _shots) {
        _queueThumbnail(shot.path);
    }
}

} // namespace cgplay
