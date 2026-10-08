#include "ThemeService.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QRegularExpression>

namespace cgplay {

namespace {
QString color(const QColor& value) { return value.isValid() ? value.name(QColor::HexArgb) : QStringLiteral("#000000"); }
int boundedOpacity(int value, int minimum) { return qBound(minimum, value, 100); }

ThemeDefinition modeDefaults(ThemeMode mode)
{
    ThemeDefinition result;
    result.mode = mode;
    switch (mode) {
    case ThemeMode::Light:
        result.name = QStringLiteral("Light");
        result.background = QColor(QStringLiteral("#F3F5F7"));
        result.backgroundSecondary = QColor(QStringLiteral("#E8EDF2"));
        result.panel = QColor(QStringLiteral("#FFFFFF"));
        result.toolbar = QColor(QStringLiteral("#FFFFFF"));
        result.timeline = QColor(QStringLiteral("#E1E6EC"));
        result.text = QColor(QStringLiteral("#18202A"));
        result.border = QColor(QStringLiteral("#B8C2CE"));
        result.accent = QColor(QStringLiteral("#C85A16"));
        result.backgroundOpacity = 100;
        result.panelOpacity = 100;
        result.toolbarOpacity = 100;
        result.timelineOpacity = 100;
        result.subtitleOpacity = 86;
        result.viewerOpacity = 100;
        break;
    case ThemeMode::HighContrast:
        result.name = QStringLiteral("High Contrast");
        result.background = QColor(QStringLiteral("#000000"));
        result.backgroundSecondary = QColor(QStringLiteral("#000000"));
        result.panel = QColor(QStringLiteral("#101010"));
        result.toolbar = QColor(QStringLiteral("#000000"));
        result.timeline = QColor(QStringLiteral("#000000"));
        result.text = QColor(QStringLiteral("#FFFFFF"));
        result.border = QColor(QStringLiteral("#FFFFFF"));
        result.accent = QColor(QStringLiteral("#FFFF00"));
        result.backgroundOpacity = 100;
        result.panelOpacity = 100;
        result.toolbarOpacity = 100;
        result.timelineOpacity = 100;
        result.subtitleOpacity = 100;
        result.viewerOpacity = 100;
        break;
    case ThemeMode::Glass:
        result.name = QStringLiteral("Glass");
        result.background = QColor(QStringLiteral("#101821"));
        result.backgroundSecondary = QColor(QStringLiteral("#1D2A36"));
        result.panel = QColor(QStringLiteral("#22313E"));
        result.toolbar = QColor(QStringLiteral("#1A2733"));
        result.timeline = QColor(QStringLiteral("#0D141C"));
        result.text = QColor(QStringLiteral("#E7EDF4"));
        result.border = QColor(QStringLiteral("#516170"));
        result.accent = QColor(QStringLiteral("#FF8A3D"));
        result.backgroundOpacity = 82;
        result.panelOpacity = 82;
        result.toolbarOpacity = 78;
        result.timelineOpacity = 94;
        result.subtitleOpacity = 88;
        result.viewerOpacity = 72;
        result.shadowStrength = 35;
        result.blurRadius = 12;
        break;
    case ThemeMode::Custom:
    case ThemeMode::Dark:
    default:
        result.name = QStringLiteral("Dark");
        result.mode = mode;
        result.background = QColor(QStringLiteral("#10161D"));
        result.backgroundSecondary = QColor(QStringLiteral("#18212B"));
        result.panel = QColor(QStringLiteral("#18212B"));
        result.toolbar = QColor(QStringLiteral("#141C24"));
        result.timeline = QColor(QStringLiteral("#0D1218"));
        result.text = QColor(QStringLiteral("#D8DEE7"));
        result.border = QColor(QStringLiteral("#303B47"));
        result.accent = QColor(QStringLiteral("#FF8A3D"));
        result.backgroundOpacity = 100;
        result.panelOpacity = 100;
        result.toolbarOpacity = 100;
        result.timelineOpacity = 100;
        result.subtitleOpacity = 86;
        result.viewerOpacity = 100;
        break;
    }
    return result;
}

QColor ensureOpaque(const QColor& colorValue, const QColor& fallback)
{
    const QColor value = colorValue.isValid() ? colorValue : fallback;
    QColor result = value;
    result.setAlpha(255);
    return result;
}

QColor blend(const QColor& foreground, const QColor& backdrop, int opacity)
{
    const QColor fg = ensureOpaque(foreground, QColor(Qt::black));
    const QColor bg = ensureOpaque(backdrop, QColor(Qt::black));
    const int alpha = qBound(0, opacity, 100) * 255 / 100;
    if (alpha >= 255) return fg;
    if (alpha <= 0) return bg;
    const int inverse = 255 - alpha;
    return QColor(
        (fg.red() * alpha + bg.red() * inverse) / 255,
        (fg.green() * alpha + bg.green() * inverse) / 255,
        (fg.blue() * alpha + bg.blue() * inverse) / 255,
        255);
}

QColor contrastingText(const QColor& background)
{
    const int luminance = (background.red() * 299 + background.green() * 587 + background.blue() * 114) / 1000;
    return luminance > 150 ? QColor(QStringLiteral("#101418")) : QColor(Qt::white);
}
}

QString themeModeName(ThemeMode mode)
{
    switch (mode) { case ThemeMode::Light: return QStringLiteral("light"); case ThemeMode::Glass: return QStringLiteral("glass"); case ThemeMode::HighContrast: return QStringLiteral("highContrast"); case ThemeMode::Custom: return QStringLiteral("custom"); default: return QStringLiteral("dark"); }
}

ThemeMode themeModeFromName(const QString& value)
{
    QString normalized = value.trimmed().toLower();
    normalized.remove(QLatin1Char('-')).remove(QLatin1Char('_')).remove(QLatin1Char(' '));
    if (normalized == QStringLiteral("light")) return ThemeMode::Light;
    if (normalized == QStringLiteral("glass")) return ThemeMode::Glass;
    if (normalized == QStringLiteral("highcontrast")) return ThemeMode::HighContrast;
    if (normalized == QStringLiteral("custom")) return ThemeMode::Custom;
    return ThemeMode::Dark;
}

QString backgroundTypeName(BackgroundType type)
{
    switch (type) { case BackgroundType::Gradient: return QStringLiteral("gradient"); case BackgroundType::Image: return QStringLiteral("image"); case BackgroundType::Texture: return QStringLiteral("texture"); default: return QStringLiteral("solid"); }
}

BackgroundType backgroundTypeFromName(const QString& value)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("gradient")) return BackgroundType::Gradient;
    if (normalized == QStringLiteral("image")) return BackgroundType::Image;
    if (normalized == QStringLiteral("texture")) return BackgroundType::Texture;
    return BackgroundType::Solid;
}

