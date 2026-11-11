/*
 * SPDX-FileCopyrightText: 2026 Rishon Jonathan R
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

import QtQuick
import QtQuick.Layouts

import org.kde.kirigami as Kirigami
import org.kde.plasma.components as PlasmaComponents3

// Ripple: rings spread out while scanning, then a green disc or a red ring.
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
        if (failed) {
            shakeAnimation.restart();
        }
    }

    ColumnLayout {
        id: column
        anchors.horizontalCenter: parent.horizontalCenter
        spacing: Kirigami.Units.largeSpacing

        Item {
            id: stage
            Layout.alignment: Qt.AlignHCenter
            implicitWidth: Kirigami.Units.gridUnit * 5
            implicitHeight: implicitWidth

            transform: Translate {
                id: shake
            }

            // Three rings, staggered, each growing and fading out
            Repeater {
                model: 3
                Rectangle {
                    id: wave
                    required property int index
                    anchors.centerIn: parent
                    width: stage.width
                    height: width
                    radius: width / 2
                    color: "transparent"
                    border.width: 2
                    border.color: root.accent
                    visible: root.scanning
                    scale: 0.3
                    opacity: 0

                    SequentialAnimation {
                        running: root.scanning
                        loops: Animation.Infinite
                        PauseAnimation { duration: wave.index * 500 }
                        ParallelAnimation {
                            NumberAnimation { target: wave; property: "scale"; from: 0.3; to: 1; duration: 1500; easing.type: Easing.OutCubic }
                            NumberAnimation { target: wave; property: "opacity"; from: 0.9; to: 0; duration: 1500 }
                        }
                        PauseAnimation { duration: (2 - wave.index) * 500 }
                    }
                }
            }

            // Centre disc: small while scanning, fills in when recognized
            Rectangle {
                id: disc
                anchors.centerIn: parent
                width: stage.width * (root.recognized ? 0.8 : 0.4)
                height: width
                radius: width / 2
                color: root.recognized ? root.successColor : "transparent"
                border.width: root.recognized ? 0 : 3
                border.color: root.accent

                Behavior on width {
                    NumberAnimation { duration: Kirigami.Units.longDuration; easing.type: Easing.OutBack }
                }
                Behavior on color {
                    ColorAnimation { duration: Kirigami.Units.longDuration }
                }
                Behavior on border.color {
                    ColorAnimation { duration: Kirigami.Units.longDuration }
                }

                Kirigami.Icon {
                    anchors.centerIn: parent
                    width: parent.width * 0.55
                    height: width
                    source: root.recognized ? "checkmark" : (root.failed ? "dialog-close" : (root.fingerprint ? "fingerprint-symbolic" : "user-identity"))
                    color: root.recognized ? "white" : root.accent
                    isMask: true
                }
            }
        }

        PlasmaComponents3.Label {
            Layout.alignment: Qt.AlignHCenter
            Layout.maximumWidth: Kirigami.Units.gridUnit * 16
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            font.pointSize: Kirigami.Theme.defaultFont.pointSize + 2
            color: root.textColor
            text: root.message
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
