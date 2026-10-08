#pragma once
// CGPlay AnnotationManager.h — 批注 CRUD、撤销、信号通知

#include "AnnotationItem.h"
#include "annotation/api/IAnnotationService.h"
#include "core/session/api/ISessionContributor.h"

#include <QObject>
#include <QVector>
#include <QSet>
#include <QColor>
#include <QJsonObject>
#include <functional>
#include <QPair>
#include <QVariant>
#include <QHash>

namespace cgplay {

// ─── AnnotationManager ──────────────────────────────────────────────────────
class AnnotationManager : public QObject,
                          public IAnnotationService,
                          public ISessionContributor
{
    Q_OBJECT
public:
    explicit AnnotationManager(QObject* parent = nullptr);
    ~AnnotationManager() override = default;

    int count() const override { return static_cast<int>(_annotations.size()); }
    QVector<AnnotationItem> annotations() const override;
    void replaceAnnotations(const QVector<AnnotationItem>& annotations) override;
    void clearAnnotations() override;
    QString addAnnotation(const AnnotationItem& annotation) override;
    bool removeAnnotation(const QString& annotationId) override;
    bool removeSelectedAnnotation() override;
    bool updateAnnotationComment(const QString& annotationId, const QString& text) override;
    QString createNote(const QString& text) override;
    void selectAnnotation(const QString& annotationId) override;
    QString selectedAnnotationId() const override;
    void setTool(int tool) override;
    int currentTool() const override;
    void setToolColor(const QColor& color) override;
    QColor currentToolColor() const override;

    QString sessionKey() const override;
    void serializeInto(QJsonObject& root) const override;
    void deserializeFrom(const QJsonObject& root) override;

    // ── 变更回调 ──────────────────────────────────────────────────────
    void setOnChanged(std::function<void()> cb) { _onChanged = std::move(cb); }

    // ── CRUD ──────────────────────────────────────────────────────────
    QString add(const AnnotationItem& ann);
    bool    remove(const QString& annId);
    bool    removeAtFrame(int frame);   // 返回是否删除了
    bool    update(const QString& annId, const AnnotationItem& newAnn);
    AnnotationItem* get(const QString& annId);
    const AnnotationItem* get(const QString& annId) const;
    QVector<AnnotationItem> getAtFrame(int frame) const;
    QSet<int>   getFramesWithAnnotations() const;
    QVector<AnnotationItem> all() const;
    void        clear();
    int         frameCount() const { return static_cast<int>(getFramesWithAnnotations().size()); }

    // ── Undo / Redo ───────────────────────────────────────────────────
    bool canUndo() const { return !_undoStack.isEmpty(); }
    bool canRedo() const { return !_redoStack.isEmpty(); }
    bool undo() override;
    bool redo() override;
    void clearUndoStack();

    // ── 批量操作 ──────────────────────────────────────────────────────
    void fromList(const QVector<AnnotationItem>& list);
    QVector<AnnotationItem> toList() const;

    // ── 帧摘要 ────────────────────────────────────────────────────────
    QString getFrameSummary(int frame) const;

Q_SIGNALS:
    void dataChanged();

private:
    void _rebuildFrameLookupCache() const;
    void _notify();
    void _pushUndo(const QString& opType, const QVariant& data);

    QVector<AnnotationItem> _annotations;
    std::function<void()>   _onChanged;
    QString _selectedAnnotationId;
    int _currentTool = 0;
    QColor _currentToolColor = QColor(255, 0, 0);

    // Undo stack
    struct UndoEntry {
        QString  opType;    // "add", "remove", "update"
        QVariant data;      // depends on opType
    };
    QVector<UndoEntry> _undoStack;
    QVector<UndoEntry> _redoStack;
    mutable QHash<int, QVector<AnnotationItem>> _annotationsByFrameCache;
    mutable bool _annotationsByFrameDirty = true;
    static constexpr int kMaxUndo = 50;
};

} // namespace cgplay
