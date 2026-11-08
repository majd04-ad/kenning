/*
    SPDX-FileCopyrightText: 2026 Rishon Jonathan R
    SPDX-License-Identifier: GPL-2.0-or-later
*/
// kenning: manage face models and run recognition, with Howdy's arguments.

#include "daemon.h"
#include "engine.h"
#include "service.h"

#include <opencv2/core/utils/logger.hpp>

#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <pwd.h>
#include <unistd.h>

using namespace FaceUnlock;

namespace
{
std::string defaultUser()
{
    if (const char *sudoUser = std::getenv("SUDO_USER")) {
        return sudoUser;
    }
    const passwd *entry = getpwuid(getuid());
    return entry ? entry->pw_name : "";
}

bool validUser(const std::string &user)
{
    return !user.empty() && user.find('/') == std::string::npos && getpwnam(user.c_str());
}

int usage()
{
    std::cerr << "usage: kenning [--plain] [-y] [-U USER] add [LABEL] | remove ID | clear | list | test\n"
                 "       kenning compare USER\n"
                 "       kenning daemon\n";
    return Error;
}

int requireRoot()
{
    if (geteuid() != 0) {
        std::cerr << "This command changes face models and needs root, e.g. sudo kenning ...\n";
        return Error;
    }
    return 0;
}

int add(const std::string &user, const std::string &label, bool plain)
{
    if (const int error = requireRoot()) {
        return error;
    }
    if (!plain) {
        std::cout << "Adding face model for the user " << user << '\n';
    }
    // The service captures the face in its sandbox, so OpenCV doesn't run as root here.
    constexpr int enrolmentLineTimeoutSeconds = 30;
    const Reply reply = callService({.command = "enroll",
                                     .user = user,
                                     .argument = label,
                                     .idleTimeoutSeconds = enrolmentLineTimeoutSeconds,
                                     .onMessage =
                                         [](MessageKind, const std::string &line) {
                                             std::cout << line << std::endl;
                                         },
                                     .cancelled = {}});
    if (reply.code < 0) {
        std::cout << "The Kenning service isn't reachable at " << socketPath << std::endl;
        return Error;
    }
    return reply.code;
}

int remove(const std::string &user, const std::string &idText)
{
    if (const int error = requireRoot()) {
        return error;
    }
    int id = -1;
    try {
        id = std::stoi(idText);
    } catch (...) {
        return usage();
    }
    std::vector<FaceModel> models = loadModels(user);
    const auto removed = std::erase_if(models, [id](const FaceModel &model) {
        return model.id == id;
    });
    if (removed == 0) {
        std::cout << "No face model with the ID " << id << " exists for " << user << std::endl;
        return Error;
    }
    if (!saveModels(user, models)) {
        return Error;
    }
    std::cout << "Removed face model " << id << std::endl;
    return 0;
}

int list(const std::string &user, bool plain)
{
    // Face models belong to the service user; others can only ask whether there are any.
    if (geteuid() != 0) {
        const int code = askService("has-model", user, quickTimeoutSeconds);
        if (code == Recognized || code == LockedOut) {
            if (!plain) {
                std::cout << "Face recognition is set up for " << user << ". To see the face models, run:\n\n\tsudo kenning list\n";
                if (code == LockedOut) {
                    std::cout << "\nFace unlock is paused for a few minutes after too many faces that weren't recognized.\n";
                }
            }
            return 0;
        }
        if (code < 0) {
            std::cerr << "The Kenning service isn't reachable at " << socketPath << '\n';
            return Error;
        }
    }
    const std::vector<FaceModel> models = geteuid() == 0 ? loadModels(user) : std::vector<FaceModel>();
    if (models.empty()) {
        if (!plain) {
            std::cout << "No face model known for the user " << user << ", please run:\n\n\tsudo kenning -U " << user << " add\n";
        }
        return Error;
    }
    if (!plain) {
        std::cout << "Known face models for " << user << ":\n\nID  Date                 Label\n";
    }
    for (const FaceModel &model : models) {
        char date[32];
        const std::time_t time = model.time;
        std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", std::localtime(&time));
        if (plain) {
            std::cout << model.id << ',' << date << ',' << model.label << '\n';
        } else {
            std::cout << model.id << std::string(std::max<int>(1, 4 - int(std::to_string(model.id).size())), ' ') << date << "  " << model.label << '\n';
        }
    }
    return 0;
}

int testAsUser(const Config &config, const std::string &user)
{
    const int code = askService("compare", user, scanTimeoutSeconds(config));
    switch (code) {
    case Recognized:
        std::cout << "Identified face as " << user << '\n';
        break;
    case NoModel:
        std::cout << "Failure, no face model known\n";
        break;
    case Timeout:
        std::cout << "Failure, timeout reached\n";
        break;
    case TooDark:
        std::cout << "Failure, image too dark\n";
        break;
    case TooBright:
        std::cout << "Failure, image too bright\n";
        break;
    case LockedOut:
        std::cout << "Failure, too many attempts; face unlock is locked for a few minutes\n";
        break;
    case -1:
        std::cout << "Failure, the Kenning service isn't reachable at " << socketPath << '\n';
        return Error;
    default:
        std::cout << "Failure, general abort\n";
        break;
    }
    std::cout << "  (sudo kenning test shows the scores and timings)" << std::endl;
    return code;
}

int test(const std::string &user)
{
    const Config config;
    if (geteuid() != 0) {
        return testAsUser(config, user);
    }
    const ScanResult result = compare(config, user);
    const ScanStats &stats = result.stats;
    std::cout << result.message << '\n'
              << "  best score       " << result.bestScore << " (needs " << config.number("video/certainty", 6.0) << " or lower)\n"
              << "  frames           " << stats.frames << " read, " << stats.darkFrames << " too dark, " << stats.brightFrames << " too bright, "
              << stats.faceFrames << " with a face\n"
              << "  face confidence  " << stats.bestDetection << " (detector keeps " << config.number("video/detection_threshold", 0.8) << " or higher)\n"
              << "  camera open      " << stats.cameraMs << " ms\n"
              << "  networks load    " << stats.networksMs << " ms\n"
              << "  first frame      " << stats.firstFrameMs << " ms after start\n"
              << "  detection        " << stats.detectMs << " ms total\n"
              << "  encoding         " << stats.encodeMs << " ms total" << std::endl;
    return result.code;
}
}

