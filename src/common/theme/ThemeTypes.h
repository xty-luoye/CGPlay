#pragma once

#include <QColor>
#include <QJsonObject>
#include <QPalette>
#include <QString>

namespace cgplay {

enum class ThemeMode { Dark, Light, Glass, HighContrast, Custom };
enum class BackgroundType { Solid, Gradient, Image, Texture };

struct ThemeDefinition {
    QString name = QStringLiteral("默认深色");
    ThemeMode mode = ThemeMode::Dark;
    BackgroundType backgroundType = BackgroundType::Solid;
    QColor background = QColor(QStringLiteral("#10161D"));
    QColor backgroundSecondary = QColor(QStringLiteral("#18212B"));
    QColor panel = QColor(QStringLiteral("#18212B"));
    QColor toolbar = QColor(QStringLiteral("#18212B"));
    QColor timeline = QColor(QStringLiteral("#0D1218"));
    QColor text = QColor(QStringLiteral("#D8DEE7"));
    QColor border = QColor(QStringLiteral("#303B47"));
    QColor accent = QColor(QStringLiteral("#FF8A3D"));
    QString backgroundImage;
    QString texturePath;
    QString fillMode = QStringLiteral("cover");
    int backgroundOpacity = 100;
    int panelOpacity = 96;
    int toolbarOpacity = 92;
    int timelineOpacity = 96;
    int subtitleOpacity = 86;
    int viewerOpacity = 100;
    int shadowStrength = 25;
    int blurRadius = 0;
    int vignette = 0;
    int brightness = 100;
    int saturation = 100;
    bool dynamicBackground = false;

    void normalize();
    QJsonObject toJson() const;
    static ThemeDefinition fromJson(const QJsonObject& object, bool* valid = nullptr);
};

// Runtime colors are derived once from a ThemeDefinition and then shared by
// the application palette and the top-level surface styles.  Every color in
// this structure is opaque so a translucent setting cannot expose a stale
// parent background.
struct ThemeTokens {
    QColor surfaceMain;
    QColor surfacePanel;
    QColor surfaceToolbar;
    QColor surfaceTimeline;
    QColor surfaceViewer;
    QColor surfaceDialog;
    QColor surfaceCodex;
    QColor textPrimary;
    QColor textSecondary;
    QColor textDisabled;
    QColor textOnAccent;
    QColor borderNormal;
    QColor borderFocus;
    QColor accentNormal;
    QColor accentHover;
    QColor accentPressed;
    QColor selection;
    QColor selectionText;
    QColor tooltipBackground;
    QColor tooltipText;
    QColor subtitleBackground;
    QColor sliderTrack;
};

ThemeDefinition defaultThemeForMode(ThemeMode mode);
ThemeTokens buildThemeTokens(const ThemeDefinition& definition);
QPalette buildThemePalette(const ThemeTokens& tokens);

// Composite a foreground surface over a known backdrop.  The result is
// always opaque and is therefore safe for QPalette and stylesheet surfaces.
QColor composeSurface(const QColor& foreground, const QColor& backdrop, int opacity);

QString themeModeName(ThemeMode mode);
ThemeMode themeModeFromName(const QString& value);
QString backgroundTypeName(BackgroundType type);
BackgroundType backgroundTypeFromName(const QString& value);

} // namespace cgplay
