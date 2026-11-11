/*  This file is part of the KDE project
    SPDX-FileCopyrightText: 2021 Aleix Pol Gonzalez <aleixpol@kde.org>
    SPDX-FileCopyrightText: 2023 Devin Lin <devin@kde.org>
    SPDX-FileCopyrightText: 2026 Majd Abu Diab

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "QuickAuthDialog.h"
#include "IdentitiesModel.h"
#include "agentconfig.h"

#include <PolkitQt1/Authority>

#include <KConfig>
#include <KConfigGroup>
#include <KLocalizedQmlContext>
#include <KLocalizedString>
#include <KUser>

#include <PolkitQt1/ActionDescription>

#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QTimer>

QuickAuthDialog::QuickAuthDialog(const QString &actionId,
                                 const QString &message,
                                 [[maybe_unused]] const PolkitQt1::Details &details,
                                 const PolkitQt1::Identity::List &identities)
    : QObject(nullptr)
    , m_actionId(actionId)
{
    // Here rather than in the listener, so the smoke test, which has none, gets them too.
    static const bool registered = [] {
        qmlRegisterType<IdentitiesModel>("org.kde.polkitkde", 1, 0, "IdentitiesModel");
        qmlRegisterUncreatableType<PolkitQt1::ActionDescription>("org.kde.polkitkde", 1, 0, "ActionDescription", "nope!");
        return true;
    }();
    Q_UNUSED(registered);

    auto engine = new QQmlApplicationEngine(this);
    QVariantMap props = {
        {"mainText", message},
        // The face scan animation chosen in System Settings > Face & Fingerprint
        {"animationStyle",
         KConfig(QStringLiteral(KENNING_SYSTEM_CONFIG), KConfig::SimpleConfig)
             .group(QStringLiteral("Appearance"))
             .readEntry("AnimationStyle", QStringLiteral("hello"))},
    };

    const auto actions = PolkitQt1::Authority::instance()->enumerateActionsSync();
    for (const PolkitQt1::ActionDescription &desc : actions) {
        if (actionId == desc.actionId()) {
            qDebug() << "Action description has been found";
            props.insert("descriptionString", desc.description());
            props.insert("descriptionActionId", desc.actionId());
            props.insert("descriptionVendorName", desc.vendorName());
            props.insert("descriptionVendorUrl", desc.vendorUrl());
            break;
        }
    }

    engine->setInitialProperties(props);
    engine->rootContext()->setContextObject(new KLocalizedQmlContext(engine));

    // Kenning keeps the desktop dialog only.
    engine->load(QStringLiteral("qrc:/qml/QuickAuthDialog.qml"));

    if (engine->rootObjects().isEmpty()) {
        qWarning() << "The dialog's QML failed to load";
        return;
    }
    m_theDialog = qobject_cast<QQuickWindow *>(engine->rootObjects().constFirst());

    auto idents = qobject_cast<IdentitiesModel *>(m_theDialog->property("identitiesModel").value<QObject *>());
    idents->setIdentities(identities, false);
    if (!identities.isEmpty()) {
        int initialIndex = std::max(0, idents->indexForUser(KUser().loginName()));
        m_theDialog->setProperty("identitiesCurrentIndex", initialIndex);
    }

    // listen for dialog accept/reject
    connect(m_theDialog, SIGNAL(accept()), this, SIGNAL(okClicked()));
    connect(m_theDialog, SIGNAL(reject()), this, SIGNAL(rejected()));
    connect(m_theDialog, SIGNAL(userSelected()), this, SIGNAL(userSelected()));
    connect(m_theDialog, SIGNAL(methodSelected(QString)), this, SIGNAL(methodSelected(QString)));
}

enum KirigamiInlineMessageTypes {
    Information = 0,
    Positive = 1,
    Warning = 2,
    Error = 3
};

QString QuickAuthDialog::actionId() const
{
    return m_actionId;
}

QString QuickAuthDialog::password() const
{
    return m_theDialog->property("password").toString();
}

void QuickAuthDialog::showError(const QString &message)
{
    m_theDialog->setProperty("inlineMessageType", Error);
    m_theDialog->setProperty("inlineMessageText", message);
}

void QuickAuthDialog::showInfo(const QString &message)
{
    m_theDialog->setProperty("inlineMessageType", Information);
    m_theDialog->setProperty("inlineMessageText", message);
}

PolkitQt1::Identity QuickAuthDialog::adminUserSelected() const
{
    return PolkitQt1::Identity::fromString(m_theDialog->property("selectedIdentity").toString());
}

void QuickAuthDialog::authenticationFailure()
{
    QTimer::singleShot(0, m_theDialog, SLOT(authenticationFailure()));
}

void QuickAuthDialog::show()
{
    QTimer::singleShot(0, m_theDialog, SLOT(show()));
}

void QuickAuthDialog::hide()
{
    QTimer::singleShot(0, m_theDialog, SLOT(hide()));
}

void QuickAuthDialog::request([[maybe_unused]] const QString &request, [[maybe_unused]] bool echo)
{
    QTimer::singleShot(0, m_theDialog, SLOT(request()));
}

void QuickAuthDialog::setMethods(const QStringList &methods)
{
    m_theDialog->setProperty("methods", methods);
}

void QuickAuthDialog::setMethod(const QString &method)
{
    m_theDialog->setProperty("method", method);
}

void QuickAuthDialog::setMethodStatus(const QString &status)
{
    m_theDialog->setProperty("methodStatus", status);
}

void QuickAuthDialog::setMethodState(int state)
{
    m_theDialog->setProperty("methodState", state);
}

int QuickAuthDialog::methodState() const
{
    return m_theDialog->property("methodState").toInt();
}

#include "moc_QuickAuthDialog.cpp"
