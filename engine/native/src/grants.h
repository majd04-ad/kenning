/*
    SPDX-FileCopyrightText: 2026 Majd Abu Diab
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

// Administrator prompts: Kenning's polkit dialog asks the service for the user's finger or face;
// on a match the service gives it a token, which it hands to pam_kenning.so grant in polkit's
// helper through the PAM conversation. Only the dialog that got the token can use it, once,
// and only for a short while.

#include <chrono>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <sys/random.h>
#include <sys/types.h>

namespace FaceUnlock
{
class GrantStore
{
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::seconds lifetime{15};

    // A new token for the user, replacing any earlier one; empty if no randomness was available.
    std::string issue(uid_t uid, Clock::time_point now)
    {
        unsigned char bytes[16];
        if (getrandom(bytes, sizeof(bytes), 0) != ssize_t(sizeof(bytes))) {
            return {};
        }
        std::string token;
        for (const unsigned char byte : bytes) {
            char hex[3];
            std::snprintf(hex, sizeof(hex), "%02x", byte);
            token += hex;
        }
        m_grants[uid] = {token, now + lifetime};
        return token;
    }

    bool pending(uid_t uid, Clock::time_point now)
    {
        forgetExpired(now);
        return m_grants.contains(uid);
    }

    // True once for the right token. Any attempt uses the grant up, so it can't be guessed.
    bool use(uid_t uid, const std::string &token, Clock::time_point now)
    {
        forgetExpired(now);
        const auto found = m_grants.find(uid);
        if (found == m_grants.end()) {
            return false;
        }
        const std::string expected = found->second.token;
        m_grants.erase(found);
        if (token.size() != expected.size()) {
            return false;
        }
        unsigned char difference = 0;
        for (std::size_t i = 0; i < token.size(); ++i) {
            difference |= static_cast<unsigned char>(token[i] ^ expected[i]);
        }
        return difference == 0;
    }

    bool isEmpty(Clock::time_point now)
    {
        forgetExpired(now);
        return m_grants.empty();
    }

private:
    struct Grant {
        std::string token;
        Clock::time_point expires;
    };

    void forgetExpired(Clock::time_point now)
    {
        std::erase_if(m_grants, [now](const auto &entry) {
            return entry.second.expires <= now;
        });
    }

    std::map<uid_t, Grant> m_grants;
};
}
