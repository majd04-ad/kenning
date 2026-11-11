/*
    SPDX-FileCopyrightText: 2026 Rishon Jonathan R
    SPDX-License-Identifier: GPL-2.0-or-later
*/
// kenning daemon: face checks for pam_kenning, so the face models stay root-only.

#include "daemon.h"
#include "camera.h"
#include "engine.h"
#include "failurelimiter.h"
#include "fingerprint.h"
#include "grants.h"
#include "service.h"
#include "signedin.h"
#include "turnqueue.h"

#include <atomic>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <optional>
#include <poll.h>
#include <pwd.h>
#include <sys/stat.h>
#include <systemd/sd-login.h>
#include <thread>
#include <vector>

namespace FaceUnlock
{
namespace
{
// Socket activated: exits after this long without a request.
constexpr int idleExitMs = 5 * 60 * 1000;
// A client that connects and says nothing must not hold up everyone else.
constexpr int clientTimeoutSeconds = 2;
constexpr std::size_t maxRequestLength = 255;
// Each connection has a thread; more than this at once are turned away.
constexpr int maxConnections = 32;
// In /run like pam_faillock's records: kept across service restarts, cleared at reboot.
constexpr const char *failuresPath = "/run/kenning/failures";
constexpr const char *fingerprintFailuresPath = "/run/kenning/fingerprint-failures";
constexpr const char *signedInPath = "/run/kenning/signed-in";

void sendLine(int client, const std::string &line)
{
    const std::string text = line + '\n';
    send(client, text.data(), text.size(), MSG_NOSIGNAL);
}

void reply(int client, int code)
{
    sendLine(client, std::to_string(code));
}

// A progress line for the client to show (or to pass on as a PAM message).
void say(int client, const std::string &text, MessageKind kind = MessageKind::Info)
{
    std::string line = text;
    std::erase(line, '\n');
    sendLine(client, (kind == MessageKind::Info ? "# " : "! ") + line);
}

// A failure limiter whose records survive a restart of the service, until the next reboot.
class KeptLimiter
{
public:
    explicit KeptLimiter(const char *path)
        : m_path(path)
    {
        std::ifstream file(m_path);
        limiter.restore(std::string(std::istreambuf_iterator<char>(file), {}));
    }

    void save()
    {
        const std::string temporary = std::string(m_path) + ".new";
        std::ofstream(temporary) << limiter.save();
        std::rename(temporary.c_str(), m_path);
    }

    FailureLimiter limiter;

private:
    const char *m_path;
};

std::optional<Request> receiveRequest(int client)
{
    char buffer[maxRequestLength + 1] = {};
    size_t received = 0;
    while (received < maxRequestLength && !std::memchr(buffer, '\n', received)) {
        const ssize_t count = recv(client, buffer + received, maxRequestLength - received, 0);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return std::nullopt;
        }
        received += size_t(count);
    }
    return parseRequest(std::string_view(buffer, received));
}

// The user's uid. getpwnam_r: each connection has a thread, and getpwnam's answer is shared by all.
std::optional<uid_t> userId(const std::string &user)
{
    if (user.find('/') != std::string::npos) {
        return std::nullopt;
    }
    passwd entry{};
    passwd *found = nullptr;
    std::vector<char> buffer(16 * 1024);
    while (getpwnam_r(user.c_str(), &entry, buffer.data(), buffer.size(), &found) == ERANGE && buffer.size() < 1024 * 1024) {
        buffer.resize(buffer.size() * 2);
    }
    return found ? std::optional(found->pw_uid) : std::nullopt;
}

// A local login of this user: the login screen refuses face unlock until there is one,
// so it was made with a password (or fingerprint, or autologin).
bool hasLocalLogin(uid_t uid)
{
    char **sessions = nullptr;
    const int count = sd_uid_get_sessions(uid, 0, &sessions);
    bool found = false;
    for (int i = 0; i < count; ++i) {
        char *sessionClass = nullptr;
        if (!found && sd_session_is_remote(sessions[i]) == 0 && sd_session_get_class(sessions[i], &sessionClass) >= 0) {
            found = std::strcmp(sessionClass, "user") == 0;
        }
        std::free(sessionClass);
        std::free(sessions[i]);
    }
    std::free(sessions);
    return found;
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

// The asking program runs at this computer: in a local session, or, outside any session (the
// polkit dialog runs under the user's service manager), for a user signed in here.
bool peerAtComputer(pid_t pid, uid_t uid)
{
    char *session = nullptr;
    if (sd_pid_get_session(pid, &session) >= 0 && session) {
        const bool local = sd_session_is_remote(session) == 0;
        std::free(session);
        return local;
    }
    return hasLocalSession(uid);
}

bool clientHungUp(int client)
{
    pollfd state{client, POLLRDHUP, 0};
    return poll(&state, 1, 0) > 0 && (state.revents & (POLLRDHUP | POLLHUP | POLLERR));
}

// The listening socket from systemd (LISTEN_FDS, fd 3), or -1.
int activatedSocket()
{
    const char *pid = std::getenv("LISTEN_PID");
    const char *fds = std::getenv("LISTEN_FDS");
    if (pid && fds && std::atoi(pid) == getpid() && std::atoi(fds) >= 1) {
        return 3;
    }
    return -1;
}

// For running by hand: create the socket ourselves.
int ownSocket()
{
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, socketPath, sizeof(address.sun_path) - 1);
    unlink(socketPath);
    if (fd < 0 || bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 || listen(fd, 8) != 0) {
        return -1;
    }
    chmod(socketPath, 0666);
    return fd;
}

class Service
{
public:
    Service()
    {
        std::ifstream file(signedInPath);
        m_signedIn.restore(std::string(std::istreambuf_iterator<char>(file), {}));
    }

