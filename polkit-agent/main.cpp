/*  This file is part of the KDE project
    SPDX-FileCopyrightText: 2009 Jaroslav Reznik <jreznik@redhat.com>
    SPDX-FileCopyrightText: 2026 Majd Abu Diab

    SPDX-License-Identifier: GPL-2.0-or-later
*/

// Kenning's polkit dialog: Plasma's, with Fingerprint and Face beside the password. It runs in
// place of polkit-kde-authentication-agent-1 (a drop-in for plasma-polkit-agent.service) and
// starts that one instead when Kenning's is switched off in System Settings > Face & Fingerprint.

#include "QuickAuthDialog.h"
#include "agentconfig.h"
#include "policykitlistener.h"
// KF
#include <KAboutData>
#include <KConfig>
#include <KConfigGroup>
#include <KDBusService>
#include <KLocalizedString>
#include <KUser>
// PolkitQt1
#include <PolkitQt1/Subject>
// Qt
#include <QApplication>
#include <QDebug>
#include <QFile>
#include <QQuickWindow>
#include <QSessionManager>
#include <QTimer>
// std
#include <unistd.h>
#if HAVE_SYS_PRCTL_H
#include <sys/prctl.h>
#endif
#if HAVE_SYS_PROCCTL_H
#include <sys/procctl.h>
#endif

namespace
{
bool kenningDialogWanted()
{
    const KConfig config(QStringLiteral(KENNING_SYSTEM_CONFIG), KConfig::SimpleConfig);
    return config.group(QStringLiteral("Polkit")).readEntry("Agent", true);
}

// Plasma's own agent, wherever this distribution keeps libexec programs.
void runPlasmaAgent(char *argv[])
{
    for (const char *path : {STOCK_POLKIT_AGENT,
                             "/usr/lib/polkit-kde-authentication-agent-1",
                             "/usr/libexec/polkit-kde-authentication-agent-1",
                             "/usr/lib/x86_64-linux-gnu/libexec/polkit-kde-authentication-agent-1",
                             "/usr/lib/aarch64-linux-gnu/libexec/polkit-kde-authentication-agent-1"}) {
        if (access(path, X_OK) == 0) {
            execv(path, argv);
        }
    }
    qWarning() << "Kenning's polkit dialog is switched off, but Plasma's polkit-kde-authentication-agent-1 wasn't found; using Kenning's";
}

// Loads the dialog without polkit and quits: checks that its QML works, as the tests do.
int smokeTest()
{
    const PolkitQt1::Identity::List identities{PolkitQt1::UnixUserIdentity(KUser().loginName())};
    QuickAuthDialog dialog(QStringLiteral("org.kde.kenning.smoketest"), QStringLiteral("Smoke test"), {}, identities);
    if (!dialog.windowHandle()) {
        return 1;
    }
    dialog.setMethods({QStringLiteral("password"), QStringLiteral("fingerprint"), QStringLiteral("face")});
    dialog.setMethod(QStringLiteral("fingerprint"));
    dialog.setMethodStatus(QStringLiteral("Place your finger on the fingerprint reader"));
    dialog.show();
    QTimer::singleShot(500, qApp, [&dialog] {
        // KENNING_SMOKE_SCREENSHOT=file.png saves what the dialog looks like.
        if (const QString path = qEnvironmentVariable("KENNING_SMOKE_SCREENSHOT"); !path.isEmpty()) {
            if (auto *window = qobject_cast<QQuickWindow *>(dialog.windowHandle())) {
                window->grabWindow().save(path);
            }
        }
        qApp->quit();
    });
    return qApp->exec();
}
}

int main(int argc, char *argv[])
{
    const bool smoke = argc > 1 && qstrcmp(argv[1], "--smoke-test") == 0;
    if (!smoke && !kenningDialogWanted()) {
        runPlasmaAgent(argv);
    }

    // disable ptrace
#if HAVE_PR_SET_DUMPABLE
    prctl(PR_SET_DUMPABLE, 0);
#endif
#if HAVE_PROC_TRACE_CTL
    int mode = PROC_TRACE_CTL_DISABLE;
    procctl(P_PID, getpid(), PROC_TRACE_CTL, &mode);
#endif

    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);

    // Plasma's translations for the strings the dialog shares with it
    KLocalizedString::setApplicationDomain(QByteArrayLiteral("polkit-kde-authentication-agent-1"));

    if (smoke) {
        return smokeTest();
    }

    // The same name as Plasma's agent, which plasma-polkit-agent.service waits for
    KAboutData aboutData("polkit-kde-authentication-agent-1", QString(), KENNING_VERSION);
    aboutData.addLicense(KAboutLicense::GPL);
    aboutData.addCredit(i18n("(c) 2009 Red Hat, Inc."));
    aboutData.addAuthor(i18n("Lukáš Tinkl"), i18n("Maintainer"), "ltinkl@redhat.com");
    aboutData.addAuthor(i18n("Jaroslav Reznik"), i18n("Former maintainer"), "jreznik@redhat.com");
    aboutData.setProductName("policykit-kde/polkit-kde-authentication-agent-1");

    KAboutData::setApplicationData(aboutData);

    // ensure singleton run
    KDBusService service(KDBusService::Unique | KDBusService::Replace);

    // disable session management
    auto disableSessionManagement = [](QSessionManager &sm) {
        sm.setRestartHint(QSessionManager::RestartNever);
    };

    QObject::connect(&app, &QGuiApplication::commitDataRequest, disableSessionManagement);
    QObject::connect(&app, &QGuiApplication::saveStateRequest, disableSessionManagement);

    // register agent
    PolicyKitListener *listener = new PolicyKitListener(&app);

    PolkitQt1::UnixSessionSubject session(getpid());

    const bool result = listener->registerListener(session, "/org/kde/PolicyKit1/AuthenticationAgent");

    qDebug() << "Authentication agent result:" << result;

    if (!result) {
        qWarning() << "Couldn't register listener!";
        exit(1);
    }

    app.exec();
}
