/*
    SPDX-FileCopyrightText: 2026 Majd Abu Diab
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "fingerprint.h"

#include <systemd/sd-bus.h>

#include <cstring>
#include <deque>
#include <map>
#include <optional>

namespace FaceUnlock
{
namespace
{
constexpr const char *fprintService = "net.reactivated.Fprint";
constexpr const char *managerPath = "/net/reactivated/Fprint/Manager";
constexpr const char *managerInterface = "net.reactivated.Fprint.Manager";
constexpr const char *deviceInterface = "net.reactivated.Fprint.Device";

struct BusError {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    BusError() = default;
    BusError(const BusError &) = delete;
    BusError &operator=(const BusError &) = delete;
    ~BusError()
    {
        sd_bus_error_free(&error);
    }
    bool is(const char *name) const
    {
        return sd_bus_error_has_name(&error, name);
    }
};

struct BusMessage {
    sd_bus_message *message = nullptr;
    BusMessage() = default;
    BusMessage(const BusMessage &) = delete;
    BusMessage &operator=(const BusMessage &) = delete;
    ~BusMessage()
    {
        sd_bus_message_unref(message);
    }
};

struct VerifyStatus {
    std::string result;
    bool done = false;
};

struct Events {
    std::string owner; // fprintd's unique bus name: nobody else may report a match
    std::deque<VerifyStatus> statuses;
};

int onVerifyStatus(sd_bus_message *message, void *userdata, sd_bus_error *)
{
    auto *events = static_cast<Events *>(userdata);
    const char *sender = sd_bus_message_get_sender(message);
    const char *result = nullptr;
    int done = 0;
    if (!sender || events->owner != sender || sd_bus_message_read(message, "sb", &result, &done) < 0) {
        return 0;
    }
    events->statuses.push_back({result, done != 0});
    return 0;
}

// A connection to fprintd's default reader.
class Reader
{
public:
    Reader(const Reader &) = delete;
    Reader &operator=(const Reader &) = delete;
    Reader() = default;

    ~Reader()
    {
        if (m_verifying) {
            call("VerifyStop");
        }
        if (m_claimed) {
            call("Release");
        }
        sd_bus_slot_unref(m_slot);
        sd_bus_flush_close_unref(m_bus);
    }

    // NoReader (with message) unless there is a reader.
    ExitCode open(std::string &message)
    {
        if (sd_bus_open_system(&m_bus) < 0) {
            message = "The fingerprint service can't be reached";
            return NoReader;
        }
        BusError error;
        BusMessage reply;
        if (sd_bus_call_method(m_bus, fprintService, managerPath, managerInterface, "GetDefaultDevice", &error.error, &reply.message, "") < 0) {
            message = "No fingerprint reader found";
            return NoReader;
        }
        const char *path = nullptr;
        if (sd_bus_message_read(reply.message, "o", &path) < 0 || !path) {
            message = "No fingerprint reader found";
            return NoReader;
        }
        m_path = path;
        return Recognized;
    }

    // Recognized when the user has at least one finger on this reader.
    ExitCode hasFingers(const std::string &user, std::string &message)
    {
        BusError error;
        BusMessage reply;
        if (sd_bus_call_method(m_bus, fprintService, m_path.c_str(), deviceInterface, "ListEnrolledFingers", &error.error, &reply.message, "s", user.c_str())
            < 0) {
            if (error.is("net.reactivated.Fprint.Error.NoEnrolledPrints")) {
                message = "No fingerprints are set up";
                return NoModel;
            }
            message = permissionMessage(error);
            return Error;
        }
        int fingers = 0;
        if (sd_bus_message_enter_container(reply.message, 'a', "s") >= 0) {
            const char *finger = nullptr;
            while (sd_bus_message_read(reply.message, "s", &finger) > 0) {
                ++fingers;
            }
        }
        message = fingers > 0 ? std::string() : "No fingerprints are set up";
        return fingers > 0 ? Recognized : NoModel;
    }

    ExitCode claim(const std::string &user, std::string &message)
    {
        // Signals are only believed from fprintd's own connection, found before claiming.
        BusError error;
        BusMessage reply;
        const char *owner = nullptr;
        if (sd_bus_call_method(m_bus,
                               "org.freedesktop.DBus",
                               "/org/freedesktop/DBus",
                               "org.freedesktop.DBus",
                               "GetNameOwner",
                               &error.error,
                               &reply.message,
                               "s",
                               fprintService)
                < 0
            || sd_bus_message_read(reply.message, "s", &owner) < 0 || !owner) {
            message = "The fingerprint service can't be reached";
            return Error;
        }
        m_events.owner = owner;
        if (sd_bus_match_signal(m_bus, &m_slot, owner, m_path.c_str(), deviceInterface, "VerifyStatus", onVerifyStatus, &m_events) < 0) {
            message = "The fingerprint service can't be reached";
            return Error;
        }

        BusError claimError;
        if (sd_bus_call_method(m_bus, fprintService, m_path.c_str(), deviceInterface, "Claim", &claimError.error, nullptr, "s", user.c_str()) < 0) {
            message = claimError.is("net.reactivated.Fprint.Error.AlreadyInUse") ? "The fingerprint reader is in use" : permissionMessage(claimError);
            return Error;
        }
        m_claimed = true;
        return Recognized;
    }

    bool swipe()
    {
        char *type = nullptr;
        BusError error;
        sd_bus_get_property_string(m_bus, fprintService, m_path.c_str(), deviceInterface, "scan-type", &error.error, &type);
        const bool swipe = type && std::strcmp(type, "swipe") == 0;
        std::free(type);
        return swipe;
    }

    ExitCode startVerifying(std::string &message)
    {
        BusError error;
        if (sd_bus_call_method(m_bus, fprintService, m_path.c_str(), deviceInterface, "VerifyStart", &error.error, nullptr, "s", "any") < 0) {
            if (error.is("net.reactivated.Fprint.Error.NoEnrolledPrints")) {
                message = "No fingerprints are set up";
                return NoModel;
            }
            message = "The fingerprint reader failed to start";
            return Error;
        }
        m_verifying = true;
        return Recognized;
    }

    void stopVerifying()
    {
        if (m_verifying) {
            call("VerifyStop");
            m_verifying = false;
        }
    }

    // The next status from the reader, waiting up to timeout. Empty on timeout; false on bus errors.
    bool next(std::chrono::milliseconds timeout, std::optional<VerifyStatus> &status)
    {
        status.reset();
        int processed = 0;
        while ((processed = sd_bus_process(m_bus, nullptr)) > 0) { }
        if (processed < 0) {
            return false;
        }
        if (m_events.statuses.empty() && sd_bus_wait(m_bus, std::chrono::microseconds(timeout).count()) < 0) {
            return false;
        }
        while ((processed = sd_bus_process(m_bus, nullptr)) > 0) { }
        if (!m_events.statuses.empty()) {
            status = m_events.statuses.front();
            m_events.statuses.pop_front();
        }
        return processed >= 0;
    }

private:
    static std::string permissionMessage(const BusError &error)
    {
        return error.is("net.reactivated.Fprint.Error.PermissionDenied") ? "Kenning may not use the fingerprint reader; is its polkit rule installed?"
                                                                         : "The fingerprint reader failed";
    }

    void call(const char *method)
    {
        BusError error;
        sd_bus_call_method(m_bus, fprintService, m_path.c_str(), deviceInterface, method, &error.error, nullptr, "");
    }

    sd_bus *m_bus = nullptr;
    sd_bus_slot *m_slot = nullptr;
    std::string m_path;
    Events m_events;
    bool m_claimed = false;
    bool m_verifying = false;
};

// What to say for a scan that has to be repeated, as pam_fprintd says it.
const char *retryMessage(const std::string &result, bool swipe)
{
    static const std::map<std::string, const char *> messages = {
        {"verify-swipe-too-short", "Swipe was too short, try again"},
        {"verify-finger-not-centered", "Your finger was not centered, try again"},
        {"verify-remove-and-retry", "Remove your finger, and try again"},
        {"verify-too-fast", "Your finger was lifted too quickly, try again"},
    };
    if (const auto found = messages.find(result); found != messages.end()) {
        return found->second;
    }
    return swipe ? "Swipe your finger again" : "Place your finger on the reader again";
}
}

ExitCode enrolledFingerprints(const std::string &user, std::string &message)
{
    Reader reader;
    if (const ExitCode code = reader.open(message); code != Recognized) {
        return code;
    }
    return reader.hasFingers(user, message);
}

ExitCode verifyFingerprint(const std::string &user, const FingerprintOptions &options, const FingerprintCallbacks &callbacks, std::string &message)
{
    Reader reader;
    if (const ExitCode code = reader.open(message); code != Recognized) {
        return code;
    }
    if (const ExitCode code = reader.hasFingers(user, message); code != Recognized) {
        return code;
    }
    if (const ExitCode code = reader.claim(user, message); code != Recognized) {
        return code;
    }
    const bool swipe = reader.swipe();
    if (const ExitCode code = reader.startVerifying(message); code != Recognized) {
        return code;
    }
    callbacks.say(swipe ? "Swipe your finger across the fingerprint reader" : "Place your finger on the fingerprint reader");

    using Clock = std::chrono::steady_clock;
    auto deadline = Clock::now() + options.timeout;
    int tries = 0;
    for (;;) {
        if (callbacks.cancelled && callbacks.cancelled()) {
            message = "Cancelled";
            return Aborted;
        }
        if (Clock::now() >= deadline) {
            message = "No finger was placed on the reader in time";
            return Timeout;
        }
        std::optional<VerifyStatus> status;
        if (!reader.next(std::chrono::milliseconds(200), status)) {
            message = "The fingerprint service stopped answering";
            return Error;
        }
        if (!status) {
            continue;
        }
        deadline = Clock::now() + options.timeout;
        if (status->result == "verify-match") {
            return Recognized;
        }
        if (status->result == "verify-disconnected") {
            message = "The fingerprint reader was disconnected";
            return Error;
        }
        if (status->result == "verify-unknown-error") {
            message = "The fingerprint reader failed";
            return Error;
        }
        if (status->result == "verify-no-match") {
            ++tries;
            if (callbacks.noMatch && !callbacks.noMatch()) {
                message = "Too many fingerprint attempts; use your password";
                return LockedOut;
            }
            if (tries >= options.maxTries) {
                message = "Fingerprint not recognized";
                return NotRecognized;
            }
            callbacks.say("Fingerprint not recognized, try again");
        } else {
            callbacks.say(retryMessage(status->result, swipe));
        }
        if (status->done) {
            reader.stopVerifying();
            if (const ExitCode code = reader.startVerifying(message); code != Recognized) {
                return code;
            }
        }
    }
}
}
