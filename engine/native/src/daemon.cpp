/*
    SPDX-FileCopyrightText: 2026 Rishon Jonathan R
    SPDX-License-Identifier: GPL-2.0-or-later
*/
// kenning daemon: face checks for pam_kenning, so the face models stay root-only.

#include "daemon.h"
#include "camera.h"
#include "engine.h"
#include "failurelimiter.h"
#include "service.h"
#include "signedin.h"
#include "turnqueue.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
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
// A request must arrive whole within this time, however slowly it trickles in. Each
// connection has a thread, so a slow client only holds up itself.
constexpr auto requestDeadline = std::chrono::seconds(2);
constexpr int replyTimeoutSeconds = 2;
constexpr std::size_t maxRequestLength = 255;
// Each connection has a thread; more than this at once are turned away.
constexpr int maxConnections = 32;
// Connections of one user still sending their request, so a slow sender can't take them all.
constexpr std::size_t maxArrivingPerUser = 4;
// In /run like pam_faillock's records: kept across service restarts, cleared at reboot.
constexpr const char *failuresPath = "/run/kenning/failures";
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

using Clock = std::chrono::steady_clock;

// The request line, which must arrive whole before the deadline however slowly it trickles in.
std::optional<Request> receiveRequest(int client, Clock::time_point deadline)
{
    char buffer[maxRequestLength] = {};
    std::size_t received = 0;
    while (received < maxRequestLength && !std::memchr(buffer, '\n', received)) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (left <= 0) {
            return std::nullopt;
        }
        pollfd state{client, POLLIN, 0};
        const int ready = poll(&state, 1, int(left));
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready <= 0) {
            return std::nullopt;
        }
        const ssize_t count = recv(client, buffer + received, maxRequestLength - received, MSG_DONTWAIT);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        if (count <= 0) {
            return std::nullopt;
        }
        received += std::size_t(count);
    }
    return parseRequest(std::string_view(buffer, received));
}

// Connections still sending their request, per user.
class Arrivals
{
public:
    bool enter(uid_t uid)
    {
        const std::lock_guard lock(m_mutex);
        std::size_t &count = m_counts[uid];
        if (count >= maxArrivingPerUser) {
            return false;
        }
        ++count;
        return true;
    }

    void leave(uid_t uid)
    {
        const std::lock_guard lock(m_mutex);
        if (--m_counts[uid] == 0) {
            m_counts.erase(uid);
        }
    }

private:
    std::mutex m_mutex;
    std::map<uid_t, std::size_t> m_counts;
};

class Service
{
public:
    Service()
    {
        restoreFailures();
        std::ifstream file(signedInPath);
        m_signedIn.restore(std::string(std::istreambuf_iterator<char>(file), {}));
    }

    // The request has been read, or ran out of time.
    void serve(int client, const ucred &peer, const std::optional<Request> &request)
    {
        const timeval timeout{replyTimeoutSeconds, 0};
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
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
        // Only root, or the user asking about their own face.
        if (peer.uid != 0 && peer.uid != uid) {
            return reply(client, Aborted);
        }
        // From pam_kenning when PAM ends: a typed password was wrong. Counted even before the
        // first sign-in after a restart, as pam_faillock would.
        if (request->command == "password-failed") {
            recordFailure(request->user, uid, FailureLimiter::Kind::Password, FailureLimits::fromConfig(Config()));
            return reply(client, Recognized);
        }

        if (needsPassword) {
            return reply(client, NeedsPassword);
        }
        if (request->command == "has-model") {
            return reply(client, hasModel(request->user, uid));
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
        return m_failures.isEmpty(FailureLimiter::Clock::now());
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
            if (m_failures.isLockedOut(uid, FailureLimits::fromConfig(config), FailureLimiter::Clock::now())) {
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
        // One scan at a time; a user whose request is already waiting can't add another.
        const std::optional<TurnQueue::Turn> turn = m_camera.take(uid, hungUp);
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
        return m_failures.isLockedOut(uid, limits, FailureLimiter::Clock::now());
    }

    void restoreFailures()
    {
        std::ifstream file(failuresPath);
        m_failures.restore(std::string(std::istreambuf_iterator<char>(file), {}));
    }

    void saveFailures()
    {
        const std::string temporary = std::string(failuresPath) + ".new";
        std::ofstream(temporary) << m_failures.save();
        std::rename(temporary.c_str(), failuresPath);
    }

    void recordResult(const std::string &user, uid_t uid, const ScanResult &result, const FailureLimits &limits)
    {
        if (result.code == Recognized) {
            const std::lock_guard lock(m_state);
            m_failures.recordSuccess(uid);
            saveFailures();
            return;
        }
        // A face that was seen and not recognized counts, however the scan ended: a client
        // that hangs up just before the timeout must not get unlimited tries.
        if (result.stats.faceFrames > 0) {
            recordFailure(user, uid, FailureLimiter::Kind::Face, limits);
        }
    }

    void recordFailure(const std::string &user, uid_t uid, FailureLimiter::Kind kind, const FailureLimits &limits)
    {
        const std::lock_guard lock(m_state);
        if (m_failures.recordFailure(uid, kind, limits, FailureLimiter::Clock::now())) {
            std::cerr << "kenning daemon: face unlock locked for " << user << " for " << limits.lockout.count() << " minutes after " << limits.maxFailures
                      << " unrecognized faces and " << limits.maxPasswordFailures << " wrong passwords\n";
        }
        saveFailures();
    }

    // Guards m_failures and m_signedIn: each connection is served in a thread of its own.
    std::mutex m_state;
    FailureLimiter m_failures;
    SignedInUsers m_signedIn;
    TurnQueue m_camera;
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
    Arrivals arriving;
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
        const auto deadline = Clock::now() + requestDeadline;
        ucred peer{};
        socklen_t peerSize = sizeof(peer);
        if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &peer, &peerSize) != 0 || !arriving.enter(peer.uid)) {
            close(client);
            continue;
        }
        // A scan takes seconds; questions such as has-model must not wait for it.
        if (++connections > maxConnections) {
            --connections;
            arriving.leave(peer.uid);
            reply(client, Aborted);
            close(client);
            continue;
        }
        std::thread([&service, &arriving, &connections, client, peer, deadline] {
            const std::optional<Request> request = receiveRequest(client, deadline);
            arriving.leave(peer.uid);
            // Nothing a request does may end the service for everyone else.
            try {
                service.serve(client, peer, request);
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
