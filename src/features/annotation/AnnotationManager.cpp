// CGPlay AnnotationManager.cpp

#include "AnnotationManager.h"

#include "common/core/ServiceLocator.h"
#include "common/events/api/EventTypes.h"
#include "common/events/api/IEventBus.h"

#include <QJsonArray>

namespace cgplay {

AnnotationManager::AnnotationManager(QObject* parent)
    : QObject(parent) {}

// ─── CRUD ──────────────────────────────────────────────────────────────────────
QString AnnotationManager::add(const AnnotationItem& ann)
{
    auto item = ann;
    if (item.id.isEmpty()) {
        item.id = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    }
    _annotations.append(item);
    _pushUndo("add", item.id);
    _notify();
    return item.id;
}

bool AnnotationManager::remove(const QString& annId)
{
    for (int i = 0; i < _annotations.size(); ++i) {
        if (_annotations[i].id == annId) {
            _pushUndo("remove", QVariant::fromValue(_annotations[i]));
            _annotations.removeAt(i);
            _notify();
            return true;
        }
    }
    return false;
}

bool AnnotationManager::removeAtFrame(int frame)
{
    bool removed = false;
    for (int i = _annotations.size() - 1; i >= 0; --i) {
        if (_annotations[i].frame == frame) {
            _pushUndo("remove", QVariant::fromValue(_annotations[i]));
            _annotations.removeAt(i);
            removed = true;
        }
    }
    if (removed) _notify();
    return removed;
}

bool AnnotationManager::update(const QString& annId, const AnnotationItem& newAnn)
{
    for (int i = 0; i < _annotations.size(); ++i) {
        if (_annotations[i].id == annId) {
            // Save old state for undo
            QVariantMap oldState;
            oldState["id"]     = _annotations[i].id;
            oldState["type"]   = static_cast<int>(_annotations[i].type);
            oldState["color"]  = _annotations[i].color;
            oldState["comment"] = _annotations[i].comment;
            oldState["points"] = QVariant::fromValue(_annotations[i].points);
            oldState["status"] = static_cast<int>(_annotations[i].status);
            oldState["assignee"] = _annotations[i].assignee;

            // Apply new values
            _annotations[i].type    = newAnn.type;
            _annotations[i].color   = newAnn.color;
            _annotations[i].comment = newAnn.comment;
            _annotations[i].points  = newAnn.points;
            _annotations[i].status  = newAnn.status;
            _annotations[i].assignee = newAnn.assignee;

            _pushUndo("update", oldState);
            _notify();
            return true;
        }
    }
    return false;
}

AnnotationItem* AnnotationManager::get(const QString& annId)
{
    for (auto& a : _annotations) {
        if (a.id == annId) return &a;
    }
    return nullptr;
}

const AnnotationItem* AnnotationManager::get(const QString& annId) const
{
    for (const auto& a : _annotations) {
        if (a.id == annId) return &a;
    }
    return nullptr;
}

QVector<AnnotationItem> AnnotationManager::getAtFrame(int frame) const
{
    _rebuildFrameLookupCache();
    return _annotationsByFrameCache.value(frame);
}

QSet<int> AnnotationManager::getFramesWithAnnotations() const
{
    _rebuildFrameLookupCache();
    QSet<int> frames;
    for (auto it = _annotationsByFrameCache.cbegin(); it != _annotationsByFrameCache.cend(); ++it) {
        frames.insert(it.key());
    }
    return frames;
}

QVector<AnnotationItem> AnnotationManager::all() const
{
    return _annotations;
}

void AnnotationManager::clear()
{
    _annotations.clear();
    _selectedAnnotationId.clear();
    _undoStack.clear();
    _redoStack.clear();
    _notify();
}

// ─── Undo / Redo ───────────────────────────────────────────────────────────────
void AnnotationManager::_pushUndo(const QString& opType, const QVariant& data)
{
    _undoStack.append({opType, data});
    if (_undoStack.size() > kMaxUndo)
        _undoStack.removeFirst();
    _redoStack.clear(); // new action resets redo history
}

bool AnnotationManager::undo()
{
    if (_undoStack.isEmpty()) return false;

    auto entry = _undoStack.takeLast();
    if (entry.opType == "add") {
        // Undo add: remove by id
        QString id = entry.data.toString();
        for (int i = 0; i < _annotations.size(); ++i) {
            if (_annotations[i].id == id) {
                _redoStack.append({"add", QVariant::fromValue(_annotations[i])});
                _annotations.removeAt(i);
                _notify();
                return true;
            }
        }
    } else if (entry.opType == "remove") {
        // Undo remove: restore full item
        AnnotationItem restored = entry.data.value<AnnotationItem>();
        if (restored.isValid()) {
            _annotations.append(restored);
            _redoStack.append({"remove", restored.id});
            _notify();
            return true;
        }
        return false;
    } else if (entry.opType == "update") {
        QString id = entry.data.toMap()["id"].toString();
        for (auto& a : _annotations) {
            if (a.id == id) {
                auto old = entry.data.toMap();
                // Swap old state back
                AnnotationItem backup = a;
                a.type    = static_cast<AnnotationType>(old["type"].toInt());
                a.color   = old["color"].value<QColor>();
                a.comment = old["comment"].toString();
                a.points  = old["points"].value<QVector<QPointF>>();
                a.status  = static_cast<ReviewStatus>(old["status"].toInt());
                a.assignee= old["assignee"].toString();

                QVariantMap newState;
                newState["id"] = a.id;
                newState["type"] = static_cast<int>(backup.type);
                newState["color"] = backup.color;
                newState["comment"] = backup.comment;
                newState["points"] = QVariant::fromValue(backup.points);
                newState["status"] = static_cast<int>(backup.status);
                newState["assignee"] = backup.assignee;
                _redoStack.append({"update", newState});
                _notify();
                return true;
            }
        }
    }
    return false;
}

bool AnnotationManager::redo()
{
    if (_redoStack.isEmpty()) return false;

    auto entry = _redoStack.takeLast();
    if (entry.opType == "add") {
        _annotations.append(entry.data.value<AnnotationItem>());
        _undoStack.append({"add", _annotations.last().id});
        if (_undoStack.size() > kMaxUndo)
            _undoStack.removeFirst();
        _notify();
        return true;
    } else if (entry.opType == "remove") {
        QString id = entry.data.toString();
        for (int i = 0; i < _annotations.size(); ++i) {
            if (_annotations[i].id == id) {
                _undoStack.append({"remove", QVariant::fromValue(_annotations[i])});
                if (_undoStack.size() > kMaxUndo)
                    _undoStack.removeFirst();
                _annotations.removeAt(i);
                _notify();
                return true;
            }
        }
    } else if (entry.opType == "update") {
        QString id = entry.data.toMap()["id"].toString();
        for (auto& a : _annotations) {
            if (a.id == id) {
                auto newState = entry.data.toMap();
                QVariantMap oldState;
                oldState["id"] = a.id;
                oldState["type"] = static_cast<int>(a.type);
                oldState["color"] = a.color;
                oldState["comment"] = a.comment;
                oldState["points"] = QVariant::fromValue(a.points);
                oldState["status"] = static_cast<int>(a.status);
                oldState["assignee"] = a.assignee;

                a.type    = static_cast<AnnotationType>(newState["type"].toInt());
                a.color   = newState["color"].value<QColor>();
                a.comment = newState["comment"].toString();
                a.points  = newState["points"].value<QVector<QPointF>>();
                a.status  = static_cast<ReviewStatus>(newState["status"].toInt());
                a.assignee= newState["assignee"].toString();

                _undoStack.append({"update", oldState});
                if (_undoStack.size() > kMaxUndo)
                    _undoStack.removeFirst();
                _notify();
                return true;
            }
        }
    }
    return false;
}

void AnnotationManager::clearUndoStack()
{
    _undoStack.clear();
    _redoStack.clear();
}

// ─── 批量操作 ──────────────────────────────────────────────────────────────────
void AnnotationManager::fromList(const QVector<AnnotationItem>& list)
{
    _annotations = list;
    _notify();
}

QVector<AnnotationItem> AnnotationManager::toList() const
{
    return _annotations;
}

QString AnnotationManager::getFrameSummary(int frame) const
{
    auto anns = getAtFrame(frame);
    if (anns.isEmpty()) return {};
    QStringList previews;
    for (const auto& a : anns) {
        QString t = a.latestCommentText();
        if (!t.isEmpty())
            previews.append(t.mid(0, 20));
    }
    QString summary = QString("%1 annotations").arg(anns.size());
    if (!previews.isEmpty())
        summary += ": " + previews.join("; ");
    return summary;
}

QVector<AnnotationItem> AnnotationManager::annotations() const
{
    return all();
}

void AnnotationManager::replaceAnnotations(const QVector<AnnotationItem>& annotations)
{
    fromList(annotations);
}

void AnnotationManager::clearAnnotations()
{
    clear();
}

QString AnnotationManager::addAnnotation(const AnnotationItem& annotation)
{
    return add(annotation);
}

bool AnnotationManager::removeAnnotation(const QString& annotationId)
{
    return remove(annotationId);
}

bool AnnotationManager::removeSelectedAnnotation()
{
    return !_selectedAnnotationId.isEmpty() && remove(_selectedAnnotationId);
}

bool AnnotationManager::updateAnnotationComment(const QString& annotationId, const QString& text)
{
    auto* ann = get(annotationId);
    if (!ann) {
        return false;
    }

    AnnotationItem updated = *ann;
    updated.comment = text;
    if (!updated.comments.isEmpty()) {
        updated.comments.last().text = text;
    }
    return update(annotationId, updated);
}

QString AnnotationManager::createNote(const QString& text)
{
    AnnotationItem ann;
    ann.type = AnnotationType::Point;
    ann.comment = text;
    ann.author = QString::fromUtf8("用户");
    ann.status = ReviewStatus::Open;
    ann.color = _currentToolColor;
    ann.points.append(QPointF(0.5, 0.5));
    return addAnnotation(ann);
}

void AnnotationManager::selectAnnotation(const QString& annotationId)
{
    if (_selectedAnnotationId == annotationId) {
        return;
    }
    _selectedAnnotationId = annotationId;
    _notify();
}

QString AnnotationManager::selectedAnnotationId() const
{
    return _selectedAnnotationId;
}

void AnnotationManager::setTool(int tool)
{
    if (_currentTool == tool) {
        return;
    }
    _currentTool = tool;
    _notify();
}

int AnnotationManager::currentTool() const
{
    return _currentTool;
}

void AnnotationManager::setToolColor(const QColor& color)
{
    if (_currentToolColor == color) {
        return;
    }
    _currentToolColor = color;
    _notify();
}

QColor AnnotationManager::currentToolColor() const
{
    return _currentToolColor;
}

QString AnnotationManager::sessionKey() const
{
    return QStringLiteral("annotations");
}

void AnnotationManager::serializeInto(QJsonObject& root) const
{
    QJsonArray annArr;
    for (const auto& a : _annotations) {
        QJsonObject ao;
        ao["id"] = a.id;
        ao["frame"] = a.frame;
        ao["type"] = static_cast<int>(a.type);
        ao["color"] = a.color.name(QColor::HexArgb);
        ao["author"] = a.author;
        ao["created"] = a.createdTime;
        ao["comment"] = a.comment;
        ao["status"] = static_cast<int>(a.status);
        ao["assignee"] = a.assignee;

        QJsonArray pts;
        for (const auto& p : a.points) {
            pts.append(QJsonArray{ p.x(), p.y() });
        }
        ao["points"] = pts;

        QJsonArray comments;
        for (const auto& c : a.comments) {
            QJsonObject co;
            co["author"] = c.author;
            co["text"] = c.text;
            co["time"] = c.time;
            co["status"] = static_cast<int>(c.status);
            co["assignee"] = c.assignee;
            comments.append(co);
        }
        ao["comments"] = comments;
        annArr.append(ao);
    }
    root[sessionKey()] = annArr;
}

void AnnotationManager::deserializeFrom(const QJsonObject& root)
{
    if (!root.contains(sessionKey())) {
        return;
    }

    clear();
    const QJsonArray annArr = root.value(sessionKey()).toArray();
    for (const auto& v : annArr) {
        const QJsonObject ao = v.toObject();
        AnnotationItem a;
        a.id = ao["id"].toString();
        a.frame = ao["frame"].toInt(0);
        a.type = static_cast<AnnotationType>(ao["type"].toInt(0));
        a.color = QColor(ao["color"].toString());
        a.author = ao["author"].toString("user");
        a.createdTime = ao["created"].toString();
        a.comment = ao["comment"].toString();
        a.status = static_cast<ReviewStatus>(ao["status"].toInt(0));
        a.assignee = ao["assignee"].toString("Other");

        const QJsonArray pts = ao["points"].toArray();
        for (const auto& p : pts) {
            const QJsonArray xy = p.toArray();
            a.points.append(QPointF(xy[0].toDouble(), xy[1].toDouble()));
        }

        const QJsonArray comments = ao["comments"].toArray();
        for (const auto& cv : comments) {
            const QJsonObject co = cv.toObject();
            Comment c;
            c.author = co["author"].toString();
            c.text = co["text"].toString();
            c.time = co["time"].toString();
            c.status = static_cast<ReviewStatus>(co["status"].toInt(0));
            c.assignee = co["assignee"].toString();
            a.comments.append(c);
        }
        add(a);
    }
}

// ─── Private ───────────────────────────────────────────────────────────────────
void AnnotationManager::_rebuildFrameLookupCache() const
{
    if (!_annotationsByFrameDirty) {
        return;
    }

    _annotationsByFrameCache.clear();
    for (const auto& annotation : _annotations) {
        _annotationsByFrameCache[annotation.frame].append(annotation);
    }
    _annotationsByFrameDirty = false;
}

void AnnotationManager::_notify()
{
    _annotationsByFrameDirty = true;
    Q_EMIT dataChanged();
    if (auto* eventBus = ServiceLocator::getService<IEventBus>()) {
        eventBus->publish(AnnotationChangedEvent{ _annotations });
    }
    if (_onChanged)
        _onChanged();
}

} // namespace cgplay
