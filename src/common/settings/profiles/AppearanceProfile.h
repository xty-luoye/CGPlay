#pragma once

#include <QJsonObject>
#include <QMap>
#include <QString>

namespace cgplay {

struct ButtonStyleProfile
{
    QString background;
    QString hover;
    QString pressed;
    QString disabled;
    QString text;
    QString icon;
    QString border;
    int radius = 6;
    int iconSize = 18;
    int opacity = 88;
    int borderOpacity = 100;
    int shadow = 0;
    int width = 0;
    int height = 0;
    bool showText = true;
    bool showIcon = true;

    QJsonObject toJson() const;
    static ButtonStyleProfile fromJson(const QJsonObject& object, bool* valid = nullptr,
                                       QString* error = nullptr);
    bool validate(QString* error = nullptr) const;
};

class AppearanceProfile
{
public:
    static constexpr int SchemaVersion = 1;

    static AppearanceProfile defaults();
    static QJsonObject migrate(const QJsonObject& object, bool* valid = nullptr,
                               QString* error = nullptr);
    static AppearanceProfile fromJson(const QJsonObject& object, bool* valid = nullptr,
                                      QString* error = nullptr);
    static AppearanceProfile fromPackageJson(const QJsonObject& theme,
                                             const QJsonObject& buttonStyles,
                                             bool* valid = nullptr,
                                             QString* error = nullptr);

    QJsonObject toJson() const;
    QJsonObject buttonStylesToJson() const;
    bool validate(QString* error = nullptr) const;

    QString themeName;
    QString mode = QStringLiteral("dark");
    QString backgroundType = QStringLiteral("solid");
    QString backgroundColor = QStringLiteral("#10161D");
    QString backgroundSecondary = QStringLiteral("#18212B");
    QString panelColor = QStringLiteral("#18212B");
    QString toolbarColor = QStringLiteral("#141C24");
    QString timelineColor = QStringLiteral("#0D1218");
    QString textColor = QStringLiteral("#D8DEE7");
    QString borderColor = QStringLiteral("#303B47");
    QString accentColor = QStringLiteral("#FF8A3D");
    QString backgroundImage;
    QString texturePath;
    QString fillMode = QStringLiteral("cover");
    int backgroundOpacity = 100;
    int panelOpacity = 96;
    int toolbarOpacity = 92;
    int timelineOpacity = 96;
    int subtitleOpacity = 86;
    int viewerOpacity = 100;
    int windowOpacity = 100;
    int buttonOpacity = 88;
    int vignette = 0;
    int blurRadius = 0;
    int brightness = 100;
    int saturation = 100;
    int shadowStrength = 25;
    bool dynamicBackground = false;
    QMap<QString, ButtonStyleProfile> buttonStyles;
};

} // namespace cgplay
