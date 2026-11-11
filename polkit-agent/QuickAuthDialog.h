/*  This file is part of the KDE project
    SPDX-FileCopyrightText: 2021 Aleix Pol Gonzalez <aleixpol@kde.org>
    SPDX-FileCopyrightText: 2023 Devin Lin <devin@kde.org>
    SPDX-FileCopyrightText: 2026 Majd Abu Diab

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef QUICKAUTHDIALOG_H
#define QUICKAUTHDIALOG_H

#include <QObject>
#include <QWindow>

#include <PolkitQt1/Details>
#include <PolkitQt1/Identity>

class QuickAuthDialog : public QObject
{
    Q_OBJECT
public:
    QuickAuthDialog(const QString &actionId, const QString &message, const PolkitQt1::Details &details, const PolkitQt1::Identity::List &identities);

    QWindow *windowHandle() const
    {
        return m_theDialog;
    }
    PolkitQt1::Identity adminUserSelected() const;

    QString actionId() const;
    QString password() const;
    void showError(const QString &message);
    void showInfo(const QString &message);
    void authenticationFailure();
    void show();
    void hide();
    void request(const QString &request, bool echo);

    // Kenning: "password", "fingerprint" and "face", as available; the one in use; what it says.
    void setMethods(const QStringList &methods);
    void setMethod(const QString &method);
    void setMethodStatus(const QString &status);
    // The scan animation: 1 waiting, 2 recognized, 3 failed (FaceScanIndicator's states).
    void setMethodState(int state);
    int methodState() const;

Q_SIGNALS:
    void okClicked();
    void rejected();
    void userSelected();
    // The user chose another way to approve, or typed while the finger or face was asked for.
    void methodSelected(const QString &method);

private:
    QWindow *m_theDialog = nullptr;
    QString m_actionId;
};

#endif
