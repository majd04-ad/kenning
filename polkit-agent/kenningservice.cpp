/*
    SPDX-FileCopyrightText: 2026 Majd Abu Diab
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "kenningservice.h"

#include "config.h"
#include "service.h"

#include <QCoreApplication>
#include <QFile>
#include <QPointer>

#include <algorithm>
#include <initializer_list>
#include <thread>

namespace
{
// The polkit-1 PAM service takes Kenning's token; without that line a grant would be useless.
bool polkitTakesGrants()
{
    QFile file(QStringLiteral("/etc/pam.d/polkit-1"));
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    for (const QByteArray &line : file.readAll().split('\n')) {
        const QByteArray trimmed = line.trimmed();
        if (!trimmed.startsWith('#') && trimmed.contains("pam_kenning.so") && trimmed.contains("grant")) {
            return true;
        }
    }
    return false;
}

// Runs f in the main thread, if the service object still exists by then.
template<typename F>
void deliver(const QPointer<KenningService> &service, F f)
{
    QMetaObject::invokeMethod(
        QCoreApplication::instance(),
        [service, f] {
            if (service) {
                f(service.data());
            }
        },
        Qt::QueuedConnection);
}
}

KenningService::~KenningService()
{
    cancel();
}

void KenningService::findMethods(const QString &user)
{
    const QPointer<KenningService> self(this);
    std::thread([self, user] {
        const FaceUnlock::Config config;
        const std::string name = user.toStdString();
        // A method that is set up stays a button while it can't be used (the camera switched
        // off, too many attempts): choosing it says why, where a missing button says nothing.
        QStringList methods;
        QStringList ready;
        const auto offer = [&](const QString &method, int code, std::initializer_list<int> setUp) {
            if (code == FaceUnlock::Recognized) {
                methods << method;
                ready << method;
            } else if (std::find(setUp.begin(), setUp.end(), code) != setUp.end()) {
                methods << method;
            }
        };
        if (polkitTakesGrants()) {
            if (config.boolean("polkit/fingerprint", false)) {
                offer(QStringLiteral("fingerprint"), FaceUnlock::askService("has-fingerprint", name, FaceUnlock::quickTimeoutSeconds), {FaceUnlock::LockedOut});
            }
            if (config.boolean("polkit/face", false) && !config.boolean("core/disabled", false)) {
                offer(QStringLiteral("face"),
                      FaceUnlock::askService("has-model", name, FaceUnlock::quickTimeoutSeconds),
                      {FaceUnlock::LockedOut, FaceUnlock::NoCamera, FaceUnlock::NeedsPassword});
            }
        }
        deliver(self, [user, methods, ready](KenningService *service) {
            Q_EMIT service->methodsFound(user, methods, ready);
        });
    }).detach();
}

void KenningService::requestGrant(const QString &user, const QString &method)
{
    cancel();
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    m_cancelled = cancelled;
    const QPointer<KenningService> self(this);
    std::thread([self, cancelled, user, method] {
        const FaceUnlock::Config config;
        const int timeout = method == QLatin1String("face") ? FaceUnlock::scanTimeoutSeconds(config) : FaceUnlock::fingerprintTimeoutSeconds(config);
        QString lastError;
        const FaceUnlock::Reply reply = FaceUnlock::callService({
            .command = "grant",
            .user = user.toStdString(),
            .argument = method.toStdString(),
            .idleTimeoutSeconds = timeout,
            .onMessage =
                [&](FaceUnlock::MessageKind kind, const std::string &text) {
                    const QString line = QString::fromStdString(text);
                    const bool error = kind == FaceUnlock::MessageKind::Error;
                    if (error) {
                        lastError = line;
                    }
                    deliver(self, [cancelled, line, error](KenningService *service) {
                        if (!*cancelled) {
                            Q_EMIT service->message(line, error);
                        }
                    });
                },
            .cancelled =
                [cancelled] {
                    return cancelled->load();
                },
        });
        deliver(self, [cancelled, method, reply, lastError](KenningService *service) {
            if (!*cancelled) {
                Q_EMIT service->grantFinished(method, reply.code, QString::fromStdString(reply.data), lastError);
            }
        });
    }).detach();
}

void KenningService::cancel()
{
    if (m_cancelled) {
        *m_cancelled = true;
        m_cancelled.reset();
    }
}

#include "moc_kenningservice.cpp"
