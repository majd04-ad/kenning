/*
    SPDX-FileCopyrightText: 2026 Rishon Jonathan R
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

// The service (kenning daemon) owns the camera and the face models. One request per connection:
// "<command> <user>[ <argument>]\n". The service may first send progress lines, "# <text>\n" to
// show or "! <text>\n" as an error, and then answers "<ExitCode>[ <data>]\n". It answers only
// for root or the user's own account; daemon.cpp has who may ask what. Header-only, for
// pam_kenning.

#include "config.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

namespace FaceUnlock
{
#ifndef KENNING_SOCKET
#define KENNING_SOCKET "/run/kenning.sock"
#endif
inline constexpr const char *socketPath = KENNING_SOCKET;

// For has-model, which answers at once.
inline constexpr int quickTimeoutSeconds = 5;

// For compare: requests are served one at a time, so allow for a scan already running.
inline int scanTimeoutSeconds(const Config &config)
{
    return int(config.number("video/timeout", 4)) * 2 + 10;
}

struct Request {
    std::string command;
    std::string user;
    std::string argument; // the rest of the line, e.g. a face label; may be empty
};

inline std::optional<Request> parseRequest(std::string_view text)
{
    const auto end = text.find('\n');
    const auto space = text.find(' ');
    if (end == std::string_view::npos || space == std::string_view::npos || space > end) {
        return std::nullopt;
    }
    const std::string_view rest = text.substr(space + 1, end - space - 1);
    const auto argumentSpace = rest.find(' ');
    Request request{std::string(text.substr(0, space)), std::string(rest.substr(0, argumentSpace)), {}};
    if (argumentSpace != std::string_view::npos) {
        request.argument = std::string(rest.substr(argumentSpace + 1));
    }
    if (request.command.empty() || request.user.empty()) {
        return std::nullopt;
    }
    return request;
}

// What the service answered: the code, and any data after it.
struct Reply {
    int code = -1; // -1 when the service can't be reached or hung up
    std::string data;
};

enum class MessageKind {
    Info, // "# "
    Error, // "! "
};

// Returns true and fills reply for the final line, or false for a progress line (calling onMessage).
inline bool parseReplyLine(std::string_view line, Reply &reply, const std::function<void(MessageKind, const std::string &)> &onMessage)
{
    if (line.size() >= 2 && (line[0] == '#' || line[0] == '!') && line[1] == ' ') {
        if (onMessage) {
            onMessage(line[0] == '#' ? MessageKind::Info : MessageKind::Error, std::string(line.substr(2)));
        }
        return false;
    }
    const auto space = line.find(' ');
    const std::string code(line.substr(0, space));
    char *end = nullptr;
    const long value = std::strtol(code.c_str(), &end, 10);
    reply.code = code.empty() || *end ? -1 : int(value);
    reply.data = space == std::string_view::npos ? std::string() : std::string(line.substr(space + 1));
    return true;
}

struct Call {
    std::string command;
    std::string user;
    std::string argument;
    int idleTimeoutSeconds = quickTimeoutSeconds; // longest wait for the next line
    std::function<void(MessageKind, const std::string &)> onMessage = {};
    std::function<bool()> cancelled = {}; // checked while waiting; true hangs up
};

inline Reply callService(const Call &call)
{
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return {};
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, socketPath, sizeof(address.sun_path) - 1);
    // Short slices, so cancelled() is checked while a long scan runs.
    constexpr int sliceMs = 250;
    const timeval slice{0, sliceMs * 1000};
    const timeval sendTimeout{quickTimeoutSeconds, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &slice, sizeof(slice));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &sendTimeout, sizeof(sendTimeout));
    if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        close(fd);
        return {};
    }

    std::string request = call.command + ' ' + call.user;
    if (!call.argument.empty()) {
        request += ' ' + call.argument;
    }
    request += '\n';
    if (send(fd, request.data(), request.size(), MSG_NOSIGNAL) != ssize_t(request.size())) {
        close(fd);
        return {};
    }

    Reply reply;
    std::string buffer;
    int idleMs = 0;
    for (;;) {
        char chunk[512];
        const ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && errno == EAGAIN) { // the receive timeout; EWOULDBLOCK is the same on Linux
            idleMs += sliceMs;
            if (idleMs >= call.idleTimeoutSeconds * 1000 || (call.cancelled && call.cancelled())) {
                break;
            }
            continue;
        }
        if (n <= 0) {
            break;
        }
        idleMs = 0;
        buffer.append(chunk, size_t(n));
        bool finished = false;
        for (auto newline = buffer.find('\n'); newline != std::string::npos && !finished; newline = buffer.find('\n')) {
            finished = parseReplyLine(std::string_view(buffer).substr(0, newline), reply, call.onMessage);
            buffer.erase(0, newline + 1);
        }
        if (finished) {
            close(fd);
            return reply;
        }
        // A line longer than any the service sends: not the service.
        if (buffer.size() > 4096) {
            break;
        }
    }
    close(fd);
    return {};
}

// Returns the reply code, or -1 when the service can't be reached.
inline int askService(const std::string &command, const std::string &user, int timeoutSeconds)
{
    return callService({.command = command, .user = user, .argument = {}, .idleTimeoutSeconds = timeoutSeconds}).code;
}
}
