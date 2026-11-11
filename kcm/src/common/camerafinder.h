/*
    SPDX-FileCopyrightText: 2026 Rishon Jonathan R
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

// The camera face unlock uses is the engine's choice: findCamera() and cameraFor() in camera.h.

#include "camera.h"

#include <QString>

namespace FaceUnlock
{
// A colour camera for previews; virtual cameras are fine, nothing is authenticated with them.
QString findPreviewCamera();
}
