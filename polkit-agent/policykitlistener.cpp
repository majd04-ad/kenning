/*  This file is part of the KDE project
    SPDX-FileCopyrightText: 2009 Jaroslav Reznik <jreznik@redhat.com>
    SPDX-FileCopyrightText: 2023 Kai Uwe Broulik <kde@broulik.de>
    SPDX-FileCopyrightText: 2026 Majd Abu Diab

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <QDBusConnection>
#include <QDebug>
#include <QGuiApplication>
#include <QQmlEngine>

#include <KConfig>
#include <KConfigGroup>
#include <KLocalizedString>
#include <KSharedConfig>
#include <KUser>
#include <KWindowSystem>

#include <PolkitQt1/ActionDescription>
#include <PolkitQt1/Agent/Session>
#include <PolkitQt1/Details>
#include <PolkitQt1/Identity>
#include <PolkitQt1/Subject>

#include "IdentitiesModel.h"
#include "QuickAuthDialog.h"
#include "config.h"
#include "kenningservice.h"
#include "policykitlistener.h"
#include "polkit1authagentadaptor.h"
#include "service.h"

namespace
{
const QString password = QStringLiteral("password");
const QString fingerprint = QStringLiteral("fingerprint");
const QString face = QStringLiteral("face");

// FaceScanIndicator's states
enum ScanState {
    Waiting = 1,
    Recognized = 2,
    Failed = 3,
};

KConfigGroup kenningState()
{
    return KSharedConfig::openStateConfig(QStringLiteral("kenningpolkitstaterc"))->group(QStringLiteral("Dialog"));
}

// Why a finger or face didn't approve, when the service didn't say.
QString failureText(int code)
{
    switch (code) {
    case FaceUnlock::Timeout:
        return i18n("Couldn't recognize you");
    case FaceUnlock::TooDark:
        return i18n("Too dark to see you");
    case FaceUnlock::TooBright:
        return i18n("Too bright to see you");
    case FaceUnlock::LockedOut:
        return i18n("Too many attempts. Use your password.");
    case FaceUnlock::NeedsPassword:
        return i18n("Use your password once after a restart");
    case FaceUnlock::NotRecognized:
        return i18n("Fingerprint not recognized");
    case FaceUnlock::NoCamera:
        return i18n("The camera is off or in use");
    case FaceUnlock::NoReader:
        return i18n("No fingerprint reader found");
    case -1:
        return i18n("Kenning's service isn't reachable");
    default:
        return i18n("Use your password");
    }
}
}

PolicyKitListener::PolicyKitListener(QObject *parent)
    : Listener(parent)
    , m_inProgress(false)
    , m_gainedAuthorization(false)
    , m_wasCancelled(false)
    , m_numTries(0)
    , m_result(nullptr)
    , m_selectedUser(nullptr)
    , m_kenning(new KenningService(this))
    , m_method(password)
    , m_retryTimer(new QTimer(this))
    , m_successTimer(new QTimer(this))
    , m_failureTimer(new QTimer(this))
{
    m_retryTimer->setSingleShot(true);
    m_retryTimer->setInterval(900);
    connect(m_retryTimer, &QTimer::timeout, this, [this] {
        if (!m_dialog.isNull() && m_method != password) {
            m_dialog->setMethodState(Waiting);
        }
    });
    // As the lock screen's pause before unlocking
    m_successTimer->setSingleShot(true);
    m_successTimer->setInterval(500);
    connect(m_successTimer, &QTimer::timeout, this, [this] {
        if (!m_dialog.isNull() && m_inProgress && !m_token.isEmpty()) {
            tryAgain();
        }
    });
    m_failureTimer->setSingleShot(true);
    m_failureTimer->setInterval(1500);
    connect(m_failureTimer, &QTimer::timeout, this, [this] {
        if (m_dialog.isNull() || m_method == password) {
            return;
        }
        selectMethod(password);
        m_dialog->showError(m_failure);
    });

    (void)new Polkit1AuthAgentAdaptor(this);

    if (!QDBusConnection::sessionBus().registerObject("/org/kde/Polkit1AuthAgent",
                                                      this,
                                                      QDBusConnection::ExportScriptableSlots | QDBusConnection::ExportScriptableProperties
                                                          | QDBusConnection::ExportAdaptors)) {
        qWarning() << "Could not initiate DBus helper!";
    }

    connect(m_kenning, &KenningService::methodsFound, this, &PolicyKitListener::methodsFound);
    connect(m_kenning, &KenningService::message, this, &PolicyKitListener::kenningMessage);
    connect(m_kenning, &KenningService::grantFinished, this, &PolicyKitListener::grantFinished);

    qDebug() << "Listener online";
}

PolicyKitListener::~PolicyKitListener()
{
}

void PolicyKitListener::setWIdForAction(const QString &action, qulonglong wID)
{
    // For compatibility.
    setWindowHandleForAction(action, QString::number(wID));
}

void PolicyKitListener::setWindowHandleForAction(const QString &action, const QString &handle)
{
    m_windowHandles[action] = handle;

    handleParentWindow(action, handle);
}

void PolicyKitListener::setActivationTokenForAction(const QString &action, const QString &token)
{
    m_activationTokens[action] = token;
    handleWaylandActivation(action, token);
}

void PolicyKitListener::initiateAuthentication(const QString &actionId,
                                               const QString &message,
                                               const QString &iconName,
                                               const PolkitQt1::Details &details,
                                               const QString &cookie,
                                               const PolkitQt1::Identity::List &identities,
                                               PolkitQt1::Agent::AsyncResult *result)
{
    // The auth action might set any random old icon; all we really want here is a nice
    // generic "lock" icon, so we hardcode it and ignore the icon from the auth action.
    Q_UNUSED(iconName);

    qDebug() << "Initiating authentication";

    if (m_inProgress) {
        result->setError(i18n("Another client is already authenticating, please try again later."));
        result->setCompleted();
        qDebug() << "Another client is already authenticating, please try again later.";
        return;
    }

    if (identities.isEmpty()) {
        result->setError(i18nc("Error response when polkit calls us with an empty list of identities",
                               "No user to authenticate as. Please check your system configuration."));
        result->setCompleted();
        qWarning() << "No user to authenticate as. Please check your system configuration.";
        return;
    }

    m_identities = identities;
    m_cookie = cookie;
    m_result = result;
    m_session.clear();
    // Only completed() sets it; a cancel while the finger or face is asked for has no session to
    // complete, and must not count the previous request's success (LastMethod).
    m_gainedAuthorization = false;

    m_inProgress = true;

    const QString parentHandle = m_windowHandles.value(actionId);
    const QString activationToken = m_activationTokens.value(actionId);

    m_dialog = new QuickAuthDialog(actionId, message, details, identities);
    if (!m_dialog->windowHandle()) {
        // Its QML didn't load (a broken install): refuse this request rather than crash the agent.
        qWarning() << "Kenning's polkit dialog could not be shown";
        m_dialog->deleteLater();
        m_inProgress = false;
        result->setError(i18n("The authentication dialog could not be shown."));
        result->setCompleted();
        return;
    }

    if (!parentHandle.isEmpty()) {
        handleParentWindow(actionId, parentHandle);
    }

    connect(m_dialog.data(), SIGNAL(okClicked()), SLOT(dialogAccepted()));
    connect(m_dialog.data(), SIGNAL(rejected()), SLOT(dialogCanceled()));
    connect(m_dialog.data(), SIGNAL(userSelected()), SLOT(userSelected()));
    connect(m_dialog.data(), &QuickAuthDialog::methodSelected, this, &PolicyKitListener::methodSelected);

    m_dialog->show();

    if (!activationToken.isEmpty()) {
        handleWaylandActivation(actionId, activationToken);
    }

    if (identities.length() == 1) {
        m_selectedUser = identities[0];
    } else {
        m_selectedUser = m_dialog.data()->adminUserSelected();
    }

    m_method = password;
    m_token.clear();
    m_dialog->setMethods({password});
    m_dialog->setMethod(password);
    findMethods();

    m_numTries = 0;
    tryAgain();
}

void PolicyKitListener::handleParentWindow(const QString &action, const QString &handle)
{
    if (!m_dialog || m_dialog->actionId() != action) {
        return;
    }

    KWindowSystem::setMainWindow(m_dialog->windowHandle(), handle);
}

void PolicyKitListener::handleWaylandActivation(const QString &action, const QString &token)
{
    if (!m_dialog || m_dialog->actionId() != action) {
        return;
    }

    qputenv("XDG_ACTIVATION_TOKEN", token.toUtf8());
    m_dialog->windowHandle()->requestActivate();
}

void PolicyKitListener::tryAgain()
{
    qDebug() << "Trying again";
    m_wasCancelled = false;

    // We will create new session only when some user is selected
    if (m_selectedUser.isValid()) {
        m_session = new Session(m_selectedUser, m_cookie, m_result);
        // clang-format off
        connect(m_session.data(), SIGNAL(request(QString,bool)), this, SLOT(request(QString,bool)));
        connect(m_session.data(), SIGNAL(completed(bool)), this, SLOT(completed(bool)));
        connect(m_session.data(), SIGNAL(showError(QString)), this, SLOT(showError(QString)));
        connect(m_session.data(), SIGNAL(showInfo(QString)), this, SLOT(showInfo(QString)));
        // clang-format on

        m_session.data()->initiate();
    }
}

void PolicyKitListener::finishObtainPrivilege()
{
    qDebug() << "Finishing obtaining privileges";

    // Number of tries increase only when some user is selected
    if (m_selectedUser.isValid()) {
        m_numTries++;
    }

    if (m_gainedAuthorization) {
        kenningState().writeEntry("LastMethod", m_method);
    }
    if (!m_gainedAuthorization && !m_wasCancelled && !m_dialog.isNull()) {
        m_dialog.data()->authenticationFailure();
        // A finger or face that didn't get through PAM: try again with the password, not a new scan.
        m_token.clear();
        if (m_method != password) {
            m_method = password;
            m_dialog->setMethod(password);
        }

        if (m_numTries < 3) {
            m_session.data()->deleteLater();

            tryAgain();
            return;
        }
    }

    if (!m_session.isNull()) {
        m_session.data()->result()->setCompleted();
        m_session.data()->deleteLater();
    } else {
        m_result->setCompleted();
    }

    if (!m_dialog.isNull()) {
        m_dialog.data()->hide();
        m_dialog.data()->deleteLater();
    }

    m_kenning->cancel();
    stopAnimationTimers();
    m_token.clear();
    m_inProgress = false;

    qDebug() << "Finish obtain authorization:" << m_gainedAuthorization;
}

bool PolicyKitListener::initiateAuthenticationFinish()
{
    qDebug() << "Finishing authentication";
    return true;
}

void PolicyKitListener::cancelAuthentication()
{
    qDebug() << "Cancelling authentication";

    m_wasCancelled = true;
    finishObtainPrivilege();
}

void PolicyKitListener::request(const QString &request, bool echo)
{
    qDebug() << "Request: " << request << " echo: " << echo;
    // pam_kenning.so grant asks for the token a finger or face got; never shown.
    if (request == QLatin1String(FaceUnlock::grantPrompt)) {
        if (!m_session.isNull()) {
            m_session->setResponse(m_token);
        }
        m_token.clear();
        return;
    }
    // PAM wants a password after all, e.g. a polkit-1 without Kenning's line.
    if (m_method != password && !m_dialog.isNull()) {
        m_kenning->cancel();
        m_method = password;
        m_dialog->setMethod(password);
    }
    if (!m_dialog.isNull()) {
        m_dialog.data()->request(request, echo);
    }
}

void PolicyKitListener::completed(bool gainedAuthorization)
{
    qDebug() << "Completed: " << gainedAuthorization;

    m_gainedAuthorization = gainedAuthorization;

    finishObtainPrivilege();
}

void PolicyKitListener::showError(const QString &text)
{
    qDebug() << "Error: " << text;
    if (!m_dialog.isNull()) {
        m_dialog.data()->showError(text);
    }
}

void PolicyKitListener::showInfo(const QString &text)
{
    qDebug() << "Info: " << text;
    if (!m_dialog.isNull()) {
        m_dialog.data()->showInfo(text);
    }
}

void PolicyKitListener::dialogAccepted()
{
    qDebug() << "Dialog accepted";

    if (!m_session.isNull() && !m_dialog.isNull()) {
        m_session.data()->setResponse(m_dialog.data()->password());
    }
}

void PolicyKitListener::dialogCanceled()
{
    qDebug() << "Dialog cancelled";

    m_wasCancelled = true;
    m_kenning->cancel();
    if (!m_session.isNull()) {
        m_session.data()->cancel();
    }

    finishObtainPrivilege();
}

void PolicyKitListener::userSelected()
{
    m_selectedUser = m_dialog.data()->adminUserSelected();
    // If some user is selected we must destroy existing session
    dropSession();
    m_kenning->cancel();
    stopAnimationTimers();
    m_method = password;
    m_token.clear();
    m_dialog->setMethods({password});
    m_dialog->setMethod(password);
    findMethods();
    tryAgain();
}

QString PolicyKitListener::kenningUser() const
{
    // A finger or face only answers for the user at this computer.
    const QString login = KUser().loginName();
    return m_selectedUser.isValid() && m_selectedUser.toString() == QLatin1String("unix-user:") + login ? login : QString();
}

void PolicyKitListener::findMethods()
{
    if (const QString user = kenningUser(); !user.isEmpty()) {
        m_kenning->findMethods(user);
    }
}

void PolicyKitListener::methodsFound(const QString &user, const QStringList &methods, const QStringList &ready)
{
    if (m_dialog.isNull() || user != kenningUser() || methods.isEmpty()) {
        return;
    }
    m_dialog->setMethods(QStringList{password} + methods);
    // Start with the method chosen in System Settings > Face & Fingerprint (face, fingerprint or the one
    // that approved last time), else the one used last, unless the user has begun typing.
    const QString last = kenningState().readEntry("LastMethod", password);
    const QString start = KConfig(QStringLiteral("kenningrc")).group(QStringLiteral("Methods")).readEntry("Start", face);
    QString first = start != QLatin1String("last") && methods.contains(start) ? start : last;
    // With the camera switched off, or after too many attempts: the other one, if there is one.
    // The button stays, and says why when chosen.
    if (methods.contains(first) && !ready.contains(first)) {
        first = ready.value(0);
    }
    if (ready.contains(first) && m_method == password && m_dialog->password().isEmpty()) {
        selectMethod(first);
    }
}

void PolicyKitListener::methodSelected(const QString &method)
{
    if (method != m_method) {
        selectMethod(method);
    }
}

void PolicyKitListener::dropSession()
{
    if (!m_session.isNull()) {
        m_session->disconnect(this);
        m_session->cancel();
        m_session->deleteLater();
        m_session.clear();
    }
}

void PolicyKitListener::selectMethod(const QString &method)
{
    if (m_dialog.isNull()) {
        return;
    }
    m_kenning->cancel();
    stopAnimationTimers();
    m_token.clear();
    m_method = method;
    m_dialog->setMethod(method);
    m_dialog->setMethodStatus(QString());
    m_dialog->setMethodState(Waiting);
    dropSession();
    if (method == password || kenningUser().isEmpty()) {
        m_method = password;
        m_dialog->setMethod(password);
        tryAgain();
        return;
    }
    m_dialog->setMethodStatus(method == fingerprint ? i18n("Place your finger on the fingerprint reader") : i18n("Look at the camera…"));
    m_kenning->requestGrant(kenningUser(), method);
}

void PolicyKitListener::kenningMessage(const QString &text, bool error)
{
    if (m_dialog.isNull() || error || m_method == password || m_failureTimer->isActive() || m_successTimer->isActive()) {
        return;
    }
    m_dialog->setMethodStatus(text);
    // "…, try again": a touch to repeat flashes the failure look
    if (text.contains(QLatin1String("again"))) {
        m_dialog->setMethodState(Failed);
        m_retryTimer->start();
    } else {
        m_retryTimer->stop();
        m_dialog->setMethodState(Waiting);
    }
}

void PolicyKitListener::grantFinished(const QString &method, int code, const QString &token, const QString &error)
{
    if (m_dialog.isNull() || method != m_method) {
        return;
    }
    m_retryTimer->stop();
    if (code == FaceUnlock::Recognized && !token.isEmpty()) {
        // Through PAM with the token once the animation has shown it: pam_kenning.so grant asks for
        // it in the new conversation.
        m_token = token;
        m_dialog->setMethodStatus(method == fingerprint ? i18n("Fingerprint recognized") : i18n("Recognized"));
        m_dialog->setMethodState(Recognized);
        m_successTimer->start();
        return;
    }
    m_failure = error.isEmpty() ? failureText(code) : error;
    m_dialog->setMethodStatus(m_failure);
    m_dialog->setMethodState(Failed);
    m_failureTimer->start();
}

void PolicyKitListener::stopAnimationTimers()
{
    m_retryTimer->stop();
    m_successTimer->stop();
    m_failureTimer->stop();
}

#include "moc_policykitlistener.cpp"
