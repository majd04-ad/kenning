/*
    SPDX-FileCopyrightText: 2026 Rishon Jonathan R
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "settingsschema.h"

#include <QRegularExpression>

namespace FaceUnlock
{
namespace
{
using Validator = std::function<bool(const QString &)>;

Validator numberIn(double minimum, double maximum)
{
    return [minimum, maximum](const QString &value) {
        bool isNumber = false;
        const double number = value.toDouble(&isNumber);
        return isNumber && number >= minimum && number <= maximum;
    };
}

std::optional<Setting> findSetting(const QList<std::pair<QString, Validator>> &settings, const QString &id)
{
    for (const auto &[key, accepts] : settings) {
        if (key == id) {
            const qsizetype slash = key.indexOf(QLatin1Char('/'));
            return Setting{key.left(slash), key.mid(slash + 1), accepts};
        }
    }
    return std::nullopt;
}

bool isActiveLineWith(const QString &line, QLatin1StringView module)
{
    const QString trimmed = line.trimmed();
    return !trimmed.startsWith(QLatin1Char('#')) && trimmed.contains(module);
}
}

std::optional<Setting> engineSetting(const QString &id)
{
    static const QList<std::pair<QString, Validator>> settings = {
        {QStringLiteral("core/disabled"), isBoolText},
        {QStringLiteral("core/detection_notice"), isBoolText},
        {QStringLiteral("core/no_confirmation"), isBoolText},
        {QStringLiteral("core/abort_if_ssh"), isBoolText},
        {QStringLiteral("core/abort_if_lid_closed"), isBoolText},
        {QStringLiteral("core/password_after_boot"), isBoolText},
        {QStringLiteral("polkit/face"), isBoolText},
        {QStringLiteral("polkit/fingerprint"), isBoolText},
        {QStringLiteral("fingerprint/timeout"), numberIn(5, 120)},
        {QStringLiteral("fingerprint/max_tries"), numberIn(1, 10)},
        {QStringLiteral("video/certainty"), numberIn(1, 10)},
        {QStringLiteral("video/timeout"), numberIn(1, 30)},
        {QStringLiteral("video/dark_threshold"), numberIn(0, 100)},
    };
    return findSetting(settings, id);
}

std::optional<Setting> systemSetting(const QString &id)
{
    static const QList<std::pair<QString, Validator>> settings = {
        {QStringLiteral("Appearance/AnimationStyle"), isThemeId},
        {QStringLiteral("LoginScreen/AutoScan"), isBoolText},
        {QStringLiteral("LoginScreen/WelcomePause"), numberIn(0, 3000)},
        {QStringLiteral("LoginScreen/Fingerprint"), isBoolText},
        {QStringLiteral("Polkit/Agent"), isBoolText},
    };
    return findSetting(settings, id);
}

bool isBoolText(const QString &value)
{
    return value == QLatin1String("true") || value == QLatin1String("false");
}

bool isThemeId(const QString &id)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$"));
    return pattern.match(id).hasMatch();
}

// Whether pam_kenning.so is given this option on the line.
static bool hasModuleOption(const QString &line, QLatin1StringView option)
{
    const QStringList words = line.simplified().split(QLatin1Char(' '));
    const qsizetype module = words.indexOf(QLatin1String("pam_kenning.so"));
    return module >= 0 && words.mid(module + 1).contains(option);
}

bool isPamLine(const QString &line)
{
    return isActiveLineWith(line, QLatin1StringView("pam_kenning.so")) && !hasModuleOption(line, QLatin1StringView("fingerprint"))
        && !hasModuleOption(line, QLatin1StringView("grant"));
}

bool isFingerprintPamLine(const QString &line)
{
    return isActiveLineWith(line, QLatin1StringView("pam_kenning.so")) && hasModuleOption(line, QLatin1StringView("fingerprint"));
}

bool isFacePamLine(const QString &line)
{
    return isPamLine(line) || isActiveLineWith(line, QLatin1StringView("pam_howdy.so"));
}

QList<PamService> pamServices()
{
    return {
        {QStringLiteral("login"), QStringLiteral("/etc/pam.d/plasmalogin")},
        {QStringLiteral("lockscreen"), QStringLiteral("/etc/pam.d/kde")},
        {QStringLiteral("sudo"), QStringLiteral("/etc/pam.d/sudo")},
    };
}
}
