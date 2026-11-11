/*
 * SPDX-FileCopyrightText: 2026 Rishon Jonathan R
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

import QtQuick
import QtQuick.Layouts
import QtQuick.Shapes

import org.kde.kirigami as Kirigami
import org.kde.plasma.components as PlasmaComponents3

// Ring: an orbiting arc; green checkmark on success, red shake on failure.
// Theme API 1: the host (FaceScanIndicator) sets the properties below.
Item {
    id: root

    property int faceState // 0 idle, 1 scanning, 2 recognized, 3 failed
    property string message
    property color highlightColor
    property color successColor
    property color failureColor
    property color textColor
    // Optional: "face" or "fingerprint", what is being read; hosts before 0.9 don't set it.
    property string method: "face"

    readonly property bool fingerprint: method === "fingerprint"
    readonly property bool scanning: faceState === 1
    readonly property bool recognized: faceState === 2
    readonly property bool failed: faceState === 3
    readonly property color accent: recognized ? successColor : (failed ? failureColor : highlightColor)

    implicitWidth: column.implicitWidth
    implicitHeight: column.implicitHeight

    onFaceStateChanged: {
        if (recognized) {
            popAnimation.restart();
        } else if (failed) {
            shakeAnimation.restart();
        }
    }

    ColumnLayout {
        id: column
        anchors.horizontalCenter: parent.horizontalCenter
        spacing: Kirigami.Units.largeSpacing

        Item {
            id: badge
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: Kirigami.Units.smallSpacing
            implicitWidth: Kirigami.Units.gridUnit * 3
            implicitHeight: implicitWidth

            transform: Translate {
                id: shake
            }

            // Soft glow that breathes while scanning
            Rectangle {
                id: halo
                anchors.centerIn: parent
                width: parent.width
                height: width
                radius: width / 2
                color: root.accent
                opacity: root.scanning ? 0.15 : (root.active ? 0.12 : 0)

                SequentialAnimation on scale {
                    running: root.scanning
                    loops: Animation.Infinite
                    alwaysRunToEnd: true
                    NumberAnimation {
                        from: 0.8
                        to: 1.05
                        duration: 900
                        easing.type: Easing.InOutSine
                    }
                    NumberAnimation {
                        from: 1.05
                        to: 0.8
                        duration: 900
                        easing.type: Easing.InOutSine
                    }
                }

                Behavior on color {
                    ColorAnimation {
                        duration: Kirigami.Units.longDuration
                    }
                }
            }

            // Progress ring: a short arc that orbits while scanning and closes on a result
            Shape {
                id: ring
                anchors.fill: parent
                preferredRendererType: Shape.CurveRenderer

                RotationAnimator on rotation {
                    running: root.scanning
                    loops: Animation.Infinite
                    from: 0
                    to: 360
                    duration: 1100
                }

                ShapePath {
                    strokeColor: root.accent
                    strokeWidth: Kirigami.Units.smallSpacing
                    fillColor: "transparent"
                    capStyle: ShapePath.RoundCap

                    PathAngleArc {
                        id: arc
                        centerX: ring.width / 2
                        centerY: ring.height / 2
                        radiusX: ring.width / 2 - Kirigami.Units.smallSpacing
                        radiusY: radiusX
                        startAngle: -90
                        sweepAngle: root.scanning ? 100 : 360

                        Behavior on sweepAngle {
                            NumberAnimation {
                                duration: Kirigami.Units.veryLongDuration
                                easing.type: Easing.OutCubic
                            }
                        }
                    }

                    Behavior on strokeColor {
                        ColorAnimation {
                            duration: Kirigami.Units.longDuration
                        }
                    }
                }
            }

            // What the ring waits for, when it is a finger
            Kirigami.Icon {
                anchors.centerIn: parent
                width: parent.width * 0.5
                height: width
                visible: root.fingerprint
                source: "fingerprint-symbolic"
                color: root.accent
                isMask: true
                opacity: root.recognized ? 0 : 1

                Behavior on opacity {
                    NumberAnimation {
                        duration: Kirigami.Units.longDuration
                    }
                }
            }

            // Checkmark that draws itself in on success
            Shape {
                id: check
                anchors.centerIn: parent
                width: parent.width * 0.46
                height: width
                preferredRendererType: Shape.CurveRenderer
                visible: root.recognized

                property real progress: root.recognized ? 1 : 0
                Behavior on progress {
                    NumberAnimation {
                        duration: Kirigami.Units.veryLongDuration
                        easing.type: Easing.OutCubic
                    }
                }

                ShapePath {
                    strokeColor: root.successColor
                    strokeWidth: Math.max(3, check.width * 0.12)
                    fillColor: "transparent"
                    capStyle: ShapePath.RoundCap
                    joinStyle: ShapePath.RoundJoin

                    // Short stroke first (first 40% of progress), then the long one
                    startX: check.width * 0.12
                    startY: check.height * 0.52
                    PathLine {
                        x: check.width * (0.12 + 0.28 * Math.min(1, check.progress / 0.4))
                        y: check.height * (0.52 + 0.24 * Math.min(1, check.progress / 0.4))
                    }
                    PathLine {
                        x: check.width * (0.40 + 0.50 * Math.max(0, (check.progress - 0.4) / 0.6))
                        y: check.height * (0.76 - 0.52 * Math.max(0, (check.progress - 0.4) / 0.6))
                    }
                }
            }
        }

        PlasmaComponents3.Label {
            id: label
            Layout.alignment: Qt.AlignHCenter
            Layout.maximumWidth: Kirigami.Units.gridUnit * 16
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            font.pointSize: Kirigami.Theme.defaultFont.pointSize + 2
            text: root.message
            color: root.textColor

            SequentialAnimation on opacity {
                running: root.scanning
                loops: Animation.Infinite
                alwaysRunToEnd: true
                NumberAnimation {
                    from: 1
                    to: 0.55
                    duration: 900
                    easing.type: Easing.InOutSine
                }
                NumberAnimation {
                    from: 0.55
                    to: 1
                    duration: 900
                    easing.type: Easing.InOutSine
                }
            }
        }
    }

    SequentialAnimation {
        id: popAnimation
        NumberAnimation {
            target: badge
            property: "scale"
            to: 1.12
            duration: Kirigami.Units.longDuration
            easing.type: Easing.OutQuad
        }
        NumberAnimation {
            target: badge
            property: "scale"
            to: 1
            duration: Kirigami.Units.longDuration
            easing.type: Easing.InQuad
        }
    }

    SequentialAnimation {
        id: shakeAnimation
        loops: 2
        NumberAnimation { target: shake; property: "x"; to: -Kirigami.Units.largeSpacing; duration: 50 }
        NumberAnimation { target: shake; property: "x"; to: Kirigami.Units.largeSpacing; duration: 100 }
        NumberAnimation { target: shake; property: "x"; to: 0; duration: 50 }
    }
}
