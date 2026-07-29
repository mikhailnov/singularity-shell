#pragma once

#include <QStandardPaths>
#include <QString>

/// Single settings file for the whole application:
/// ~/.local/share/singularity-shell/settings.conf
inline QString settingsPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
           + QStringLiteral("/settings.conf");
}
