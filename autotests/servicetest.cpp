/*
    SPDX-FileCopyrightText: 2026 Rishon Jonathan R
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "service.h"

#include <QTest>

#include <vector>

using namespace FaceUnlock;

class ServiceTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void parsesRequests()
    {
        const std::optional<Request> request = parseRequest("compare alice\n");
        QVERIFY(request);
        QCOMPARE(request->command, std::string("compare"));
        QCOMPARE(request->user, std::string("alice"));
        QVERIFY(request->argument.empty());
    }

    void parsesTheArgument()
    {
        const std::optional<Request> request = parseRequest("enroll alice Glasses off\n");
        QVERIFY(request);
        QCOMPARE(request->command, std::string("enroll"));
        QCOMPARE(request->user, std::string("alice"));
        QCOMPARE(request->argument, std::string("Glasses off"));
    }

    void parsesReplies()
    {
        std::vector<std::pair<MessageKind, std::string>> messages;
        const auto onMessage = [&messages](MessageKind kind, const std::string &text) {
            messages.emplace_back(kind, text);
        };
        Reply reply;
        QVERIFY(!parseReplyLine("# Place your finger on the reader", reply, onMessage));
        QVERIFY(!parseReplyLine("! Fingerprint not recognized", reply, onMessage));
        QCOMPARE(messages.size(), std::size_t(2));
        QVERIFY(messages[0].first == MessageKind::Info);
        QCOMPARE(messages[0].second, std::string("Place your finger on the reader"));
        QVERIFY(messages[1].first == MessageKind::Error);

        QVERIFY(parseReplyLine("0 3f2a", reply, onMessage));
        QCOMPARE(reply.code, 0);
        QCOMPARE(reply.data, std::string("3f2a"));
        QVERIFY(parseReplyLine("16", reply, onMessage));
        QCOMPARE(reply.code, 16);
        QVERIFY(reply.data.empty());
        QVERIFY(parseReplyLine("garbage", reply, onMessage));
        QCOMPARE(reply.code, -1);
    }

    void rejectsMalformedRequests_data()
    {
        QTest::addColumn<QString>("text");
        QTest::newRow("no newline") << QStringLiteral("compare alice");
        QTest::newRow("no user") << QStringLiteral("compare\n");
        QTest::newRow("empty user") << QStringLiteral("compare \n");
        QTest::newRow("empty command") << QStringLiteral(" alice\n");
        QTest::newRow("space after the newline") << QStringLiteral("compare\n alice");
    }

    void rejectsMalformedRequests()
    {
        QFETCH(QString, text);
        QVERIFY(!parseRequest(text.toStdString()));
    }
};

QTEST_GUILESS_MAIN(ServiceTest)
#include "servicetest.moc"
