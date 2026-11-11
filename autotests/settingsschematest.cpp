/*
    SPDX-FileCopyrightText: 2026 Rishon Jonathan R
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "settingsschema.h"
#include "lockscreenstate.h"

#include <QTest>

using namespace FaceUnlock;

class SettingsSchemaTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void validatesEngineSettings_data()
    {
        QTest::addColumn<QString>("id");
        QTest::addColumn<QString>("value");
        QTest::addColumn<bool>("accepted");

        QTest::newRow("bool") << QStringLiteral("core/disabled") << QStringLiteral("true") << true;
        QTest::newRow("bool, not a bool") << QStringLiteral("core/disabled") << QStringLiteral("yes") << false;
        QTest::newRow("certainty") << QStringLiteral("video/certainty") << QStringLiteral("6.4") << true;
        QTest::newRow("certainty too lenient") << QStringLiteral("video/certainty") << QStringLiteral("11") << false;
        QTest::newRow("timeout, not a number") << QStringLiteral("video/timeout") << QStringLiteral("4s") << false;
        QTest::newRow("dark threshold") << QStringLiteral("video/dark_threshold") << QStringLiteral("100") << true;
        QTest::newRow("fingerprint for administrator prompts") << QStringLiteral("polkit/fingerprint") << QStringLiteral("true") << true;
        QTest::newRow("seconds to wait for a finger") << QStringLiteral("fingerprint/timeout") << QStringLiteral("30") << true;
        QTest::newRow("seconds to wait for a finger, too short") << QStringLiteral("fingerprint/timeout") << QStringLiteral("2") << false;
        QTest::newRow("fingerprint tries") << QStringLiteral("fingerprint/max_tries") << QStringLiteral("10") << true;
        QTest::newRow("fingerprint tries, none") << QStringLiteral("fingerprint/max_tries") << QStringLiteral("0") << false;
        QTest::newRow("face for administrator prompts, not a bool") << QStringLiteral("polkit/face") << QStringLiteral("1") << false;
    }

    void validatesEngineSettings()
    {
        QFETCH(QString, id);
        QFETCH(QString, value);
        QFETCH(bool, accepted);
        const std::optional<Setting> setting = engineSetting(id);
        QVERIFY(setting);
        QCOMPARE(setting->accepts(value), accepted);
    }

    void splitsGroupAndKey()
    {
        const std::optional<Setting> setting = systemSetting(QStringLiteral("LoginScreen/WelcomePause"));
        QVERIFY(setting);
        QCOMPARE(setting->group, QStringLiteral("LoginScreen"));
        QCOMPARE(setting->key, QStringLiteral("WelcomePause"));
        QVERIFY(setting->accepts(QStringLiteral("3000")));
        QVERIFY(!setting->accepts(QStringLiteral("3001")));
    }

    void rejectsUnknownSettings()
    {
        QVERIFY(!engineSetting(QStringLiteral("video/device_path")));
        QVERIFY(!systemSetting(QStringLiteral("core/disabled")));
        QVERIFY(!engineSetting(QStringLiteral("fingerprint/device")));
        QVERIFY(systemSetting(QStringLiteral("Polkit/Agent")));
    }

    void checksThemeIds_data()
    {
        QTest::addColumn<QString>("id");
        QTest::addColumn<bool>("valid");
        QTest::newRow("plain") << QStringLiteral("hello") << true;
        QTest::newRow("dotted") << QStringLiteral("org.example.ring-2") << true;
        QTest::newRow("hidden") << QStringLiteral(".hello") << false;
        QTest::newRow("path") << QStringLiteral("../etc") << false;
        QTest::newRow("slash") << QStringLiteral("a/b") << false;
        QTest::newRow("empty") << QString() << false;
        QTest::newRow("too long") << QString(65, QLatin1Char('a')) << false;
    }

    void checksThemeIds()
    {
        QFETCH(QString, id);
        QFETCH(bool, valid);
        QCOMPARE(isThemeId(id), valid);
    }

    void recognizesPamLines()
    {
        QVERIFY(isPamLine(pamLine));
        QVERIFY(!isPamLine(QStringLiteral("# auth sufficient pam_kenning.so")));
        QVERIFY(!isPamLine(QStringLiteral("auth sufficient pam_howdy.so")));
        QVERIFY(isFacePamLine(QStringLiteral("auth sufficient pam_howdy.so")));
        QVERIFY(!isFacePamLine(QStringLiteral("auth include system-login")));
        // A finger for sudo and the polkit dialog's token aren't face unlock, and stay when it is switched off.
        const QString finger = QStringLiteral("auth       sufficient                  pam_kenning.so fingerprint");
        QVERIFY(!isPamLine(finger) && !isFacePamLine(finger) && isFingerprintPamLine(finger));
        QVERIFY(isFingerprintPamLine(QStringLiteral("auth [success=done default=die] pam_kenning.so fingerprint explicit")));
        QVERIFY(!isFingerprintPamLine(QStringLiteral("# auth sufficient pam_kenning.so fingerprint")));
        QVERIFY(!isFingerprintPamLine(pamLine));
        const QString grant = QStringLiteral("auth sufficient pam_kenning.so grant");
        QVERIFY(!isPamLine(grant) && !isFacePamLine(grant) && !isFingerprintPamLine(grant));
    }

    void parsesLockScreenStatus()
    {
        const LockScreenState face = parseLockScreenStatus(QStringLiteral("face on\n"));
        QVERIFY(face.known && face.faceService && face.enabled);
        const LockScreenState password = parseLockScreenStatus(QStringLiteral("password off"));
        QVERIFY(password.known && !password.faceService && !password.enabled);
        QVERIFY(!parseLockScreenStatus(QStringLiteral("unknown")).known);
    }
};

QTEST_GUILESS_MAIN(SettingsSchemaTest)
#include "settingsschematest.moc"
