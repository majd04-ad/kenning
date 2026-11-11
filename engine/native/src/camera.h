/*
    SPDX-FileCopyrightText: 2026 Rishon Jonathan R
    SPDX-FileCopyrightText: 2026 Majd Abu Diab
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

// Which camera face unlock uses. Header-only, without OpenCV or Qt, so the service, the settings
// page and its helper all agree.

#include "config.h"

#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <linux/videodev2.h>
#include <optional>
#include <string>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace FaceUnlock
{
struct Camera {
    std::string path; // empty without a usable camera
    bool colour = false; // an ordinary webcam rather than an IR sensor
};

struct CameraFormats {
    bool capture = false;
    bool loopback = false; // v4l2loopback
    bool grey = false;
    bool colour = false;
};

inline constexpr const char *stableCameraDir = "/dev/v4l/by-path";

inline CameraFormats probeCamera(const std::string &path)
{
    CameraFormats found;
    const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        return found;
    }
    v4l2_capability capability{};
    if (ioctl(fd, VIDIOC_QUERYCAP, &capability) == 0) {
        const __u32 capabilities = (capability.capabilities & V4L2_CAP_DEVICE_CAPS) ? capability.device_caps : capability.capabilities;
        found.capture = capabilities & V4L2_CAP_VIDEO_CAPTURE;
        found.loopback = std::strncmp(reinterpret_cast<const char *>(capability.driver), "v4l2 loopback", sizeof(capability.driver)) == 0;
    }
    v4l2_fmtdesc format{};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    while (found.capture && ioctl(fd, VIDIOC_ENUM_FMT, &format) == 0) {
        found.grey |= format.pixelformat == V4L2_PIX_FMT_GREY;
        found.colour |= format.pixelformat == V4L2_PIX_FMT_MJPEG || format.pixelformat == V4L2_PIX_FMT_YUYV;
        ++format.index;
    }
    close(fd);
    return found;
}

// The first video node of each camera, by where it is plugged in. Only these are considered:
// virtual cameras, which any program can feed, have no by-path name.
inline std::vector<std::string> stableCameraPaths(const std::string &directory = stableCameraDir)
{
    constexpr std::string_view suffix = "-video-index0";
    std::vector<std::string> paths;
    std::error_code error;
    for (const auto &entry : std::filesystem::directory_iterator(directory, error)) {
        const std::string name = entry.path().filename().string();
        if (name.size() > suffix.size() && name.ends_with(suffix)) {
            paths.push_back(entry.path().string());
        }
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

using CameraProbe = std::function<CameraFormats(const std::string &)>;

// The IR sensor (greyscale only) if there is one, else a colour webcam.
inline Camera findCamera(const std::vector<std::string> &paths = stableCameraPaths(), const CameraProbe &probe = probeCamera)
{
    Camera colourCamera;
    for (const std::string &path : paths) {
        const CameraFormats camera = probe(path);
        if (!camera.capture || camera.loopback) {
            continue;
        }
        if (camera.grey && !camera.colour) {
            return {.path = path, .colour = false};
        }
        // Several paths can name one device (usb-… and usbv2-…); keep the first.
        if (camera.colour && colourCamera.path.empty()) {
            colourCamera = {.path = path, .colour = true};
        }
    }
    return colourCamera;
}

inline bool cameraConfigured(const Config &config)
{
    const std::string path = config.string("video/device_path");
    return !path.empty() && path != "none";
}

inline bool isCharacterDevice(const std::string &path)
{
    struct stat status{};
    return stat(path.c_str(), &status) == 0 && S_ISCHR(status.st_mode);
}

// The camera a scan opens, or nothing when there is none to use:
//  - the configured one, as long as it is there. A camera that is switched off or unplugged is
//    never replaced by another: an IR sensor must not quietly become a webcam a photo can fool.
//  - with none configured (a config.ini that lost its settings, or a first look added from the
//    terminal), an IR camera found now. A colour webcam has to be chosen in Set Up.
inline std::optional<Camera> cameraFor(
    const Config &config,
    const std::function<Camera()> &find = [] {
        return findCamera();
    })
{
    if (cameraConfigured(config)) {
        const std::string path = config.string("video/device_path");
        if (!isCharacterDevice(path)) {
            return std::nullopt;
        }
        return Camera{.path = path, .colour = config.boolean("video/colour", false)};
    }
    const Camera found = find();
    if (found.path.empty() || found.colour) {
        return std::nullopt;
    }
    return found;
}
}