void ThemeDefinition::normalize()
{
    const ThemeDefinition defaults = modeDefaults(mode);
    backgroundOpacity = boundedOpacity(backgroundOpacity, 20);
    panelOpacity = boundedOpacity(panelOpacity, 35);
    toolbarOpacity = boundedOpacity(toolbarOpacity, 35);
    timelineOpacity = boundedOpacity(timelineOpacity, 45);
    subtitleOpacity = boundedOpacity(subtitleOpacity, 20);
    viewerOpacity = boundedOpacity(viewerOpacity, 20);
    shadowStrength = qBound(0, shadowStrength, 100);
    blurRadius = qBound(0, blurRadius, 64);
    vignette = qBound(0, vignette, 100);
    brightness = qBound(0, brightness, 200);
    saturation = qBound(0, saturation, 200);
    if (!background.isValid()) background = defaults.background;
    if (!backgroundSecondary.isValid()) backgroundSecondary = defaults.backgroundSecondary;
    if (!panel.isValid()) panel = defaults.panel;
    if (!toolbar.isValid()) toolbar = defaults.toolbar;
    if (!timeline.isValid()) timeline = defaults.timeline;
    if (!text.isValid()) text = defaults.text;
    if (!border.isValid()) border = defaults.border;
    if (!accent.isValid()) accent = defaults.accent;
}

ThemeDefinition defaultThemeForMode(ThemeMode mode)
{
    ThemeDefinition result = modeDefaults(mode);
    result.normalize();
    return result;
}

QColor composeSurface(const QColor& foreground, const QColor& backdrop, int opacity)
{
    // Include an explicit color alpha in the requested surface opacity, then
    // return an opaque result so parent widget backgrounds cannot leak through.
    const QColor fg = foreground.isValid() ? foreground : QColor(Qt::black);
    const int effectiveOpacity = qBound(0, opacity, 100) * fg.alpha() / 255;
    return blend(fg, backdrop, effectiveOpacity);
}

