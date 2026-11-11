/*
 * SPDX-FileCopyrightText: 2026 Rishon Jonathan R
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

import QtQuick
import QtQuick.Layouts

import org.kde.kirigami as Kirigami

// Face unlock progress for the login screen, the lock screen and the settings page; themes: see THEMES.md.
Item {
    id: root

    // 0 idle (hidden), 1 scanning, 2 recognized, 3 failed
    property int faceState: 0
    property string message
    property string style: "hello"
    property var themeDirs: ["file:///usr/local/share/kenning/themes/", "file:///usr/share/kenning/themes/"]

    readonly property bool active: faceState !== 0
    readonly property string themeId: /^[A-Za-z0-9][A-Za-z0-9._-]*$/.test(style) ? style : "hello"

// The form is disabled while PAM runs, which would grey the text out.
    property color textColor
    Component.onCompleted: textColor = Kirigami.Theme.textColor
    Connections {
        target: root.Kirigami.Theme
        function onTextColorChanged() {
            if (root.enabled) {
                root.textColor = root.Kirigami.Theme.textColor;
            }
        }
    }

    Layout.fillWidth: true
    implicitWidth: loader.item ? loader.item.implicitWidth : plainMessage.implicitWidth
    implicitHeight: !active ? 0 : loader.item ? loader.item.implicitHeight : themeMissing ? plainMessage.implicitHeight : 0
    opacity: active ? 1 : 0
    clip: true

    Behavior on implicitHeight {
        NumberAnimation {
            duration: Kirigami.Units.longDuration
            easing.type: Easing.InOutQuad
        }
    }
    Behavior on opacity {
        NumberAnimation {
            duration: Kirigami.Units.longDuration
        }
    }

    // Where the theme may be, in the order tried: each directory, then the built-in theme.
    readonly property var candidates: themeDirs.map(dir => dir + themeId + "/contents/ui/main.qml")
        .concat(themeId === "hello" ? [] : [themeDirs[themeDirs.length - 1] + "hello/contents/ui/main.qml"])
    property int attempt: 0
    onThemeIdChanged: attempt = 0
    // Neither the chosen theme nor the built-in one could be loaded.
    readonly property bool themeMissing: loader.status === Loader.Error && attempt >= candidates.length - 1

    // On to the next place when this one failed. Also asked after each change of source: a file
    // that isn't there fails at once, and the status, an error before and after, tells nobody.
    function tryNextTheme() {
        if (loader.status === Loader.Error && attempt < candidates.length - 1) {
            attempt++;
        }
    }

    Loader {
        id: loader
        anchors.horizontalCenter: parent.horizontalCenter
        source: root.candidates[Math.min(root.attempt, root.candidates.length - 1)]
        onStatusChanged: root.tryNextTheme()
        onSourceChanged: Qt.callLater(root.tryNextTheme)
    }

    // No theme could be loaded, not even the built-in one (a broken install): the message alone,
    // so a scan is never silent.
    Text {
        id: plainMessage
        anchors.horizontalCenter: parent.horizontalCenter
        width: Math.min(implicitWidth, root.width)
        visible: root.themeMissing
        text: root.message
        textFormat: Text.PlainText
        wrapMode: Text.WordWrap
        horizontalAlignment: Text.AlignHCenter
        font: Kirigami.Theme.defaultFont
        color: root.faceState === 2 ? Kirigami.Theme.positiveTextColor : root.faceState === 3 ? Kirigami.Theme.negativeTextColor : root.textColor
        topPadding: Kirigami.Units.smallSpacing
        bottomPadding: Kirigami.Units.smallSpacing
    }

    Binding {
        target: loader.item
        when: loader.status === Loader.Ready
        property: "faceState"
        value: root.faceState
    }
    Binding {
        target: loader.item
        when: loader.status === Loader.Ready
        property: "message"
        value: root.message
    }
    Binding {
        target: loader.item
        when: loader.status === Loader.Ready
        property: "highlightColor"
        value: Kirigami.Theme.highlightColor
    }
    Binding {
        target: loader.item
        when: loader.status === Loader.Ready
        property: "successColor"
        value: Kirigami.Theme.positiveTextColor
    }
    Binding {
        target: loader.item
        when: loader.status === Loader.Ready
        property: "failureColor"
        value: Kirigami.Theme.negativeTextColor
    }
    Binding {
        target: loader.item
        when: loader.status === Loader.Ready
        property: "textColor"
        value: root.textColor
    }
}
