/*
    SPDX-FileCopyrightText: 2026 Rishon Jonathan R
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include <KQuickConfigModule>

#include <QVariantList>
#include <QVariantMap>

// settings is one flat map that QML edits through setValue():
//   "core/…", "video/…"                  /etc/kenning/config.ini (helper)
//   "pam/<service>"                      pam_kenning.so in /etc/pam.d (helper)
//   "Appearance/…", "LoginScreen/…"      /etc/xdg/kenningrc (helper)
//   "lock/…"                             ~/.config/kscreenlockerrc (written directly)
class FaceUnlockKcm : public KQuickConfigModule
{
    Q_OBJECT
    Q_PROPERTY(QVariantMap settings READ settings NOTIFY settingsChanged)
    Q_PROPERTY(QVariantList faces READ faces NOTIFY facesChanged)
    Q_PROPERTY(QVariantList themes READ themes NOTIFY themesChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString userName READ userName CONSTANT)
    Q_PROPERTY(QString displayName READ displayName CONSTANT)
    Q_PROPERTY(QString devicePath READ devicePath NOTIFY settingsChanged)
    Q_PROPERTY(QString previewPath READ previewPath NOTIFY settingsChanged)
    // A colour webcam rather than an IR camera
    Q_PROPERTY(bool colourCamera READ colourCamera NOTIFY settingsChanged)
    // The camera face unlock was set up with is switched off or unplugged
    Q_PROPERTY(bool cameraAway READ cameraAway NOTIFY settingsChanged)
    Q_PROPERTY(bool engineInstalled READ engineInstalled CONSTANT)
    Q_PROPERTY(bool lockScreenPatched READ lockScreenPatched CONSTANT)
    Q_PROPERTY(bool loginScreenSupported READ loginScreenSupported CONSTANT)
    // fprintd has a fingerprint of this user
    Q_PROPERTY(bool fingerprintEnrolled READ fingerprintEnrolled NOTIFY settingsChanged)

public:
    FaceUnlockKcm(QObject *parent, const KPluginMetaData &data);

    QVariantMap settings() const;
    QVariantList faces() const;
    QVariantList themes() const;
    bool busy() const;
    QString userName() const;
    QString displayName() const;
    QString devicePath() const;
    QString previewPath() const;
    bool colourCamera() const;
    bool cameraAway() const;
    bool engineInstalled() const;
    bool lockScreenPatched() const;
    bool loginScreenSupported() const;
    bool fingerprintEnrolled() const;

    Q_INVOKABLE void setValue(const QString &key, const QVariant &value);
    Q_INVOKABLE void addFace(const QString &label);
    Q_INVOKABLE void removeFace(int id);
    Q_INVOKABLE void testRecognition();
    Q_INVOKABLE void refreshFaces();
    Q_INVOKABLE void refreshThemes();
    Q_INVOKABLE void installTheme(const QUrl &archive);
    Q_INVOKABLE void removeTheme(const QString &id);

public Q_SLOTS:
    void load() override;
    void save() override;
    void defaults() override;

Q_SIGNALS:
    void settingsChanged();
    void facesChanged();
    void themesChanged();
    void busyChanged();
    void progress(const QString &line);
    // exitCode is -1 if authorization failed or the helper couldn't run.
    void operationFinished(const QString &operation, int exitCode, const QString &message);
    void errorOccurred(const QString &message);

private:
    void runHelper(const QString &operation, const QVariantMap &arguments);
    void setBusy(bool busy);
    void updateState();
    static QVariantMap defaultSettings();

    QVariantMap m_settings;
    QVariantMap m_loaded;
    QVariantList m_faces;
    QVariantList m_themes;
    QString m_devicePath;
    QString m_previewPath;
    bool m_colourCamera = false;
    bool m_cameraAway = false;
    bool m_fingerprintEnrolled = false;
    bool m_lockScreenFaceService = false;
    bool m_busy = false;
};
