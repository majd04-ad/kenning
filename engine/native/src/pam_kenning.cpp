/*
    SPDX-FileCopyrightText: 2026 Rishon Jonathan R
    SPDX-License-Identifier: GPL-2.0-or-later
*/
// pam_kenning: asks the kenning service, so OpenCV never loads here. Face messages match pam_howdy.
//
// Options:
//   (none)         face unlock; on the login screen KENNING_METHOD=fingerprint in the PAM
//                  environment asks for the finger instead, and =password skips Kenning
//   fingerprint    the user's finger, through the service and fprintd. In front of the password
//                  of a terminal program (sudo): Ctrl+C while the reader waits goes on to the
//                  password, and without a terminal nobody is asked for a finger
//   explicit       with fingerprint: only once the lock screen answers fingerprintStartPrompt.
//                  Plasma also runs its fingerprint service unasked, beside the password,
//                  and never answers prompts from that one, so the finger is only read when
//                  the user chose Fingerprint.
//   grant          administrator prompts: succeeds with a token Kenning's polkit dialog got
//                  from the service for a finger or face, and passes at once without one

#include "config.h"
#include "service.h"

#include <security/pam_ext.h>
#include <security/pam_modules.h>

#include <systemd/sd-bus.h>
#include <systemd/sd-login.h>

#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <syslog.h>
#include <thread>
#include <unistd.h>

namespace
{
using FaceUnlock::Config;

// Answered "start" by the lock screen add-on (lockscreen/kenning-lockscreen.py).
constexpr const char *fingerprintStartPrompt = "kenning-start-fingerprint";

// The camera is switched off, unplugged, in use by another program, or none is set up. The lock
// and login screens look for "camera".
constexpr const char *cameraMessage = "Failure, camera not available";

enum class Mode {
    Face,
    Fingerprint,
    Grant,
};

struct Options {
    Mode mode = Mode::Face;
    bool explicitStart = false;
};

// When the reader is asked for a finger.
enum class FingerprintStart {
    Now, // the login screen's Use Fingerprint button
    WhenAsked, // the lock screen, once Fingerprint is chosen there
    Terminal, // in front of a terminal program's password, such as sudo's
};

Options parseOptions(pam_handle_t *pamh, int argc, const char **argv)
{
    Options options;
    for (int i = 0; i < argc; ++i) {
        if (std::strcmp(argv[i], "fingerprint") == 0) {
            options.mode = Mode::Fingerprint;
        } else if (std::strcmp(argv[i], "grant") == 0) {
            options.mode = Mode::Grant;
        } else if (std::strcmp(argv[i], "explicit") == 0) {
            options.explicitStart = true;
        } else {
            pam_syslog(pamh, LOG_ERR, "Unknown option %s", argv[i]);
        }
    }
    return options;
}

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
bool remoteSession(pam_handle_t *pamh)
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
    // Outside any session (on Plasma 6, apps run under the user's service manager): only for
    // a user at this computer. Root callers, such as the login manager, have no session yet.
    const uid_t caller = getuid();
    return caller != 0 && !hasLocalSession(caller);
}

