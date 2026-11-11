/*
    SPDX-FileCopyrightText: 2026 Rishon Jonathan R
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include <QList>
#include <QString>

#include <functional>
#include <optional>

namespace FaceUnlock
{
struct Setting {
    QString group;
    QString key;
    std::function<bool(const QString &)> accepts;
};

std::optional<Setting> engineSetting(const QString &id);
std::optional<Setting> systemSetting(const QString &id);

bool isBoolText(const QString &value);
// Theme ids double as directory names, so no slashes or leading dots.
bool isThemeId(const QString &id);

inline const QString pamLine = QStringLiteral("auth       sufficient                  pam_kenning.so");
// pam_kenning.so as face unlock: not its lines for a finger ("fingerprint") or for the polkit
// dialog's token ("grant").
bool isPamLine(const QString &line);
// pam_kenning.so asking for a finger, as pam-tidy sudo-fingerprint writes it.
bool isFingerprintPamLine(const QString &line);
// pam_kenning or a leftover pam_howdy; both are removed so only one engine runs.
bool isFacePamLine(const QString &line);

struct PamService {
    QString id; // as in the settings page's "pam/<id>"
    QString path;
};
// Face unlock in front of the password. Administrator prompts (polkit-1), the lock screen's
// Fingerprint method and a finger for sudo are pam-tidy's: "polkit", "fingerprint" and
// "sudo-fingerprint".
QList<PamService> pamServices();
}
