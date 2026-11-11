/*
    SPDX-FileCopyrightText: 2026 Rishon Jonathan R
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "failurelimiter.h"

#include <algorithm>
#include <sstream>

namespace FaceUnlock
{
FailureLimits FailureLimits::fromConfig(const Config &config)
{
    return {
        .maxFailures = std::max(0, int(config.number("core/max_failures", 5))),
        .window = std::chrono::minutes(std::max(1, int(config.number("core/failure_window", 15)))),
        .lockout = std::chrono::minutes(std::max(1, int(config.number("core/lockout_minutes", 10)))),
    };
}

bool FailureLimiter::isLockedOut(uid_t uid, const FailureLimits &limits, Clock::time_point now)
{
    forgetExpired(now);
    const auto found = m_records.find(uid);
    return limits.maxFailures > 0 && found != m_records.end() && now < found->second.lockedUntil;
}

void FailureLimiter::recordSuccess(uid_t uid)
{
    m_records.erase(uid);
}

bool FailureLimiter::recordFailure(uid_t uid, const FailureLimits &limits, Clock::time_point now)
{
    if (limits.maxFailures == 0) {
        return false;
    }
    Record &record = m_records[uid];
    std::erase_if(record.failures, [&](Clock::time_point time) {
        return time + limits.window <= now;
    });
    record.failures.push_back(now);
    // Never sooner than a lockout still running: a scan that waited for the camera may end in one.
    record.forgetAfter = std::max(record.forgetAfter, now + limits.window);
    if (int(record.failures.size()) < limits.maxFailures) {
        return false;
    }
    record.failures.clear();
    record.lockedUntil = now + limits.lockout;
    record.forgetAfter = record.lockedUntil;
    return true;
}

bool FailureLimiter::isEmpty(Clock::time_point now)
{
    forgetExpired(now);
    return m_records.empty();
}

// One line per user: uid, lockedUntil, forgetAfter, then each failure, in clock ticks.
std::string FailureLimiter::save() const
{
    std::ostringstream text;
    for (const auto &[uid, record] : m_records) {
        text << uid << ' ' << record.lockedUntil.time_since_epoch().count() << ' ' << record.forgetAfter.time_since_epoch().count();
        for (const Clock::time_point failure : record.failures) {
            text << ' ' << failure.time_since_epoch().count();
        }
        text << '\n';
    }
    return text.str();
}

void FailureLimiter::restore(std::string_view text)
{
    const auto at = [](Clock::rep ticks) {
        return Clock::time_point(Clock::duration(ticks));
    };
    std::istringstream lines{std::string(text)};
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        uid_t uid = 0;
        Clock::rep lockedUntil = 0;
        Clock::rep forgetAfter = 0;
        if (!(fields >> uid >> lockedUntil >> forgetAfter)) {
            continue;
        }
        Record record{.failures = {}, .lockedUntil = at(lockedUntil), .forgetAfter = at(forgetAfter)};
        for (Clock::rep failure = 0; fields >> failure;) {
            record.failures.push_back(at(failure));
        }
        m_records[uid] = record;
    }
}

void FailureLimiter::forgetExpired(Clock::time_point now)
{
    std::erase_if(m_records, [now](const auto &entry) {
        return entry.second.forgetAfter <= now;
    });
}
}
