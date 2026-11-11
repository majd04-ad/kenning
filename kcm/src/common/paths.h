/*
    SPDX-FileCopyrightText: 2026 Rishon Jonathan R
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

// The engine's paths come from config.h, so a package that moves them moves them everywhere.
// The rest come from the build too: Debian and Fedora keep PAM modules outside lib/security.

#include "config.h"

#include <QString>

#ifndef KENNING_BINARY
#define KENNING_BINARY "/usr/bin/kenning"
#endif
#ifndef KENNING_PAM_MODULE
#define KENNING_PAM_MODULE "/usr/lib/security/pam_kenning.so"
#endif
#ifndef KENNING_LIBEXECDIR
#define KENNING_LIBEXECDIR "/usr/lib/kenning"
#endif

namespace FaceUnlock
{
inline const QString engineConfigPath = QString::fromUtf8(configPath);
inline const QString modelsPath = QString::fromUtf8(modelsDir);
inline const QString serviceUserName = QString::fromUtf8(serviceUser);

inline QString modelPath(const QString &user)
{
    return QString::fromStdString(modelPath(user.toStdString()));
}

inline const QString engineBinary = QStringLiteral(KENNING_BINARY);
inline const QString pamModulePath = QStringLiteral(KENNING_PAM_MODULE);
inline const QString systemConfigPath = QStringLiteral("/etc/xdg/kenningrc");
// Per user, over systemConfigPath: the method to start with ([Methods] Start)
inline const QString userConfigName = QStringLiteral("kenningrc");
inline const QString lockScreenPath = QStringLiteral("/usr/share/plasma/shells/org.kde.plasma.desktop/contents/lockscreen/LockScreenUi.qml");
inline const QString lockScreenMarker = QStringLiteral("kenning lock screen patch");
// An unpatched login manager would scan before checking a typed password.
inline const QString loginManagerSupportPath = QStringLiteral(KENNING_DATADIR "/login-manager");
inline const QString userThemesPath = QStringLiteral("/usr/local/share/kenning/themes");
inline const QString systemThemesPath = QStringLiteral(KENNING_DATADIR "/themes");
inline const QString pamTidy = QStringLiteral(KENNING_LIBEXECDIR "/pam-tidy");
}