int authenticateFace(pam_handle_t *pamh, const Config &config)
{
    if (config.boolean("core/disabled", false)) {
        return PAM_AUTHINFO_UNAVAIL;
    }
    if (config.boolean("core/abort_if_ssh", true) && remoteSession(pamh)) {
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

// The lock screen's answer to fingerprintStartPrompt; blocks forever for Plasma's unasked one.
bool fingerprintStartRequested(pam_handle_t *pamh)
{
    char *response = nullptr;
    const bool requested =
        pam_prompt(pamh, PAM_PROMPT_ECHO_ON, &response, "%s", fingerprintStartPrompt) == PAM_SUCCESS && response && std::strcmp(response, "start") == 0;
    std::free(response);
    return requested;
}

volatile std::sig_atomic_t interruptSeen = 0;

void onInterrupt(int)
{
    interruptSeen = 1;
}

// Ctrl+C while the reader waits for a finger in a terminal: on to the password, as with
// pam_fprintd, rather than the end of sudo. Catches SIGINT for as long as it lives.
class InterruptWatch
{
public:
    InterruptWatch()
    {
        interruptSeen = 0;
        struct sigaction action{};
        action.sa_handler = onInterrupt;
        sigemptyset(&action.sa_mask);
        m_installed = sigaction(SIGINT, &action, &m_previous) == 0;
    }
    InterruptWatch(const InterruptWatch &) = delete;
    InterruptWatch &operator=(const InterruptWatch &) = delete;
    ~InterruptWatch()
    {
        if (m_installed) {
            sigaction(SIGINT, &m_previous, nullptr);
        }
    }

    bool interrupted()
    {
        if (interruptSeen) {
            m_interrupted = true;
        } else if (sigset_t pending; sigpending(&pending) == 0 && sigismember(&pending, SIGINT) == 1) {
            // A program that holds the signal back while PAM runs: take it, or it would end
            // the program once let through.
            sigset_t interrupt;
            sigemptyset(&interrupt);
            sigaddset(&interrupt, SIGINT);
            const timespec noWait{};
            sigtimedwait(&interrupt, nullptr, &noWait);
            m_interrupted = true;
        }
        return m_interrupted;
    }

private:
    struct sigaction m_previous{};
    bool m_installed = false;
    bool m_interrupted = false;
};

bool hasTerminal(pam_handle_t *pamh)
{
    const void *tty = nullptr;
    return pam_get_item(pamh, PAM_TTY, &tty) == PAM_SUCCESS && tty && *static_cast<const char *>(tty);
}

int authenticateFingerprint(pam_handle_t *pamh, const Config &config, FingerprintStart start)
{
    // fprintd reads whoever touches the reader here, as the camera sees whoever sits here.
    if (config.boolean("core/abort_if_ssh", true) && remoteSession(pamh)) {
        return PAM_AUTHINFO_UNAVAIL;
    }
    const char *user = nullptr;
    if (pam_get_user(pamh, &user, nullptr) != PAM_SUCCESS || !user || !*user) {
        return PAM_USER_UNKNOWN;
    }
    if (start == FingerprintStart::WhenAsked && !fingerprintStartRequested(pamh)) {
        return PAM_AUTHINFO_UNAVAIL;
    }
    const bool terminal = start == FingerprintStart::Terminal;
    if (terminal) {
        // A password piped in (sudo -S) or asked for by a program: nobody would see the request
        // for a finger, and the password would wait for the reader to give up.
        if (!hasTerminal(pamh)) {
            return PAM_AUTHINFO_UNAVAIL;
        }
        // Asked first, so a user without a fingerprint, or locked out, goes straight to the password.
        const int has = FaceUnlock::askService("has-fingerprint", user, FaceUnlock::quickTimeoutSeconds);
        if (has == FaceUnlock::LockedOut) {
            pam_error(pamh, "%s", "Too many fingerprint attempts; use your password");
        }
        if (has != FaceUnlock::Recognized) {
            return PAM_AUTHINFO_UNAVAIL;
        }
    }

    std::optional<InterruptWatch> interrupt;
    if (terminal) {
        interrupt.emplace();
    }
    bool hinted = false;
    const FaceUnlock::Reply reply = FaceUnlock::callService({
        .command = "fingerprint",
        .user = user,
        .argument = {},
        .idleTimeoutSeconds = FaceUnlock::fingerprintTimeoutSeconds(config),
        .onMessage =
            [&](FaceUnlock::MessageKind kind, const std::string &text) {
                if (kind != FaceUnlock::MessageKind::Info) {
                    pam_error(pamh, "%s", text.c_str());
                } else if (terminal && !hinted) {
                    // The first line asks for the finger.
                    hinted = true;
                    pam_info(pamh, "%s (Ctrl+C to use your password)", text.c_str());
                } else {
                    pam_info(pamh, "%s", text.c_str());
                }
            },
        .cancelled =
            [&] {
                return interrupt && interrupt->interrupted();
            },
    });
    if (interrupt && interrupt->interrupted()) {
        pam_syslog(pamh, LOG_INFO, "Fingerprint skipped with Ctrl+C");
        return PAM_AUTHINFO_UNAVAIL;
    }
    switch (reply.code) {
    case FaceUnlock::Recognized:
        pam_syslog(pamh, LOG_INFO, "Login approved by fingerprint");
        return PAM_SUCCESS;
    case -1:
        pam_syslog(pamh, LOG_ERR, "Face unlock service not reachable at %s", FaceUnlock::socketPath);
        return PAM_AUTHINFO_UNAVAIL;
    case FaceUnlock::NoReader:
    case FaceUnlock::NoModel:
        return PAM_AUTHINFO_UNAVAIL;
    default:
        pam_syslog(pamh, LOG_NOTICE, "Fingerprint not recognized (code %d)", reply.code);
        return PAM_AUTH_ERR;
    }
}

// Only asks for the token while the service holds one for this user, so other polkit dialogs
// (pkexec's in a terminal) never see the question.
int authenticateGrant(pam_handle_t *pamh)
{
    const char *user = nullptr;
    if (pam_get_user(pamh, &user, nullptr) != PAM_SUCCESS || !user || !*user) {
        return PAM_USER_UNKNOWN;
    }
    if (FaceUnlock::askService("has-grant", user, FaceUnlock::quickTimeoutSeconds) != FaceUnlock::Recognized) {
        return PAM_AUTHINFO_UNAVAIL;
    }
    char *token = nullptr;
    if (pam_prompt(pamh, PAM_PROMPT_ECHO_OFF, &token, "%s", FaceUnlock::grantPrompt) != PAM_SUCCESS || !token) {
        return PAM_AUTHINFO_UNAVAIL;
    }
    const std::string answer = token;
    std::free(token);
    const FaceUnlock::Reply reply = FaceUnlock::callService({
        .command = "use-grant",
        .user = user,
        .argument = answer,
        .idleTimeoutSeconds = FaceUnlock::quickTimeoutSeconds,
    });
    if (reply.code != FaceUnlock::Recognized) {
        return PAM_AUTHINFO_UNAVAIL;
    }
    pam_syslog(pamh, LOG_INFO, "Login approved by Kenning's polkit dialog");
    return PAM_SUCCESS;
}
}

extern "C" {

PAM_EXTERN int pam_sm_authenticate(pam_handle_t *pamh, int, int argc, const char **argv)
{
    const Options options = parseOptions(pamh, argc, argv);
    if (options.mode == Mode::Grant) {
        return authenticateGrant(pamh);
    }
    // Set by the login manager: a typed password, or the button the user chose.
    const char *method = pam_getenv(pamh, "KENNING_METHOD");
    if (const char *skip = pam_getenv(pamh, "FACEUNLOCK_SKIP"); (skip && std::strcmp(skip, "1") == 0) || (method && std::strcmp(method, "password") == 0)) {
        return PAM_AUTHINFO_UNAVAIL;
    }

    const Config config;
    if (options.mode == Mode::Fingerprint) {
        return authenticateFingerprint(pamh, config, options.explicitStart ? FingerprintStart::WhenAsked : FingerprintStart::Terminal);
    }
    if (method && std::strcmp(method, "fingerprint") == 0) {
        return authenticateFingerprint(pamh, config, FingerprintStart::Now);
    }
    return authenticateFace(pamh, config);
}

PAM_EXTERN int pam_sm_setcred(pam_handle_t *, int, int, const char **)
{
    return PAM_IGNORE;
}
}
