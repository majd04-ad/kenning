/*
    SPDX-FileCopyrightText: 2026 Rishon Jonathan R
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "failurelimiter.h"

#include <QTest>

using namespace FaceUnlock;
using namespace std::chrono_literals;

namespace
{
constexpr uid_t user = 1000;
// Faces alone lock, as with max_password_failures = 0.
constexpr FailureLimits limits{.maxFailures = 3, .maxPasswordFailures = 0, .window = 15min, .lockout = 10min};
constexpr FailureLimits both{.maxFailures = 3, .maxPasswordFailures = 3, .window = 15min, .lockout = 10min};
constexpr auto face = FailureLimiter::Kind::Face;
constexpr auto password = FailureLimiter::Kind::Password;
const FailureLimiter::Clock::time_point start{};
}

class FailureLimiterTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void locksAfterTooManyFailures()
    {
        FailureLimiter limiter;
        QVERIFY(!limiter.recordFailure(user, face, limits, start));
        QVERIFY(!limiter.recordFailure(user, face, limits, start + 1min));
        QVERIFY(limiter.recordFailure(user, face, limits, start + 2min));
        QVERIFY(limiter.isLockedOut(user, limits, start + 3min));
        QVERIFY(!limiter.isLockedOut(user + 1, limits, start + 3min));
    }

    void unlocksAfterTheLockout()
    {
        FailureLimiter limiter;
        for (int i = 0; i < limits.maxFailures; ++i) {
            limiter.recordFailure(user, face, limits, start);
        }
        QVERIFY(limiter.isLockedOut(user, limits, start + 9min));
        QVERIFY(!limiter.isLockedOut(user, limits, start + 10min));
        QVERIFY(limiter.isEmpty(start + 10min));
    }

    // A scan that was waiting for the camera while another locked the user out still counts,
    // but must not end a lockout longer than the window early.
    void aFailureDuringALockoutKeepsIt()
    {
        FailureLimiter limiter;
        const FailureLimits longLockout{.maxFailures = 3, .maxPasswordFailures = 0, .window = 15min, .lockout = 60min};
        for (int i = 0; i < longLockout.maxFailures; ++i) {
            limiter.recordFailure(user, face, longLockout, start);
        }
        limiter.recordFailure(user, face, longLockout, start + 1min);
        QVERIFY(limiter.isLockedOut(user, longLockout, start + 30min));
        QVERIFY(!limiter.isLockedOut(user, longLockout, start + 60min));
    }

    void forgetsFailuresOutsideTheWindow()
    {
        FailureLimiter limiter;
        limiter.recordFailure(user, face, limits, start);
        limiter.recordFailure(user, face, limits, start + 1min);
        QVERIFY(!limiter.recordFailure(user, face, limits, start + 16min));
        QVERIFY(!limiter.isLockedOut(user, limits, start + 16min));
    }

    void survivesARestart()
    {
        FailureLimiter limiter;
        for (int i = 0; i < limits.maxFailures; ++i) {
            limiter.recordFailure(user, face, limits, start);
        }
        limiter.recordFailure(user + 1, face, limits, start);
        FailureLimiter restarted;
        restarted.restore(limiter.save());
        QVERIFY(restarted.isLockedOut(user, limits, start + 9min));
        QVERIFY(!restarted.isLockedOut(user, limits, start + 10min));
        QVERIFY(!restarted.recordFailure(user + 1, face, limits, start + 1min));
        restarted.restore("not a record\n");
        QVERIFY(!restarted.isEmpty(start + 1min));
    }

    void successClearsFailures()
    {
        FailureLimiter limiter;
        limiter.recordFailure(user, face, limits, start);
        limiter.recordFailure(user, face, limits, start);
        limiter.recordSuccess(user);
        QVERIFY(!limiter.recordFailure(user, face, limits, start));
        QVERIFY(!limiter.isEmpty(start));
    }

    void needsBothFacesAndPasswords()
    {
        FailureLimiter limiter;
        for (int i = 0; i < 5; ++i) {
            QVERIFY(!limiter.recordFailure(user, face, both, start + 1min * i));
        }
        QVERIFY(!limiter.recordFailure(user, password, both, start + 5min));
        QVERIFY(!limiter.recordFailure(user, password, both, start + 6min));
        QVERIFY(!limiter.isLockedOut(user, both, start + 6min));
        QVERIFY(limiter.recordFailure(user, password, both, start + 7min));
        QVERIFY(limiter.isLockedOut(user, both, start + 8min));
    }

    void passwordsAloneDontLock()
    {
        FailureLimiter limiter;
        for (int i = 0; i < 10; ++i) {
            QVERIFY(!limiter.recordFailure(user, password, both, start));
        }
        QVERIFY(!limiter.recordFailure(user, face, both, start));
        QVERIFY(!limiter.recordFailure(user, face, both, start));
        QVERIFY(limiter.recordFailure(user, face, both, start));
    }

    void bothMustFallInTheWindow()
    {
        FailureLimiter limiter;
        for (int i = 0; i < 3; ++i) {
            limiter.recordFailure(user, password, both, start);
        }
        limiter.recordFailure(user, face, both, start + 10min);
        limiter.recordFailure(user, face, both, start + 11min);
        // The passwords have aged out by now.
        QVERIFY(!limiter.recordFailure(user, face, both, start + 16min));
        QVERIFY(!limiter.isLockedOut(user, both, start + 16min));
    }

    void restoresBothKinds()
    {
        FailureLimiter limiter;
        limiter.recordFailure(user, face, both, start);
        limiter.recordFailure(user, face, both, start);
        limiter.recordFailure(user, face, both, start);
        limiter.recordFailure(user, password, both, start);
        limiter.recordFailure(user, password, both, start);
        FailureLimiter restarted;
        restarted.restore(limiter.save());
        QVERIFY(restarted.recordFailure(user, password, both, start + 1min));
    }

    void restoresTheOlderFormat()
    {
        // Before password failures were counted: every failure after the first three fields is a face.
        FailureLimiter restarted;
        restarted.restore("1000 0 900000000000 1 2\n");
        QVERIFY(restarted.recordFailure(user, face, limits, start));
    }

    void zeroMeansNoLimit()
    {
        FailureLimiter limiter;
        const FailureLimits unlimited{.maxFailures = 0, .maxPasswordFailures = 3, .window = 15min, .lockout = 10min};
        for (int i = 0; i < 10; ++i) {
            QVERIFY(!limiter.recordFailure(user, face, unlimited, start));
        }
        QVERIFY(!limiter.isLockedOut(user, unlimited, start));
        QVERIFY(limiter.isEmpty(start));
    }
};

QTEST_GUILESS_MAIN(FailureLimiterTest)
#include "failurelimitertest.moc"
