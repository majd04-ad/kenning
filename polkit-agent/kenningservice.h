/*
    SPDX-FileCopyrightText: 2026 Majd Abu Diab
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include <QObject>
#include <QStringList>

#include <atomic>
#include <memory>

// Kenning's service, asked from a thread so the dialog never waits for the reader or the camera.
class KenningService : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
    ~KenningService() override;

    // Finds which of "fingerprint" and "face" the user has set up for approving here, and which
    // of those can be used right now: methodsFound().
    void findMethods(const QString &user);
    // Asks for the user's finger or face: message() while waiting, then grantFinished().
    void requestGrant(const QString &user, const QString &method);
    // Stops a grant request; its grantFinished() never comes.
    void cancel();

Q_SIGNALS:
    void methodsFound(const QString &user, const QStringList &methods, const QStringList &ready);
    void message(const QString &text, bool error);
    // code is the service's ExitCode, -1 if it couldn't be reached; token is set when code is 0.
    void grantFinished(const QString &method, int code, const QString &token, const QString &error);

private:
    std::shared_ptr<std::atomic<bool>> m_cancelled;
};