ThemeTokens buildThemeTokens(const ThemeDefinition& input)
{
    ThemeDefinition definition = input;
    definition.normalize();

    // High contrast is an accessibility override.  It deliberately ignores
    // custom colors so text and focus indicators retain a guaranteed contrast.
    if (definition.mode == ThemeMode::HighContrast) {
        definition = modeDefaults(ThemeMode::HighContrast);
        definition.normalize();
    }

    const ThemeDefinition defaults = modeDefaults(definition.mode);
    const QColor rootBackdrop = definition.mode == ThemeMode::Light
        ? QColor(QStringLiteral("#E6EBF0"))
        : QColor(QStringLiteral("#080B0F"));
    const QColor main = composeSurface(definition.background, rootBackdrop, definition.backgroundOpacity);

    ThemeTokens tokens;
    tokens.surfaceMain = main;
    tokens.surfacePanel = composeSurface(definition.panel, main, definition.panelOpacity);
    tokens.surfaceToolbar = composeSurface(definition.toolbar, main, definition.toolbarOpacity);
    tokens.surfaceTimeline = composeSurface(definition.timeline, main, definition.timelineOpacity);
    tokens.surfaceViewer = composeSurface(definition.background, rootBackdrop, definition.viewerOpacity);
    tokens.surfaceDialog = tokens.surfacePanel;
    tokens.surfaceCodex = tokens.surfacePanel;
    tokens.textPrimary = ensureOpaque(definition.text, defaults.text);
    tokens.textSecondary = blend(tokens.textPrimary, tokens.surfacePanel, 72);
    tokens.textDisabled = blend(tokens.textPrimary, tokens.surfacePanel, 42);
    tokens.accentNormal = ensureOpaque(definition.accent, defaults.accent);
    tokens.accentHover = blend(tokens.accentNormal, tokens.textPrimary, 86);
    tokens.accentPressed = blend(tokens.accentNormal, QColor(Qt::black), 100);
    tokens.textOnAccent = contrastingText(tokens.accentPressed);
    tokens.borderNormal = ensureOpaque(definition.border, defaults.border);
    tokens.borderFocus = blend(tokens.accentNormal, tokens.borderNormal, 100);
    tokens.selection = blend(tokens.accentNormal, tokens.surfacePanel, definition.mode == ThemeMode::HighContrast ? 100 : 28);
    tokens.selectionText = definition.mode == ThemeMode::HighContrast
        ? QColor(Qt::black)
        : tokens.textPrimary;
    tokens.tooltipBackground = composeSurface(tokens.surfacePanel, main, 96);
    tokens.tooltipText = tokens.textPrimary;
    tokens.subtitleBackground = blend(QColor(Qt::black), tokens.surfaceViewer, definition.subtitleOpacity);
    tokens.sliderTrack = blend(tokens.borderNormal, tokens.surfaceTimeline, 100);
    return tokens;
}

