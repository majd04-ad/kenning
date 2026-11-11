/*
    SPDX-FileCopyrightText: 2026 Rishon Jonathan R
    SPDX-License-Identifier: GPL-2.0-or-later
*/
// pam_kenning: asks the kenning service, so OpenCV never loads here. Messages match pam_howdy.

#include "config.h"
#include "service.h"

#include <security/pam_ext.h>
#include <security/pam_modules.h>

#include <systemd/sd-bus.h>
#include <systemd/sd-login.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <syslog.h>
#include <thread>
#include <unistd.h>

namespace
{
using FaceUnlock::Config;

// The camera is switched off, unplugged, in use by another program, or none is set up. The lock
// and login screens look for "camera".
constexpr const char *cameraMessage = "Failure, camera not available";

void say(pam_handle_t *pamh, const std::string &message)
{
    pam_info(pamh, "%s", message.c_str());
}

bool lidClosed()
{
    std::error_code error;
    for (const auto &entry : std::filesystem::directory_iterator("/proc/acpi/button/lid", error)) {
        std::ifstream state(entry.path() / "state");
        std::string line;
        if (std::getline(state, line) && line.find("closed") != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool preparingForSleep(sd_bus *bus)
{
    int preparing = 0;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    const int result = sd_bus_get_property_trivial(bus,
                                                   "org.freedesktop.login1",
                                                   "/org/freedesktop/login1",
                                                   "org.freedesktop.login1.Manager",
                                                   "PreparingForSleep",
                                                   &error,
                                                   'b',
                                                   &preparing);
    sd_bus_error_free(&error);
    return result >= 0 && preparing;
}

// Plasma locks the screen on the way into suspend, and the lock screen asks for a face at once:
// wait until the computer has woken up, when someone may be there to look at it.
void waitUntilAwake()
{
    sd_bus *bus = nullptr;
    if (sd_bus_open_system(&bus) < 0) {
        return;
    }
    // Steady time stands still while suspended, so this only bounds a sleep that never happens.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (preparingForSleep(bus) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    sd_bus_flush_close_unref(bus);
}

// A local, active session of this user, such as the desktop they are sitting at.
bool hasLocalSession(uid_t uid)
{
    char *session = nullptr;
    if (sd_uid_get_display(uid, &session) < 0 || !session) {
        return false;
    }
    const bool local = sd_session_is_remote(session) == 0 && sd_session_is_active(session) > 0;
    std::free(session);
    return local;
}

// True when the person asking may not be at this computer: logind decides, the environment can only add a refusal.
bool remoteSession(pam_handle_t *pamh, const Config &config)
{
    const void *rhost = nullptr;
    if (pam_get_item(pamh, PAM_RHOST, &rhost) == PAM_SUCCESS && rhost && *static_cast<const char *>(rhost)
        && std::strcmp(static_cast<const char *>(rhost), "localhost") != 0) {
        return true;
    }
    if (std::getenv("SSH_CONNECTION") || std::getenv("SSH_CLIENT") || std::getenv("SSH_TTY")) {
        return true;
    }
    char *session = nullptr;
    if (sd_pid_get_session(0, &session) >= 0 && session) {
        const int remote = sd_session_is_remote(session);
        std::free(session);
        return remote != 0; // remote, or logind can't tell
    }
    // Outside any session. Root callers, such as the login manager, have no session yet.
    const uid_t caller = getuid();
    if (caller == 0) {
        return false;
    }
    // A setuid program raising the user to root: sudo or su from a Plasma 6 terminal, or
    // polkit's helper. Any program of this user can run outside a session, including one
    // started over SSH with systemd-run --user, so whoever sits at the camera would approve
    // it: only when allowed. The lock screen runs as the user and isn't affected.
    if (geteuid() == 0 && !config.boolean("core/allow_outside_session", false)) {
        return true;
    }
    // On Plasma 6 the lock screen and apps run under the user's service manager: only for a
    // user at this computer.
    return !hasLocalSession(caller);
}

// --- wrong passwords -----------------------------------------------------------------
// The service locks face unlock only after wrong passwords too. A module can't run after a
// password module that failed (the stack stops), so pam_kenning, first in the stack, watches
// the conversation for a typed password and leaves data whose cleanup pam_end calls with the
// application's final result. (PAM_AUTHTOK can't tell: pam_authenticate clears it on return.)
constexpr const char *passwordWatchKey = "kenning-password-watch";

struct PasswordWatch {
    pam_conv original{}; // the application's
    pam_conv wrapper{};
    bool typed = false; // in the current attempt
};

// Passes everything to the application's conversation, noting a non-empty answer to a
// hidden prompt. Face-only stacks (kde-face) and the login screen's automatic attempt,
// which answers with an empty password, type none.
int watchConversation(int count, const pam_message **messages, pam_response **responses, void *data)
{
    auto *watch = static_cast<PasswordWatch *>(data);
    const int result = watch->original.conv(count, messages, responses, watch->original.appdata_ptr);
    if (result == PAM_SUCCESS && responses && *responses) {
        for (int i = 0; i < count; ++i) {
            const char *answer = (*responses)[i].resp;
            watch->typed |= messages[i]->msg_style == PAM_PROMPT_ECHO_OFF && answer && *answer;
        }
    }
    return result;
}

void reportPasswordFailure(pam_handle_t *pamh)
{
    const void *user = nullptr;
    if (pam_get_item(pamh, PAM_USER, &user) == PAM_SUCCESS && user && *static_cast<const char *>(user)) {
        FaceUnlock::askService("password-failed", static_cast<const char *>(user), FaceUnlock::quickTimeoutSeconds);
    }
}

void finishPasswordWatch(pam_handle_t *pamh, void *data, int status)
{
    auto *watch = static_cast<PasswordWatch *>(data);
    // sudo adds PAM_DATA_SILENT to its result; PAM_DATA_REPLACE never happens (watchPassword
    // reuses its data), but would carry no result.
    const int result = status & ~(PAM_DATA_SILENT | PAM_DATA_REPLACE);
    if (!(status & PAM_DATA_REPLACE) && result != PAM_SUCCESS && watch->typed) {
        reportPasswordFailure(pamh);
    }
    delete watch;
}

void watchPassword(pam_handle_t *pamh)
{
    const void *existing = nullptr;
    PasswordWatch *watch = nullptr;
    if (pam_get_data(pamh, passwordWatchKey, &existing) == PAM_SUCCESS && existing) {
        // Another attempt on the same handle, as sudo makes up to three: the last one failed.
        watch = static_cast<PasswordWatch *>(const_cast<void *>(existing));
        if (watch->typed) {
            reportPasswordFailure(pamh);
        }
        watch->typed = false;
    } else {
        watch = new PasswordWatch;
        if (pam_set_data(pamh, passwordWatchKey, watch, finishPasswordWatch) != PAM_SUCCESS) {
            delete watch;
            return;
        }
    }
    // Wrap the application's conversation, unless it is wrapped already.
    const void *item = nullptr;
    if (pam_get_item(pamh, PAM_CONV, &item) != PAM_SUCCESS || !item) {
        return;
    }
    const auto *conversation = static_cast<const pam_conv *>(item);
    if (conversation->conv == watchConversation || !conversation->conv) {
        return;
    }
    // pam_set_item copies the struct and frees the old one, so keep the original by value.
    watch->original = *conversation;
    watch->wrapper = {watchConversation, watch};
    pam_set_item(pamh, PAM_CONV, &watch->wrapper);
}
}

extern "C" {

PAM_EXTERN int pam_sm_authenticate(pam_handle_t *pamh, int, int, const char **)
{
    // Set by the login manager while checking a typed password.
    if (const char *skip = pam_getenv(pamh, "FACEUNLOCK_SKIP"); skip && std::strcmp(skip, "1") == 0) {
        watchPassword(pamh);
        return PAM_AUTHINFO_UNAVAIL;
    }

    const Config config;
    if (config.boolean("core/disabled", false)) {
        return PAM_AUTHINFO_UNAVAIL;
    }
    // Whatever happens to the face, a password may follow.
    watchPassword(pamh);
    if (config.boolean("core/abort_if_ssh", true) && remoteSession(pamh, config)) {
        return PAM_AUTHINFO_UNAVAIL;
    }
    waitUntilAwake();
    if (config.boolean("core/abort_if_lid_closed", true) && lidClosed()) {
        return PAM_AUTHINFO_UNAVAIL;
    }

    const char *user = nullptr;
    if (pam_get_user(pamh, &user, nullptr) != PAM_SUCCESS || !user || !*user) {
        return PAM_USER_UNKNOWN;
    }
    // Quick check first, so users without a face model never see the scan start.
    const int hasModel = FaceUnlock::askService("has-model", user, FaceUnlock::quickTimeoutSeconds);
    if (hasModel < 0) {
        pam_syslog(pamh, LOG_ERR, "Face unlock service not reachable at %s", FaceUnlock::socketPath);
        return PAM_AUTHINFO_UNAVAIL;
    }
    if (hasModel == FaceUnlock::LockedOut) {
        say(pamh, "Failure, too many attempts; use your password");
        return PAM_AUTHINFO_UNAVAIL;
    }
    if (hasModel == FaceUnlock::NeedsPassword) {
        say(pamh, "Failure, use your password after a restart");
        return PAM_AUTHINFO_UNAVAIL;
    }
    if (hasModel == FaceUnlock::NoCamera) {
        // Said, and a failure rather than "unavailable": the lock screen then offers to try again
        // once the camera is back, instead of dropping the Face method until the next lock.
        say(pamh, cameraMessage);
        pam_syslog(pamh, LOG_NOTICE, "Face unlock skipped: the camera is off, unplugged or not set up");
        return PAM_AUTH_ERR;
    }
    if (hasModel != FaceUnlock::Recognized) {
        return PAM_AUTHINFO_UNAVAIL;
    }

    if (config.boolean("core/detection_notice", true)) {
        say(pamh, "Attempting facial authentication");
    }

    int code = FaceUnlock::askService("compare", user, FaceUnlock::scanTimeoutSeconds(config));
    if (code < 0) {
        code = FaceUnlock::Error;
    }
    switch (code) {
    case FaceUnlock::Recognized:
        if (!config.boolean("core/no_confirmation", false)) {
            say(pamh, std::string("Identified face as ") + user);
        }
        pam_syslog(pamh, LOG_INFO, "Login approved");
        return PAM_SUCCESS;
    case FaceUnlock::NoModel:
        say(pamh, "Failure, no face model known");
        return PAM_AUTHINFO_UNAVAIL;
    case FaceUnlock::LockedOut:
        say(pamh, "Failure, too many attempts; use your password");
        return PAM_AUTHINFO_UNAVAIL;
    case FaceUnlock::NeedsPassword:
        say(pamh, "Failure, use your password after a restart");
        return PAM_AUTHINFO_UNAVAIL;
    case FaceUnlock::Timeout:
        say(pamh, "Failure, timeout reached");
        break;
    case FaceUnlock::TooDark:
        say(pamh, "Failure, image too dark");
        break;
    case FaceUnlock::TooBright:
        say(pamh, "Failure, image too bright");
        break;
    case FaceUnlock::NoCamera:
        say(pamh, cameraMessage);
        break;
    default:
        say(pamh, "Failure, general abort");
        break;
    }
    pam_syslog(pamh, LOG_NOTICE, "Face not recognized (code %d)", code);
    return PAM_AUTH_ERR;
}

PAM_EXTERN int pam_sm_setcred(pam_handle_t *, int, int, const char **)
{
    return PAM_IGNORE;
}
}