int main(int argc, char **argv)
{
    // OpenCV 5 warns on every network load that it ignores the preferred target.
    cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_ERROR);

    bool plain = false;
    std::string user;
    std::vector<std::string> positional;
    bool optionsDone = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (optionsDone) {
            positional.push_back(arg);
        } else if (arg == "--") {
            // Everything after is positional, e.g. a face label starting with "-".
            optionsDone = true;
        } else if (arg == "--plain") {
            plain = true;
        } else if (arg == "-y") {
            // Nothing asks questions; accepted for Howdy compatibility.
        } else if ((arg == "-U" || arg == "--user") && i + 1 < argc) {
            user = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            usage();
            return 0;
        } else {
            positional.push_back(arg);
        }
    }
    if (positional.empty()) {
        return usage();
    }
    const std::string command = positional[0];

    if (command == "daemon") {
        return runDaemon();
    }
    if (command == "compare") {
        if (positional.size() != 2 || !validUser(positional[1])) {
            return Aborted;
        }
        const ScanResult result = compare(Config(), positional[1]);
        std::cout << result.message << std::endl;
        return result.code;
    }

    if (user.empty()) {
        user = defaultUser();
    }
    if (!validUser(user)) {
        std::cerr << "Unknown user " << user << '\n';
        return Error;
    }

    if (command == "add") {
        return add(user, positional.size() > 1 ? positional[1] : std::string(), plain);
    }
    if (command == "remove" && positional.size() == 2) {
        return remove(user, positional[1]);
    }
    if (command == "clear") {
        if (const int error = requireRoot()) {
            return error;
        }
        std::filesystem::remove(modelPath(user));
        std::cout << "Removed all face models for " << user << std::endl;
        return 0;
    }
    if (command == "list") {
        return list(user, plain);
    }
    if (command == "test") {
        return test(user);
    }
    if (command == "version") {
        std::cout << "kenning " << KENNING_VERSION << '\n';
        return 0;
    }
    return usage();
}