QPalette buildThemePalette(const ThemeTokens& tokens)
{
    QPalette palette;
    palette.setColor(QPalette::Window, tokens.surfaceMain);
    palette.setColor(QPalette::WindowText, tokens.textPrimary);
    palette.setColor(QPalette::Base, tokens.surfacePanel);
    palette.setColor(QPalette::AlternateBase, tokens.surfaceTimeline);
    palette.setColor(QPalette::Text, tokens.textPrimary);
    palette.setColor(QPalette::Button, tokens.surfaceToolbar);
    palette.setColor(QPalette::ButtonText, tokens.textPrimary);
    palette.setColor(QPalette::BrightText, tokens.textOnAccent);
    palette.setColor(QPalette::Highlight, tokens.accentNormal);
    palette.setColor(QPalette::HighlightedText, tokens.textOnAccent);
    palette.setColor(QPalette::Link, tokens.accentNormal);
    palette.setColor(QPalette::ToolTipBase, tokens.tooltipBackground);
    palette.setColor(QPalette::ToolTipText, tokens.tooltipText);
    palette.setColor(QPalette::PlaceholderText, tokens.textDisabled);
    palette.setColor(QPalette::Light, tokens.surfacePanel.lighter(115));
    palette.setColor(QPalette::Midlight, tokens.surfacePanel.lighter(108));
    palette.setColor(QPalette::Mid, tokens.borderNormal);
    palette.setColor(QPalette::Dark, tokens.surfaceMain.darker(115));
    palette.setColor(QPalette::Shadow, tokens.surfaceMain.darker(140));

    // Inactive windows remain usable.  Applying disabled colors to the
    // inactive group makes an unfocused player look globally disabled.
    for (const QPalette::ColorGroup group : { QPalette::Disabled }) {
        palette.setColor(group, QPalette::Window, tokens.surfaceMain);
        palette.setColor(group, QPalette::WindowText, tokens.textDisabled);
        palette.setColor(group, QPalette::Base, tokens.surfacePanel);
        palette.setColor(group, QPalette::AlternateBase, tokens.surfaceTimeline);
        palette.setColor(group, QPalette::Text, tokens.textDisabled);
        palette.setColor(group, QPalette::Button, tokens.surfaceToolbar);
        palette.setColor(group, QPalette::ButtonText, tokens.textDisabled);
        palette.setColor(group, QPalette::BrightText, tokens.textDisabled);
        palette.setColor(group, QPalette::Highlight, tokens.accentNormal.darker(130));
        palette.setColor(group, QPalette::HighlightedText, tokens.textDisabled);
        palette.setColor(group, QPalette::Link, tokens.textDisabled);
        palette.setColor(group, QPalette::ToolTipBase, tokens.tooltipBackground);
        palette.setColor(group, QPalette::ToolTipText, tokens.textDisabled);
        palette.setColor(group, QPalette::PlaceholderText, tokens.textDisabled);
    }
    const QPalette::ColorGroup inactive = QPalette::Inactive;
    palette.setColor(inactive, QPalette::Window, tokens.surfaceMain);
    palette.setColor(inactive, QPalette::WindowText, tokens.textPrimary);
    palette.setColor(inactive, QPalette::Base, tokens.surfacePanel);
    palette.setColor(inactive, QPalette::AlternateBase, tokens.surfaceTimeline);
    palette.setColor(inactive, QPalette::Text, tokens.textPrimary);
    palette.setColor(inactive, QPalette::Button, tokens.surfaceToolbar);
    palette.setColor(inactive, QPalette::ButtonText, tokens.textPrimary);
    palette.setColor(inactive, QPalette::BrightText, tokens.textOnAccent);
    palette.setColor(inactive, QPalette::Highlight, tokens.accentNormal);
    palette.setColor(inactive, QPalette::HighlightedText, tokens.textOnAccent);
    palette.setColor(inactive, QPalette::Link, tokens.accentNormal);
    palette.setColor(inactive, QPalette::ToolTipBase, tokens.tooltipBackground);
    palette.setColor(inactive, QPalette::ToolTipText, tokens.tooltipText);
    palette.setColor(inactive, QPalette::PlaceholderText, tokens.textSecondary);
    return palette;
}

QJsonObject ThemeDefinition::toJson() const
{
    ThemeDefinition copy = *this; copy.normalize();
    return {{QStringLiteral("version"), 1}, {QStringLiteral("name"), copy.name}, {QStringLiteral("mode"), themeModeName(copy.mode)}, {QStringLiteral("backgroundType"), backgroundTypeName(copy.backgroundType)}, {QStringLiteral("background"), color(copy.background)}, {QStringLiteral("backgroundSecondary"), color(copy.backgroundSecondary)}, {QStringLiteral("panel"), color(copy.panel)}, {QStringLiteral("toolbar"), color(copy.toolbar)}, {QStringLiteral("timeline"), color(copy.timeline)}, {QStringLiteral("text"), color(copy.text)}, {QStringLiteral("border"), color(copy.border)}, {QStringLiteral("accent"), color(copy.accent)}, {QStringLiteral("backgroundImage"), copy.backgroundImage}, {QStringLiteral("texturePath"), copy.texturePath}, {QStringLiteral("fillMode"), copy.fillMode}, {QStringLiteral("backgroundOpacity"), copy.backgroundOpacity}, {QStringLiteral("panelOpacity"), copy.panelOpacity}, {QStringLiteral("toolbarOpacity"), copy.toolbarOpacity}, {QStringLiteral("timelineOpacity"), copy.timelineOpacity}, {QStringLiteral("subtitleOpacity"), copy.subtitleOpacity}, {QStringLiteral("viewerOpacity"), copy.viewerOpacity}, {QStringLiteral("shadowStrength"), copy.shadowStrength}, {QStringLiteral("blurRadius"), copy.blurRadius}, {QStringLiteral("vignette"), copy.vignette}, {QStringLiteral("brightness"), copy.brightness}, {QStringLiteral("saturation"), copy.saturation}, {QStringLiteral("dynamicBackground"), copy.dynamicBackground}};
}

