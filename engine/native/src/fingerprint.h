/*
    SPDX-FileCopyrightText: 2026 Majd Abu Diab
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

// Fingerprints through fprintd, over the system bus, for the service. The service user may
// verify any user's finger by Kenning's polkit rule (polkit/50-kenning.rules).

#include "config.h"

#include <chrono>
#include <functional>
#include <string>

namespace FaceUnlock
{
struct FingerprintOptions {
    std::chrono::seconds timeout{30}; // waiting for a finger, renewed by each touch
    int maxTries = 3; // fingers that weren't recognized before giving up
};

struct FingerprintCallbacks {
    std::function<void(const std::string &)> say; // what to show, e.g. "Place your finger…"
    std::function<bool()> cancelled;
    // A finger that wasn't recognized; returns false when that locked the user out.
    std::function<bool()> noMatch;
};

// Recognized when the user has a fingerprint on the default reader, NoModel when not,
// NoReader without a reader or fprintd. message says why, for errors.
ExitCode enrolledFingerprints(const std::string &user, std::string &message);

// Waits for the user's finger on the default reader. message says why it failed.
ExitCode verifyFingerprint(const std::string &user, const FingerprintOptions &options, const FingerprintCallbacks &callbacks, std::string &message);
}