    void serve(int client)
    {
        const timeval timeout{clientTimeoutSeconds, 0};
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        ucred peer{};
        socklen_t peerSize = sizeof(peer);
        if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &peer, &peerSize) != 0) {
            return;
        }
        const std::optional<Request> request = receiveRequest(client);
        if (!request) {
            return reply(client, Aborted);
        }
        const std::optional<uid_t> account = userId(request->user);
        if (!account) {
            return reply(client, Aborted);
        }
        const uid_t uid = *account;
        // Set Up, through the settings helper or sudo kenning add: an administrator already agreed.
        if (request->command == "enroll") {
            return reply(client, peer.uid == 0 ? enrollFace(client, request->user, request->argument) : Aborted);
        }
        const bool needsPassword = Config().boolean("core/password_after_boot", true) && !signedInSinceBoot(uid);
        // Asked by the login screen so it can skip the face scan after a restart. Open to anyone:
        // who has signed in since boot is public already (last, loginctl).
        if (request->command == "password-first") {
            return reply(client, needsPassword ? NeedsPassword : Recognized);
        }
        // polkit's helper, through pam_kenning.so grant.
        if (request->command == "has-grant" || request->command == "use-grant") {
            if (peer.uid != 0) {
                return reply(client, Aborted);
            }
            const std::lock_guard lock(m_state);
            const auto now = GrantStore::Clock::now();
            if (request->command == "has-grant") {
                return reply(client, m_grants.pending(uid, now) ? Recognized : NoModel);
            }
            return reply(client, m_grants.use(uid, request->argument, now) ? Recognized : Aborted);
        }
        // Kenning's polkit dialog, for the user's own account only.
        if (request->command == "grant") {
            if (peer.uid != uid) {
                return reply(client, Aborted);
            }
            return grant(client, request->user, uid, peer.pid, request->argument, needsPassword);
        }

        // Only root, or the user asking about their own face.
        if (peer.uid != 0 && peer.uid != uid) {
            return reply(client, Aborted);
        }

        // Fingerprints don't wait for a password after a restart: the first sign-in may use one.
        if (request->command == "has-fingerprint") {
            return reply(client, hasFingerprint(request->user, uid));
        }
        if (request->command == "fingerprint") {
            return reply(client, verifyFinger(client, request->user, uid));
        }

