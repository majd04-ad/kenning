/*
    SPDX-FileCopyrightText: 2026 Rishon Jonathan R
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "pipeline.h"
#include "camera.h"

#include <opencv2/imgproc.hpp>

#include <future>
#include <mutex>
#include <optional>
#include <thread>

namespace FaceUnlock
{
namespace
{
constexpr int histogramBins = 8;
constexpr double washedOutPercent = 50;

struct Networks {
    cv::Ptr<cv::FaceDetectorYN> detector;
    cv::Ptr<cv::FaceRecognizerSF> recognizer;
    float detectionThreshold = 0;
    double loadMs = 0; // 0 when they came from the cache
};

// Cached: the service runs many scans.
Networks loadNetworks(float detectionThreshold)
{
    static std::mutex mutex;
    static std::optional<Networks> cache;
    const std::lock_guard lock(mutex);
    if (cache && cache->detectionThreshold == detectionThreshold) {
        Networks cached = *cache;
        cached.loadMs = 0;
        return cached;
    }
    constexpr float overlapThreshold = 0.3f;
    constexpr int maxFaces = 50;
    const auto start = Clock::now();
    Networks loaded;
    loaded.detector = cv::FaceDetectorYN::create(detectorModel, "", cv::Size(320, 320), detectionThreshold, overlapThreshold, maxFaces);
    loaded.recognizer = cv::FaceRecognizerSF::create(recognizerModel, "");
    loaded.detectionThreshold = detectionThreshold;
    loaded.loadMs = millisecondsSince(start);
    cache = loaded;
    return loaded;
}
}

double millisecondsSince(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

Lighting judgeLighting(const cv::Mat &grey, double darkThresholdPercent)
{
    const float range[] = {0, 256};
    const float *ranges[] = {range};
    cv::Mat histogram;
    cv::calcHist(&grey, 1, nullptr, cv::Mat(), histogram, 1, &histogramBins, ranges);
    const double total = cv::sum(histogram)[0];
    if (total <= 0) {
        return Lighting::Black;
    }
    const double darkPercent = histogram.at<float>(0) / total * 100;
    const double brightPercent = histogram.at<float>(histogramBins - 1) / total * 100;
    if (darkPercent >= 100) {
        return Lighting::Black;
    }
    if (brightPercent > washedOutPercent) {
        return Lighting::WashedOut;
    }
    if (darkPercent > darkThresholdPercent) {
        return Lighting::Dark;
    }
    return Lighting::Usable;
}

Pipeline::Pipeline(const Config &config)
{
    const auto detectionThreshold = float(config.number("video/detection_threshold", 0.8));
    // Opening the camera wakes it from USB suspend (~250 ms); load the networks meanwhile.
    auto loading = std::async(std::launch::async, loadNetworks, detectionThreshold);

    const std::optional<Camera> camera = cameraFor(config);
    const auto start = Clock::now();
    bool opened = false;
    try {
        opened = camera && m_camera.open(camera->path, cv::CAP_V4L2);
    } catch (const cv::Exception &) {
        // As a camera that can't be opened; the networks' thread is still joined below.
    }
    m_cameraMs = millisecondsSince(start);
    const Networks networks = loading.get();
    if (!opened) {
        return;
    }
    m_device = camera->path;
    m_detector = networks.detector;
    m_recognizer = networks.recognizer;
    m_networksMs = networks.loadMs;
    m_maxHeight = int(config.number("video/max_height", 320));
    m_darkThreshold = config.number("video/dark_threshold", 60);
    m_rotate = int(config.number("video/rotate", 0));
    // Webcams keep colour: greyscale matched worse in tests.
    m_colour = camera->colour;
}

bool Pipeline::ready() const
{
    return m_camera.isOpened() && m_detector && m_recognizer;
}

bool Pipeline::colour() const
{
    return m_colour;
}

const std::string &Pipeline::device() const
{
    return m_device;
}

double Pipeline::cameraMs() const
{
    return m_cameraMs;
}

double Pipeline::networksMs() const
{
    return m_networksMs;
}

Pipeline::Frame Pipeline::read(cv::Mat &image)
{
    cv::Mat frame;
    if (!m_camera.read(frame) || frame.empty()) {
        // A camera that stopped answering fails at once: don't spin until the scan's deadline.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return Frame::Missing;
    }
    cv::Mat grey;
    if (frame.channels() == 1) {
        grey = frame;
    } else {
        cv::cvtColor(frame, grey, cv::COLOR_BGR2GRAY);
    }

    const Lighting lighting = judgeLighting(grey, m_darkThreshold);
    if (lighting == Lighting::Black) {
        return Frame::Black;
    }

    if (m_colour && frame.channels() == 3) {
        image = frame;
    } else {
        cv::cvtColor(grey, image, cv::COLOR_GRAY2BGR);
    }
    if (image.rows > m_maxHeight) {
        cv::resize(image, image, cv::Size(image.cols * m_maxHeight / image.rows, m_maxHeight));
    }
    switch (lighting) {
    case Lighting::Dark:
        return Frame::Dark;
    case Lighting::WashedOut:
        return Frame::WashedOut;
    default:
        return Frame::Usable;
    }
}

// video/rotate as in Howdy: 0 upright only, 1 upright and portrait, 2 portrait only.
std::vector<cv::Mat> Pipeline::orientations(const cv::Mat &image) const
{
    std::vector<cv::Mat> images;
    if (m_rotate != 2) {
        images.push_back(image);
    }
    if (m_rotate == 1 || m_rotate == 2) {
        for (const auto rotation : {cv::ROTATE_90_CLOCKWISE, cv::ROTATE_90_COUNTERCLOCKWISE}) {
            cv::Mat turned;
            cv::rotate(image, turned, rotation);
            images.push_back(turned);
        }
    }
    return images;
}

cv::Mat Pipeline::detectFaces(const cv::Mat &image)
{
    cv::Mat faces;
    m_detector->setInputSize(image.size());
    m_detector->detect(image, faces);
    return faces;
}

cv::Mat Pipeline::encode(const cv::Mat &image, const cv::Mat &face)
{
    cv::Mat aligned;
    cv::Mat feature;
    m_recognizer->alignCrop(image, face, aligned);
    m_recognizer->feature(aligned, feature);
    return feature.clone();
}

double Pipeline::cosineSimilarity(const cv::Mat &feature, const cv::Mat &known) const
{
    return m_recognizer->match(feature, known, cv::FaceRecognizerSF::FR_COSINE);
}
}
