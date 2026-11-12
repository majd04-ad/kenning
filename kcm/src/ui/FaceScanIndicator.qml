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
    // Candidates before this one are skipped: they failed to create an item.
    property int attempt: 0
    onThemeIdChanged: attempt = 0

    // The first candidate from `from` on that compiles, as {component, index}, or null.
    // Qt.createComponent rather than Loader.source: a Loader logs "No such file or
    // directory" for every place that has no such theme, which is the usual case for
    // /usr/local. Only a file that is there but broken is worth a warning; its error,
    // unlike a missing file's, carries a line number.
    function findTheme(list, from) {
        for (let i = from; i < list.length; i++) {
            const component = Qt.createComponent(list[i]);
            if (component.status === Component.Ready) {
                return {component: component, index: i};
            }
            if (/\.qml:\d+/.test(component.errorString())) {
                console.warn(component.errorString().trim());
            }
        }
        return null;
    }
    readonly property var theme: findTheme(candidates, attempt)
    // Neither the chosen theme nor the built-in one could be loaded.
    readonly property bool themeMissing: theme === null

    Loader {
        id: loader
        anchors.horizontalCenter: parent.horizontalCenter
        sourceComponent: root.theme ? root.theme.component : null
        // Compiled but failed to create its item: on to the next place.
        onStatusChanged: {
            if (status === Loader.Error && root.theme) {
                root.attempt = root.theme.index + 1;
            }
        }
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