        if (request->command == "has-model") {
            // Without a face model there is nothing a password would bring back.
            const int model = hasModel(request->user, uid);
            return reply(client, needsPassword && model != NoModel ? NeedsPassword : model);
        }
        if (needsPassword) {
            return reply(client, NeedsPassword);
        }
        if (request->command == "compare") {
            return reply(client, compareFace(client, request->user, uid));
        }
        reply(client, Aborted);
    }

    bool canExit()
    {
        // Exiting would forget failed attempts and lockouts.
        const std::lock_guard lock(m_state);
        const auto now = FailureLimiter::Clock::now();
        return m_failures.limiter.isEmpty(now) && m_fingerprintFailures.limiter.isEmpty(now) && m_grants.isEmpty(GrantStore::Clock::now());
    }

private:
    bool signedInSinceBoot(uid_t uid)
    {
        const std::lock_guard lock(m_state);
        if (m_signedIn.contains(uid)) {
            return true;
        }
        if (!hasLocalLogin(uid)) {
            return false;
        }
        // Remembered so face unlock still works on the login screen after signing out.
        m_signedIn.add(uid);
        const std::string temporary = std::string(signedInPath) + ".new";
        std::ofstream(temporary) << m_signedIn.save();
        std::rename(temporary.c_str(), signedInPath);
        return true;
    }

    int hasModel(const std::string &user, uid_t uid)
    {
        // Without the error code, a models directory the service can't read throws and ends the service.
        std::error_code error;
        if (!std::filesystem::exists(modelPath(user), error)) {
            return NoModel;
        }
        const Config config;
        {
            const std::lock_guard lock(m_state);
            if (m_failures.limiter.isLockedOut(uid, FailureLimits::fromConfig(config), FailureLimiter::Clock::now())) {
                return LockedOut;
            }
        }
        // Switched off or unplugged: said before a scan starts, so nothing looks for a face in vain.
        return cameraFor(config) ? Recognized : NoCamera;
    }

    int compareFace(int client, const std::string &user, uid_t uid)
    {
        // Read per request so settings changes apply without a restart.
        const Config config;
        const FailureLimits limits = FailureLimits::fromConfig(config);
        if (isLockedOut(uid, limits)) {
            return LockedOut;
        }
        const auto hungUp = [client] {
            return clientHungUp(client);
        };
        // One scan at a time; a user whose request is already waiting can't add another. The
        // patience covers a scan that was just cancelled, e.g. switching methods in the polkit dialog.
        constexpr auto patience = std::chrono::seconds(1);
        const std::optional<TurnQueue::Turn> turn = m_camera.take(uid, hungUp, patience);
        if (!turn) {
            return Aborted;
        }
        // Another request may have locked the user out while this one waited.
        if (isLockedOut(uid, limits)) {
            return LockedOut;
        }
        // A broken install must answer "failed", so PAM falls back to the password.
        try {
            const ScanResult result = compare(config, user, hungUp);
            recordResult(user, uid, result, limits);
            logScan(user, result);
            return result.code;
        } catch (const std::exception &error) {
            std::cerr << "kenning daemon: scan failed: " << error.what() << std::endl;
            return Error;
        }
    }

    // One line per scan that didn't recognize anyone, for the journal: why face unlock didn't work.
    static void logScan(const std::string &user, const ScanResult &result)
    {
        if (result.code == Recognized || result.code == Aborted) {
            return;
        }
        const ScanStats &stats = result.stats;
        std::cerr << "kenning daemon: scan for " << user << ": " << result.message << " (code " << result.code << "; " << stats.frames << " frames, "
                  << stats.darkFrames << " too dark, " << stats.brightFrames << " too bright, " << stats.faceFrames << " with a face";
        if (result.bestScore < noMatchScore) {
            std::cerr << ", best score " << result.bestScore;
        }
        if (result.code == NoCamera) {
            const Config config;
            std::cerr << "; camera " << (cameraConfigured(config) ? config.string("video/device_path") : std::string("not set up, and no IR camera found"));
        }
        std::cerr << ")" << std::endl;
    }

    // A finger or face for an administrator prompt, answered "0 <token>" on a match.
    void grant(int client, const std::string &user, uid_t uid, pid_t pid, const std::string &method, bool needsPassword)
    {
        const Config config;
        if ((method != "face" && method != "fingerprint") || !config.boolean("polkit/" + method, false)) {
            return reply(client, Aborted);
        }
        // As for sudo: the camera and the reader answer for whoever is at this computer.
        if (config.boolean("core/abort_if_ssh", true) && !peerAtComputer(pid, uid)) {
            return reply(client, Aborted);
        }
        int code = Aborted;
        if (method == "fingerprint") {
            code = verifyFinger(client, user, uid);
        } else if (config.boolean("core/disabled", false)) {
            code = Aborted;
        } else if (needsPassword) {
            code = NeedsPassword;
        } else if (const int model = hasModel(user, uid); model != Recognized) {
            code = model;
        } else {
            code = compareFace(client, user, uid);
        }
        if (code != Recognized) {
            return reply(client, code);
        }
        std::string token;
        {
            const std::lock_guard lock(m_state);
            token = m_grants.issue(uid, GrantStore::Clock::now());
        }
        if (token.empty()) {
            return reply(client, Error);
        }
        sendLine(client, std::to_string(Recognized) + ' ' + token);
    }

    // Captures a face here, in the sandbox, so OpenCV never handles camera frames as root.
    int enrollFace(int client, const std::string &user, const std::string &label)
    {
        const auto hungUp = [client] {
            return clientHungUp(client);
        };
        const std::optional<TurnQueue::Turn> turn = m_camera.take(0, hungUp);
        if (!turn) {
            return Aborted;
        }
        constexpr std::size_t manyModels = 3;
        std::vector<FaceModel> models = loadModels(user);
        if (models.size() > manyModels) {
            say(client, "NOTICE: Each additional model slows down the face recognition engine slightly");
        }
        const int nextId = models.empty() ? 0 : models.back().id + 1;
        FaceModel model{nextId, faceLabel(label, nextId), std::time(nullptr), {}};
        try {
            const ScanResult result = enroll(
                Config(),
                model.encodings,
                [client](const std::string &line) {
                    say(client, line);
                },
                hungUp);
            if (result.code != Recognized) {
                say(client, result.message);
                if (result.code != Aborted) {
                    std::cerr << "kenning daemon: enrolment for " << user << ": " << result.message << std::endl;
                }
                return result.code == NoCamera ? NoCamera : Error;
            }
        } catch (const std::exception &error) {
            std::cerr << "kenning daemon: enrolment failed: " << error.what() << std::endl;
            say(client, "Face recognition failed to start");
            return Error;
        }
        models.push_back(model);
        if (!saveModels(user, models)) {
            say(client, "Could not save the face model");
            return Error;
        }
        say(client, "Face model saved as \"" + model.label + "\" (ID " + std::to_string(nextId) + ")");
        return Recognized;
    }

    bool isLockedOut(uid_t uid, const FailureLimits &limits)
    {
        const std::lock_guard lock(m_state);
        return m_failures.limiter.isLockedOut(uid, limits, FailureLimiter::Clock::now());
    }

    int hasFingerprint(const std::string &user, uid_t uid)
    {
        std::string message;
        const ExitCode code = enrolledFingerprints(user, message);
        const std::lock_guard lock(m_state);
        if (code == Recognized && m_fingerprintFailures.limiter.isLockedOut(uid, FailureLimits::fromConfig(Config()), FailureLimiter::Clock::now())) {
            return LockedOut;
        }
        return code;
    }

    // Asks fprintd for the user's finger, telling the client what to do as the reader asks.
    int verifyFinger(int client, const std::string &user, uid_t uid)
    {
        const Config config;
        const FailureLimits limits = FailureLimits::fromConfig(config);
        const auto lockedOut = [&] {
            const std::lock_guard lock(m_state);
            return m_fingerprintFailures.limiter.isLockedOut(uid, limits, FailureLimiter::Clock::now());
        };
        if (lockedOut()) {
            say(client, "Too many fingerprint attempts; use your password", MessageKind::Error);
            return LockedOut;
        }
        const auto hungUp = [client] {
            return clientHungUp(client);
        };
        constexpr auto patience = std::chrono::seconds(1);
        const std::optional<TurnQueue::Turn> turn = m_reader.take(uid, hungUp, patience);
        if (!turn) {
            say(client, "The fingerprint reader is in use", MessageKind::Error);
            return Aborted;
        }
        if (lockedOut()) {
            say(client, "Too many fingerprint attempts; use your password", MessageKind::Error);
            return LockedOut;
        }
        const FingerprintOptions options{
            .timeout = std::chrono::seconds(std::max(1, int(config.number("fingerprint/timeout", 30)))),
            .maxTries = std::max(1, int(config.number("fingerprint/max_tries", 3))),
        };
        std::string message;
        const ExitCode code = verifyFingerprint(user,
                                                options,
                                                {
                                                    .say =
                                                        [client](const std::string &text) {
                                                            say(client, text);
                                                        },
                                                    .cancelled = hungUp,
                                                    .noMatch =
                                                        [&] {
                                                            return !recordFailure(m_fingerprintFailures, user, uid, limits, "fingers");
                                                        },
                                                },
                                                message);
        if (code == Recognized) {
            recordSuccess(m_fingerprintFailures, uid);
        } else if (!message.empty() && code != Aborted) {
            say(client, message, MessageKind::Error);
        }
        return code;
    }

    void recordSuccess(KeptLimiter &failures, uid_t uid)
    {
        const std::lock_guard lock(m_state);
        failures.limiter.recordSuccess(uid);
        failures.save();
    }

    // Returns true when this failure locked the user out.
    bool recordFailure(KeptLimiter &failures, const std::string &user, uid_t uid, const FailureLimits &limits, const char *what)
    {
        const std::lock_guard lock(m_state);
        const bool lockedOut = failures.limiter.recordFailure(uid, limits, FailureLimiter::Clock::now());
        if (lockedOut) {
            std::cerr << "kenning daemon: unlocking with " << what << " locked for " << user << " for " << limits.lockout.count() << " minutes after "
                      << limits.maxFailures << " unrecognized " << what << "\n";
        }
        failures.save();
        return lockedOut;
    }

    void recordResult(const std::string &user, uid_t uid, const ScanResult &result, const FailureLimits &limits)
    {
        if (result.code == Recognized) {
            recordSuccess(m_failures, uid);
        } else if (result.code == Timeout && result.stats.faceFrames > 0) {
            // Only a face that was seen and not recognized counts.
            recordFailure(m_failures, user, uid, limits, "faces");
        }
    }

    // Guards the limiters, m_signedIn and m_grants: each connection is served in a thread of its own.
    std::mutex m_state;
    KeptLimiter m_failures{failuresPath};
    KeptLimiter m_fingerprintFailures{fingerprintFailuresPath};
    SignedInUsers m_signedIn;
    GrantStore m_grants;
    TurnQueue m_camera;
    TurnQueue m_reader;
};
}

