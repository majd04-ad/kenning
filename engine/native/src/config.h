/*
    SPDX-FileCopyrightText: 2026 Rishon Jonathan R
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

// Header-only, so pam_kenning can use it without OpenCV or Qt.

#include <charconv>
#include <fstream>
#include <map>
#include <string>

namespace FaceUnlock
{
// Build time only: a runtime override could point authentication at other face models.
#ifndef KENNING_SYSCONFDIR
#define KENNING_SYSCONFDIR "/etc/kenning"
#endif
#ifndef KENNING_DATADIR
#define KENNING_DATADIR "/usr/share/kenning"
#endif

inline constexpr const char *configPath = KENNING_SYSCONFDIR "/config.ini";
inline constexpr const char *modelsDir = KENNING_SYSCONFDIR "/models";
inline constexpr int maxLabelLength = 24;
#ifndef KENNING_SERVICE_USER
#define KENNING_SERVICE_USER "kenning"
#endif
inline constexpr const char *serviceUser = KENNING_SERVICE_USER;
inline constexpr const char *detectorModel = KENNING_DATADIR "/models/face_detection_yunet_2023mar.onnx";
inline constexpr const char *recognizerModel = KENNING_DATADIR "/models/face_recognition_sface_2021dec.onnx";

// Exit codes shared with Howdy's compare.py
enum ExitCode {
    Recognized = 0,
    Error = 1,
    NoModel = 10,
    Timeout = 11,
    Aborted = 12,
    TooDark = 13,
    LockedOut = 14, // too many failed attempts (the service's rate limit)
    TooBright = 15, // washed out by sunlight or a bright lamp (Kenning only)
    NeedsPassword = 16, // no password sign-in since the computer started (Kenning only)
    NoCamera = 17, // the camera is switched off, unplugged, in use, or none is set up (Kenning only)
    NotRecognized = 18, // a finger was read but wasn't the user's, as often as allowed (Kenning only)
    NoReader = 19, // no fingerprint reader, or fprintd isn't installed (Kenning only)
};

// config.ini values keyed "section/key"
class Config
{
public:
    explicit Config(const std::string &path = configPath)
    {
        std::ifstream file(path);
        std::string section;
        std::string line;
        while (std::getline(file, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#' || line[0] == ';') {
                continue;
            }
            if (line.front() == '[' && line.back() == ']') {
                section = line.substr(1, line.size() - 2);
            } else if (const auto equals = line.find('='); equals != std::string::npos) {
                m_values[section + '/' + trim(line.substr(0, equals))] = trim(line.substr(equals + 1));
            }
        }
    }

    std::string string(const std::string &key, const std::string &fallback = {}) const
    {
        const auto it = m_values.find(key);
        return it == m_values.end() ? fallback : it->second;
    }

    // Like std::stod: a leading number counts, anything after it is ignored.
    double number(const std::string &key, double fallback) const
    {
        const std::string text = string(key);
        double value = fallback;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        return error == std::errc() ? value : fallback;
    }

    bool boolean(const std::string &key, bool fallback) const
    {
        const std::string value = string(key);
        return value.empty() ? fallback : value == "true";
    }

    const std::map<std::string, std::string> &values() const
    {
        return m_values;
    }

private:
    static std::string trim(const std::string &text)
    {
        const auto begin = text.find_first_not_of(" \t\r");
        const auto end = text.find_last_not_of(" \t\r");
        return begin == std::string::npos ? std::string() : text.substr(begin, end - begin + 1);
    }

    std::map<std::string, std::string> m_values;
};

inline std::string modelPath(const std::string &user)
{
    return std::string(modelsDir) + '/' + user + ".dat";
}
}
