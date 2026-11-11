/*
    SPDX-FileCopyrightText: 2026 Rishon Jonathan R
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "camerafinder.h"

#include <QDir>

namespace FaceUnlock
{
QString findPreviewCamera()
{
    for (const std::string &path : stableCameraPaths()) {
        if (const CameraFormats camera = probeCamera(path); camera.capture && camera.colour) {
            return QString::fromStdString(path);
        }
    }
    const QDir devices(QStringLiteral("/dev"));
    for (const QFileInfo &entry : devices.entryInfoList({QStringLiteral("video*")}, QDir::System, QDir::Name)) {
        if (const CameraFormats camera = probeCamera(entry.absoluteFilePath().toStdString()); camera.capture && camera.loopback && camera.colour) {
            return entry.absoluteFilePath();
        }
    }
    return {};
}
}