int runDaemon()
{
    int listener = activatedSocket();
    const bool activated = listener >= 0;
    if (!activated) {
        listener = ownSocket();
    }
    if (listener < 0) {
        std::cerr << "kenning daemon: can't listen on " << socketPath << '\n';
        return Error;
    }

    // OpenCV waits this many seconds for a frame (10 unless set): a camera that stops delivering
    // must not hold a scan, and everyone queued behind it, for long past video/timeout.
    setenv("OPENCV_VIDEOIO_V4L_SELECT_TIMEOUT", "3", 0);

    Service service;
    std::atomic<int> connections = 0;
    for (;;) {
        pollfd waiting{listener, POLLIN, 0};
        const int ready = poll(&waiting, 1, activated ? idleExitMs : -1);
        if (ready == 0) {
            if (connections == 0 && service.canExit()) {
                return 0;
            }
            continue;
        }
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return Error;
        }
        const int client = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0) {
            continue;
        }
        // A scan takes seconds; questions such as has-model must not wait for it.
        if (++connections > maxConnections) {
            --connections;
            reply(client, Aborted);
            close(client);
            continue;
        }
        std::thread([&service, &connections, client] {
            // Nothing a request does may end the service for everyone else.
            try {
                service.serve(client);
            } catch (const std::exception &error) {
                std::cerr << "kenning daemon: request failed: " << error.what() << std::endl;
                reply(client, Error);
            } catch (...) {
                std::cerr << "kenning daemon: request failed" << std::endl;
                reply(client, Error);
            }
            close(client);
            --connections;
        }).detach();
    }
}
}
