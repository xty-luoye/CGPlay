// CGPlay AnnotationStorage.cpp

#include "AnnotationStorage.h"
#include "AnnotationItem.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug>

namespace cgplay {

bool AnnotationStorage::save(const QVector<AnnotationItem>& annotations,
                              const QString& filepath)
{
    QFileInfo fi(filepath);
    QDir().mkpath(fi.path());

    QJsonObject root;
    root["version"] = 1;

    QJsonArray arr;
    for (const auto& ann : annotations) {
        QJsonObject obj;
        obj["id"]           = ann.id;
        obj["frame"]        = ann.frame;
        obj["type"]         = annotationTypeString(ann.type);
        obj["color"]        = ann.color.name();
        obj["author"]       = ann.author;
        obj["created_time"] = ann.createdTime;
        obj["comment"]      = ann.comment;

        QJsonArray ptsArr;
        for (const auto& pt : ann.points) {
            QJsonArray ptArr;
            ptArr.append(pt.x());
            ptArr.append(pt.y());
            ptsArr.append(ptArr);
        }
        obj["points"] = ptsArr;

        QJsonArray cmtsArr;
        for (const auto& c : ann.comments) {
            QJsonObject co;
            co["author"]   = c.author;
            co["text"]     = c.text;
            co["time"]     = c.time;
            co["status"]   = reviewStatusString(c.status);
            co["assignee"] = c.assignee;
            cmtsArr.append(co);
        }
        obj["comments"] = cmtsArr;
        obj["status"]   = reviewStatusString(ann.status);
        obj["assignee"] = ann.assignee;

        arr.append(obj);
    }
    root["annotations"] = arr;

    QFile f(filepath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "[AnnotationStorage] Cannot write:" << filepath;
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    f.close();
    return true;
}

QVector<AnnotationItem> AnnotationStorage::load(const QString& filepath)
{
    QVector<AnnotationItem> result;

    QFile f(filepath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return result;

    QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();

    if (!doc.isObject()) return result;

    auto root = doc.object();
    auto arr  = root["annotations"].toArray();

    for (const auto& val : arr) {
        auto obj = val.toObject();
        AnnotationItem ann;
        ann.id          = obj["id"].toString();
        ann.frame       = obj["frame"].toInt();
        ann.type        = annotationTypeFromString(obj["type"].toString());
        ann.color       = QColor(obj["color"].toString());
        ann.author      = obj["author"].toString();
        ann.createdTime = obj["created_time"].toString();
        ann.comment     = obj["comment"].toString();

        auto ptsArr = obj["points"].toArray();
        for (const auto& p : ptsArr) {
            auto ptArr = p.toArray();
            if (ptArr.size() >= 2)
                ann.points.append(QPointF(ptArr[0].toDouble(), ptArr[1].toDouble()));
        }

        auto cmtsArr = obj["comments"].toArray();
        for (const auto& c : cmtsArr) {
            auto co = c.toObject();
            Comment comment;
            comment.author   = co["author"].toString("user");
            comment.text     = co["text"].toString();
            comment.time     = co["time"].toString();
            comment.status   = reviewStatusFromString(co["status"].toString("Open"));
            comment.assignee = co["assignee"].toString("Other");
            ann.comments.append(comment);
        }
        ann.status   = reviewStatusFromString(obj["status"].toString("Open"));
        ann.assignee = obj["assignee"].toString("Other");

        result.append(ann);
    }
    return result;
}

QString AnnotationStorage::getReviewPath(const QString& mediaPath)
{
    QFileInfo fi(mediaPath);
    if (fi.isDir())
        return fi.absoluteFilePath() + ".review.json";
    return fi.absolutePath() + "/" + fi.completeBaseName() + ".review.json";
}

} // namespace cgplay
