/*
    SPDX-FileCopyrightText: 2026 Rishon Jonathan R
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

// Recognition with OpenCV's YuNet and SFace. Config keys, model files and exit codes follow Howdy.

#include "config.h"
#include "facemodels.h"

#include <opencv2/core.hpp>

#include <functional>
#include <string>
#include <vector>

namespace FaceUnlock
{
// Lower is closer, as in Howdy: (1 - cosine) * 10, so SFace's recommended 0.363 is 6.37.
double matchScore(double cosine);
constexpr double noMatchScore = 99;

struct ScanStats {
    double cameraMs = 0;
    double networksMs = 0;
    double firstFrameMs = 0; // since the scan started
    double detectMs = 0;
    double encodeMs = 0;
    int frames = 0;
    int darkFrames = 0; // including black ones
    int brightFrames = 0;
    int faceFrames = 0;
    double bestDetection = 0; // 0 to 1
};

struct ScanResult {
    ExitCode code = Error;
    double bestScore = noMatchScore;
    std::string message;
    ScanStats stats = {};
};

// Looks for the user's face until video/timeout, or until cancelled() returns true.
ScanResult compare(const Config &config, const std::string &user, const std::function<bool()> &cancelled = {});

using ProgressCallback = std::function<void(const std::string &)>;

// Enrols one face: straight, then turned left, right, up and down, until cancelled() returns true.
ScanResult enroll(const Config &config, std::vector<cv::Mat> &encodings, const ProgressCallback &progress, const std::function<bool()> &cancelled = {});
}
