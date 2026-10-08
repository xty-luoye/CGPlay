#include "AppearanceProfile.h"

#include <QColor>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSet>

namespace cgplay {
namespace {

bool validColor(const QString& value)
{
    return value.isEmpty() || QColor(value).isValid();
}

bool inRange(int value, int minimum, int maximum)
{
    return value >= minimum && value <= maximum;
}

void setError(QString* error, const QString& value)
{
    if (error) *error = value;
}

int jsonInt(const QJsonObject& object, const char* key, int fallback)
{
    const QJsonValue value = object.value(QLatin1String(key));
    return value.isDouble() ? value.toInt() : fallback;
}

QString jsonString(const QJsonObject& object, const char* key, const QString& fallback = {})
{
    const QJsonValue value = object.value(QLatin1String(key));
    return value.isString() ? value.toString() : fallback;
}

} // namespace

QJsonObject ButtonStyleProfile::toJson() const
{
    QJsonObject result;
    const auto addString = [&result](const char* key, const QString& value) {
        if (!value.isEmpty()) result.insert(QLatin1String(key), value);
    };
    addString("buttonColor", background);
    addString("buttonHover", hover);
    addString("buttonPressed", pressed);
    addString("buttonDisabled", disabled);
    addString("buttonText", text);
    addString("buttonIcon", icon);
    addString("buttonBorder", border);
    result.insert(QStringLiteral("buttonRadius"), radius);
    result.insert(QStringLiteral("buttonIconSize"), iconSize);
    result.insert(QStringLiteral("buttonOpacity"), opacity);
    result.insert(QStringLiteral("buttonBorderOpacity"), borderOpacity);
    result.insert(QStringLiteral("buttonShadow"), shadow);
    result.insert(QStringLiteral("buttonWidth"), width);
    result.insert(QStringLiteral("buttonHeight"), height);
    result.insert(QStringLiteral("buttonShowText"), showText);
    result.insert(QStringLiteral("buttonShowIcon"), showIcon);
    return result;
}

ButtonStyleProfile ButtonStyleProfile::fromJson(const QJsonObject& object, bool* valid,
                                                QString* error)
{
    ButtonStyleProfile result;
    result.background = jsonString(object, "buttonColor");
    result.hover = jsonString(object, "buttonHover");
    result.pressed = jsonString(object, "buttonPressed");
    result.disabled = jsonString(object, "buttonDisabled");
    result.text = jsonString(object, "buttonText");
    result.icon = jsonString(object, "buttonIcon");
    result.border = jsonString(object, "buttonBorder");
    result.radius = jsonInt(object, "buttonRadius", result.radius);
    result.iconSize = jsonInt(object, "buttonIconSize", result.iconSize);
    result.opacity = jsonInt(object, "buttonOpacity", result.opacity);
    result.borderOpacity = jsonInt(object, "buttonBorderOpacity", result.borderOpacity);
    result.shadow = jsonInt(object, "buttonShadow", result.shadow);
    result.width = jsonInt(object, "buttonWidth", result.width);
    result.height = jsonInt(object, "buttonHeight", result.height);
    if (object.value(QStringLiteral("buttonShowText")).isBool())
        result.showText = object.value(QStringLiteral("buttonShowText")).toBool();
    if (object.value(QStringLiteral("buttonShowIcon")).isBool())
        result.showIcon = object.value(QStringLiteral("buttonShowIcon")).toBool();
    const bool ok = result.validate(error);
    if (valid) *valid = ok;
    return result;
}

bool ButtonStyleProfile::validate(QString* error) const
{
    for (const auto& entry : {
             qMakePair(QStringLiteral("buttonColor"), background),
             qMakePair(QStringLiteral("buttonHover"), hover),
             qMakePair(QStringLiteral("buttonPressed"), pressed),
             qMakePair(QStringLiteral("buttonDisabled"), disabled),
             qMakePair(QStringLiteral("buttonText"), text),
             qMakePair(QStringLiteral("buttonIcon"), icon),
             qMakePair(QStringLiteral("buttonBorder"), border)}) {
        if (!validColor(entry.second)) {
            setError(error, QStringLiteral("Invalid button color: %1").arg(entry.first));
            return false;
        }
    }
    if (!inRange(radius, 0, 24) || !inRange(iconSize, 8, 48) ||
        !inRange(opacity, 30, 100) || !inRange(borderOpacity, 0, 100) ||
        !inRange(shadow, 0, 100) || !inRange(width, 0, 240) ||
        !inRange(height, 0, 120)) {
        setError(error, QStringLiteral("Button style numeric value is out of range"));
        return false;
    }
    return true;
}

AppearanceProfile AppearanceProfile::defaults()
{
    return {};
}

QJsonObject AppearanceProfile::migrate(const QJsonObject& object, bool* valid, QString* error)
{
    QJsonObject result = object;
    const int version = result.value(QStringLiteral("version")).toInt(0);
    if (version < 0 || version > SchemaVersion) {
        setError(error, QStringLiteral("Unsupported appearance profile version: %1").arg(version));
        if (valid) *valid = false;
        return {};
    }

    // ThemeDefinition presets used shorter color keys. Package exports use the
    // appearance/QSettings names; accept both without changing persisted data.
    const QMap<QString, QString> aliases = {
        {QStringLiteral("background"), QStringLiteral("backgroundColor")},
        {QStringLiteral("panel"), QStringLiteral("panelColor")},
        {QStringLiteral("toolbar"), QStringLiteral("toolbarColor")},
        {QStringLiteral("timeline"), QStringLiteral("timelineColor")},
        {QStringLiteral("text"), QStringLiteral("textColor")},
        {QStringLiteral("border"), QStringLiteral("borderColor")},
        {QStringLiteral("accent"), QStringLiteral("accentColor")},
        {QStringLiteral("name"), QStringLiteral("themeName")},
    };
    for (auto it = aliases.constBegin(); it != aliases.constEnd(); ++it) {
        if (!result.contains(it.value()) && result.contains(it.key()))
            result.insert(it.value(), result.value(it.key()));
    }
    result.insert(QStringLiteral("version"), SchemaVersion);
    if (valid) *valid = true;
    return result;
}

AppearanceProfile AppearanceProfile::fromJson(const QJsonObject& object, bool* valid,
                                              QString* error)
{
    AppearanceProfile result = defaults();
    if (error) error->clear();
    bool migratedOk = false;
    const QJsonObject value = migrate(object, &migratedOk, error);
    if (!migratedOk) {
        if (valid) *valid = false;
        return result;
    }

    const auto readString = [&value](const char* key, QString& target) {
        target = jsonString(value, key, target);
    };
    const auto readInt = [&value](const char* key, int& target) {
        target = jsonInt(value, key, target);
    };
    readString("themeName", result.themeName);
    readString("mode", result.mode);
    readString("backgroundType", result.backgroundType);
    readString("backgroundColor", result.backgroundColor);
    readString("backgroundSecondary", result.backgroundSecondary);
    readString("panelColor", result.panelColor);
    readString("toolbarColor", result.toolbarColor);
    readString("timelineColor", result.timelineColor);
    readString("textColor", result.textColor);
    readString("borderColor", result.borderColor);
    readString("accentColor", result.accentColor);
    readString("backgroundImage", result.backgroundImage);
    readString("texturePath", result.texturePath);
    readString("fillMode", result.fillMode);
    readInt("backgroundOpacity", result.backgroundOpacity);
    readInt("panelOpacity", result.panelOpacity);
    readInt("toolbarOpacity", result.toolbarOpacity);
    readInt("timelineOpacity", result.timelineOpacity);
    readInt("subtitleOpacity", result.subtitleOpacity);
    readInt("viewerOpacity", result.viewerOpacity);
    readInt("windowOpacity", result.windowOpacity);
    readInt("buttonOpacity", result.buttonOpacity);
    readInt("vignette", result.vignette);
    readInt("blurRadius", result.blurRadius);
    readInt("brightness", result.brightness);
    readInt("saturation", result.saturation);
    readInt("shadowStrength", result.shadowStrength);
    QString normalizedMode = result.mode.trimmed().toLower();
    normalizedMode.remove(QLatin1Char('-')).remove(QLatin1Char('_')).remove(QLatin1Char(' '));
    if (normalizedMode == QStringLiteral("highcontrast")) result.mode = QStringLiteral("highContrast");
    else result.mode = normalizedMode;
    result.backgroundType = result.backgroundType.trimmed().toLower();
    result.fillMode = result.fillMode.trimmed().toLower();
    if (value.value(QStringLiteral("dynamicBackground")).isBool())
        result.dynamicBackground = value.value(QStringLiteral("dynamicBackground")).toBool();

    if (value.value(QStringLiteral("buttonStyles")).isObject()) {
        const QJsonObject styles = value.value(QStringLiteral("buttonStyles")).toObject();
        for (auto it = styles.constBegin(); it != styles.constEnd(); ++it) {
            if (!it.value().isObject()) {
                migratedOk = false;
                continue;
            }
            bool styleOk = false;
            ButtonStyleProfile style = ButtonStyleProfile::fromJson(it.value().toObject(), &styleOk, error);
            if (!styleOk) migratedOk = false;
            result.buttonStyles.insert(it.key(), style);
        }
    }

    QString validationError;
    const bool ok = migratedOk && result.validate(&validationError);
    if (!ok && error && error->isEmpty()) *error = validationError;
    if (valid) *valid = ok;
    return result;
}

AppearanceProfile AppearanceProfile::fromPackageJson(const QJsonObject& theme,
                                                     const QJsonObject& buttonStyles,
                                                     bool* valid, QString* error)
{
    QJsonObject combined = theme;
    combined.insert(QStringLiteral("buttonStyles"), buttonStyles);
    return fromJson(combined, valid, error);
}

QJsonObject AppearanceProfile::toJson() const
{
    return {
        {QStringLiteral("version"), SchemaVersion},
        {QStringLiteral("themeName"), themeName},
        {QStringLiteral("mode"), mode},
        {QStringLiteral("backgroundType"), backgroundType},
        {QStringLiteral("backgroundColor"), backgroundColor},
        {QStringLiteral("backgroundSecondary"), backgroundSecondary},
        {QStringLiteral("panelColor"), panelColor},
        {QStringLiteral("toolbarColor"), toolbarColor},
        {QStringLiteral("timelineColor"), timelineColor},
        {QStringLiteral("textColor"), textColor},
        {QStringLiteral("borderColor"), borderColor},
        {QStringLiteral("accentColor"), accentColor},
        {QStringLiteral("backgroundImage"), backgroundImage},
        {QStringLiteral("texturePath"), texturePath},
        {QStringLiteral("fillMode"), fillMode},
        {QStringLiteral("backgroundOpacity"), backgroundOpacity},
        {QStringLiteral("panelOpacity"), panelOpacity},
        {QStringLiteral("toolbarOpacity"), toolbarOpacity},
        {QStringLiteral("timelineOpacity"), timelineOpacity},
        {QStringLiteral("subtitleOpacity"), subtitleOpacity},
        {QStringLiteral("viewerOpacity"), viewerOpacity},
        {QStringLiteral("windowOpacity"), windowOpacity},
        {QStringLiteral("buttonOpacity"), buttonOpacity},
        {QStringLiteral("vignette"), vignette},
        {QStringLiteral("blurRadius"), blurRadius},
        {QStringLiteral("brightness"), brightness},
        {QStringLiteral("saturation"), saturation},
        {QStringLiteral("shadowStrength"), shadowStrength},
        {QStringLiteral("dynamicBackground"), dynamicBackground},
    };
}

QJsonObject AppearanceProfile::buttonStylesToJson() const
{
    QJsonObject result;
    for (auto it = buttonStyles.constBegin(); it != buttonStyles.constEnd(); ++it)
        result.insert(it.key(), it.value().toJson());
    return result;
}

bool AppearanceProfile::validate(QString* error) const
{
    static const QSet<QString> modes = {QStringLiteral("dark"), QStringLiteral("light"),
        QStringLiteral("glass"), QStringLiteral("highContrast"), QStringLiteral("custom")};
    static const QSet<QString> backgrounds = {QStringLiteral("solid"), QStringLiteral("gradient"),
        QStringLiteral("image"), QStringLiteral("texture")};
    static const QSet<QString> fills = {QStringLiteral("cover"), QStringLiteral("contain"),
        QStringLiteral("tile")};
    if (!modes.contains(mode) || !backgrounds.contains(backgroundType) || !fills.contains(fillMode)) {
        setError(error, QStringLiteral("Appearance mode, background type, or fill mode is invalid"));
        return false;
    }
    for (const QString& value : {backgroundColor, backgroundSecondary, panelColor, toolbarColor,
                                 timelineColor, textColor, borderColor, accentColor}) {
        if (!QColor(value).isValid()) {
            setError(error, QStringLiteral("Appearance profile contains an invalid color"));
            return false;
        }
    }
    if (!inRange(backgroundOpacity, 20, 100) || !inRange(panelOpacity, 35, 100) ||
        !inRange(toolbarOpacity, 35, 100) || !inRange(timelineOpacity, 45, 100) ||
        !inRange(subtitleOpacity, 20, 100) || !inRange(viewerOpacity, 20, 100) ||
        !inRange(windowOpacity, 40, 100) || !inRange(buttonOpacity, 30, 100) ||
        !inRange(vignette, 0, 100) || !inRange(blurRadius, 0, 64) ||
        !inRange(brightness, 0, 200) || !inRange(saturation, 0, 200) ||
        !inRange(shadowStrength, 0, 100)) {
        setError(error, QStringLiteral("Appearance numeric value is out of range"));
        return false;
    }
    static const QRegularExpression idPattern(QStringLiteral("^[A-Za-z0-9_.-]+$"));
    for (auto it = buttonStyles.constBegin(); it != buttonStyles.constEnd(); ++it) {
        if (!idPattern.match(it.key()).hasMatch() || !it.value().validate(error)) {
            if (error && error->isEmpty()) *error = QStringLiteral("Invalid button style id: %1").arg(it.key());
            return false;
        }
    }
    return true;
}

} // namespace cgplay
