#pragma once

#include "ThemeTypes.h"

#include <QStringList>

namespace cgplay {

class ThemeService
{
public:
    static QString themesDirectory();
    static QString themePath(const QString& name);
    static QString sanitizeName(const QString& name);
    static bool save(const ThemeDefinition& theme, QString* error = nullptr);
    static ThemeDefinition load(const QString& name, bool* recovered = nullptr, QString* error = nullptr);
    static QStringList names();
    static bool remove(const QString& name, QString* error = nullptr);
};

} // namespace cgplay
