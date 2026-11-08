/*
    SPDX-FileCopyrightText: 2026 Majd Abu Diab
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "turnqueue.h"

#include <QTest>

#include <atomic>
#include <thread>
#include <vector>

using namespace FaceUnlock;
using namespace std::chrono_literals;

class TurnQueueTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void oneRequestPerUser()
    {
        TurnQueue queue;
        auto first = queue.take(1000);
        QVERIFY(first);
        QVERIFY(!queue.take(1000));
        first.reset();
        QVERIFY(queue.take(1000));
    }

    void rootIsNotLimited()
    {
        TurnQueue queue;
        auto first = queue.take(0);
        QVERIFY(first);
        std::atomic<bool> second = false;
        std::thread waiter([&] {
            second = queue.take(0).has_value();
        });
        std::this_thread::sleep_for(50ms);
        QCOMPARE(queue.size(), std::size_t(2));
        first.reset();
        waiter.join();
        QVERIFY(second);
    }

    void servesInOrder()
    {
        TurnQueue queue;
        auto holder = queue.take(1);
        std::vector<int> served;
        std::mutex servedMutex;
        std::vector<std::thread> waiters;
        for (int user = 2; user <= 4; ++user) {
            waiters.emplace_back([&, user] {
                auto turn = queue.take(uid_t(user));
                const std::lock_guard lock(servedMutex);
                served.push_back(user);
            });
            // Let each one join the queue before the next.
            while (queue.size() < std::size_t(user)) {
                std::this_thread::sleep_for(1ms);
            }
        }
        holder.reset();
        for (auto &waiter : waiters) {
            waiter.join();
        }
        QCOMPARE(served, (std::vector<int>{2, 3, 4}));
    }

    void cancelledWhileWaiting()
    {
        TurnQueue queue;
        auto holder = queue.take(1);
        std::atomic<bool> cancel = false;
        std::atomic<bool> got = true;
        std::thread waiter([&] {
            got = queue
                      .take(2,
                            [&] {
                                return cancel.load();
                            })
                      .has_value();
        });
        std::this_thread::sleep_for(50ms);
        cancel = true;
        waiter.join();
        QVERIFY(!got);
        QCOMPARE(queue.size(), std::size_t(1));
        // The cancelled user may ask again.
        holder.reset();
        QVERIFY(queue.take(2));
    }
};

QTEST_GUILESS_MAIN(TurnQueueTest)
#include "turnqueuetest.moc"
