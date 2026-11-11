/*
    SPDX-FileCopyrightText: 2026 Majd Abu Diab
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "grants.h"

#include <QTest>

using namespace FaceUnlock;
using namespace std::chrono_literals;

namespace
{
constexpr uid_t user = 1000;
const GrantStore::Clock::time_point start{};
}

class GrantStoreTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void usedOnce()
    {
        GrantStore grants;
        const std::string token = grants.issue(user, start);
        QCOMPARE(token.size(), std::size_t(32));
        QVERIFY(grants.pending(user, start + 1s));
        QVERIFY(grants.use(user, token, start + 1s));
        QVERIFY(!grants.pending(user, start + 1s));
        QVERIFY(!grants.use(user, token, start + 1s));
    }

    void onlyWithTheToken()
    {
        GrantStore grants;
        const std::string token = grants.issue(user, start);
        QVERIFY(!grants.use(user + 1, token, start));
        std::string wrong = token;
        wrong.back() = wrong.back() == '0' ? '1' : '0';
        QVERIFY(!grants.use(user, wrong, start));
        // A wrong guess uses the grant up.
        QVERIFY(!grants.use(user, token, start));
    }

    void expires()
    {
        GrantStore grants;
        const std::string token = grants.issue(user, start);
        QVERIFY(!grants.use(user, token, start + GrantStore::lifetime));
        QVERIFY(grants.isEmpty(start + GrantStore::lifetime));
    }

    void newTokenReplacesTheOld()
    {
        GrantStore grants;
        const std::string first = grants.issue(user, start);
        const std::string second = grants.issue(user, start);
        QVERIFY(first != second);
        QVERIFY(!grants.use(user, first, start));
        QVERIFY(!grants.pending(user, start));
    }
};

QTEST_GUILESS_MAIN(GrantStoreTest)
#include "grantstoretest.moc"
