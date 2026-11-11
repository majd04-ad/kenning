/*
    SPDX-FileCopyrightText: 2026 Rishon Jonathan R
    SPDX-License-Identifier: GPL-2.0-or-later
*/

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Dialogs as QtDialogs
import QtQuick.Layouts

import org.kde.kcmutils as KCM
import org.kde.kirigami as Kirigami

KCM.GridViewKCM {
    id: page

    title: i18nc("@title", "Animation")

    readonly property string selected: kcm.settings["Appearance/AnimationStyle"]

    // [state, message, method]: the face, then a finger
    readonly property var previewStates: [[1, i18n("Looking for you…"), "face"], [2, i18n("Welcome, %1", kcm.userName), "face"],
                                          [1, i18n("Looking for you…"), "face"], [3, i18n("Couldn't recognize you"), "face"],
                                          [1, i18n("Place your finger on the fingerprint reader"), "fingerprint"],
                                          [3, i18n("Fingerprint not recognized, try again"), "fingerprint"],
                                          [2, i18n("Welcome, %1", kcm.userName), "fingerprint"]]
    property int previewStep: 0

    Timer {
        running: page.visible
        repeat: true
        interval: 2200
        onTriggered: page.previewStep = (page.previewStep + 1) % page.previewStates.length
    }

    actions: [
        Kirigami.Action {
            icon.name: "document-import"
            text: i18nc("@action:button", "Install from File…")
            onTriggered: themeFileDialog.open()
        }
    ]

    QtDialogs.FileDialog {
        id: themeFileDialog
        title: i18nc("@title:window", "Install Scan Animation Theme")
        nameFilters: [i18n("Theme archives (*.tar.gz *.tar.xz *.tar.zst *.tgz *.zip)")]
        onAccepted: kcm.installTheme(selectedFile)
    }

    view.model: kcm.themes
    view.currentIndex: kcm.themes.findIndex(theme => theme.id === page.selected)

    view.delegate: KCM.GridDelegate {
        id: tile
        required property var modelData
        required property int index

        text: modelData.name
        subtitle: modelData.author ? i18nc("@info theme author", "By %1", modelData.author) : ""
        toolTip: modelData.description
        thumbnailAvailable: true
        thumbnail: Rectangle {
            anchors.fill: parent
            color: "#1b1e20"
            clip: true

            Kirigami.Theme.inherit: false
            Kirigami.Theme.colorSet: Kirigami.Theme.Complementary

            FaceScanIndicator {
                anchors.centerIn: parent
                width: parent.width / scale
                scale: 0.55
                style: tile.modelData.id
                faceState: page.previewStates[page.previewStep][0]
                message: page.previewStates[page.previewStep][1]
                method: page.previewStates[page.previewStep][2]
            }
        }
        actions: [
            Kirigami.Action {
                visible: tile.modelData.removable
                icon.name: "edit-delete-remove"
                tooltip: i18nc("@info:tooltip", "Remove %1", tile.modelData.name)
                onTriggered: kcm.removeTheme(tile.modelData.id)
            }
        ]
        onClicked: {
            kcm.setValue("Appearance/AnimationStyle", modelData.id);
            GridView.view.currentIndex = index;
        }
    }

    footer: RowLayout {
        Kirigami.Icon {
            source: "dialog-information"
            Layout.preferredWidth: Kirigami.Units.iconSizes.small
            Layout.preferredHeight: Layout.preferredWidth
        }
        QQC2.Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: i18n("Themes run on the login and lock screens; only install ones you trust.")
        }
    }
}