ThemeDefinition ThemeDefinition::fromJson(const QJsonObject& object, bool* valid)
{
    ThemeDefinition result;
    bool ok = !object.isEmpty();
    if (object.contains(QStringLiteral("name"))) result.name = object.value(QStringLiteral("name")).toString(result.name);
    result.mode = themeModeFromName(object.value(QStringLiteral("mode")).toString());
    result.backgroundType = backgroundTypeFromName(object.value(QStringLiteral("backgroundType")).toString());
    auto readColor = [&](const char* key, QColor& target) { const QJsonValue value = object.value(QLatin1String(key)); if (value.isString()) { QColor c(value.toString()); if (c.isValid()) target = c; else ok = false; } };
    readColor("background", result.background); readColor("backgroundSecondary", result.backgroundSecondary); readColor("panel", result.panel); readColor("toolbar", result.toolbar); readColor("timeline", result.timeline); readColor("text", result.text); readColor("border", result.border); readColor("accent", result.accent);
    result.backgroundImage = object.value(QStringLiteral("backgroundImage")).toString(); result.texturePath = object.value(QStringLiteral("texturePath")).toString(); result.fillMode = object.value(QStringLiteral("fillMode")).toString(result.fillMode);
    auto integer = [&](const char* key, int& target) { const QJsonValue value = object.value(QLatin1String(key)); if (value.isDouble()) target = value.toInt(); };
    integer("backgroundOpacity", result.backgroundOpacity); integer("panelOpacity", result.panelOpacity); integer("toolbarOpacity", result.toolbarOpacity); integer("timelineOpacity", result.timelineOpacity); integer("subtitleOpacity", result.subtitleOpacity); integer("viewerOpacity", result.viewerOpacity); integer("shadowStrength", result.shadowStrength); integer("blurRadius", result.blurRadius); integer("vignette", result.vignette); integer("brightness", result.brightness); integer("saturation", result.saturation);
    result.dynamicBackground = object.value(QStringLiteral("dynamicBackground")).toBool(false); result.normalize(); if (valid) *valid = ok; return result;
}

QString ThemeService::themesDirectory() { return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/themes"); }
QString ThemeService::sanitizeName(const QString& name) { QString safe = name.trimmed(); safe.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("_")); return safe.isEmpty() ? QStringLiteral("自定义主题") : safe; }
QString ThemeService::themePath(const QString& name) { return themesDirectory() + QStringLiteral("/") + sanitizeName(name) + QStringLiteral(".json"); }

bool ThemeService::save(const ThemeDefinition& theme, QString* error)
{
    QDir().mkpath(themesDirectory()); const QString path = themePath(theme.name); const QString backup = path + QStringLiteral(".bak");
    if (QFile::exists(path)) { QFile::remove(backup); QFile::copy(path, backup); }
    QSaveFile file(path); if (!file.open(QIODevice::WriteOnly)) { if (error) *error = file.errorString(); return false; } const QByteArray data = QJsonDocument(theme.toJson()).toJson(QJsonDocument::Indented); if (file.write(data) != data.size() || !file.commit()) { if (error) *error = file.errorString(); return false; } return true;
}

ThemeDefinition ThemeService::load(const QString& name, bool* recovered, QString* error)
{
    if (recovered) *recovered = false;
    const QString path = themePath(name); const QString backup = path + QStringLiteral(".bak");
    auto read = [](const QString& candidate, bool* valid, QString* parseErrorText) { QFile file(candidate); if (!file.open(QIODevice::ReadOnly)) return ThemeDefinition{}; QJsonParseError parseError; const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError); ThemeDefinition result = doc.isObject() ? ThemeDefinition::fromJson(doc.object(), valid) : ThemeDefinition{}; if (parseErrorText && parseError.error != QJsonParseError::NoError) *parseErrorText = parseError.errorString(); return result; };
    bool valid = false; QString parseErrorText; ThemeDefinition result = read(path, &valid, &parseErrorText); if (valid) return result;
    bool backupValid = false; ThemeDefinition backupTheme = read(backup, &backupValid, nullptr); if (backupValid) { if (recovered) *recovered = true; if (error) *error = QStringLiteral("主题文件损坏，已恢复最近一次有效版本"); return backupTheme; }
    if (recovered) *recovered = true; if (error) *error = parseErrorText.isEmpty() ? QStringLiteral("主题文件不可用") : parseErrorText; return ThemeDefinition{};
}

QStringList ThemeService::names() { QDir dir(themesDirectory()); QStringList result; for (const QFileInfo& info : dir.entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) result.append(info.completeBaseName()); return result; }
bool ThemeService::remove(const QString& name, QString* error) { const QString path = themePath(name); if (QFile::remove(path)) return true; if (error) *error = QStringLiteral("无法删除主题文件"); return false; }

} // namespace cgplay
