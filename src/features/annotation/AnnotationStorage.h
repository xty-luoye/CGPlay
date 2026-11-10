#pragma once
// CGPlay AnnotationStorage.h — .review.json 持久化

#include <QString>
#include <QVector>

namespace cgplay {

struct AnnotationItem;

class AnnotationStorage
{
public:
    static bool save(const QVector<AnnotationItem>& annotations,
                     const QString& filepath);
    static QVector<AnnotationItem> load(const QString& filepath);
    // On failure the caller's collection is left intact. An empty valid file
    // succeeds and deliberately replaces it with an empty collection.
    static bool load(const QString& filepath, QVector<AnnotationItem>* annotations,
                     QString* error = nullptr);

    static QString getReviewPath(const QString& mediaPath);
};

} // namespace cgplay
