/*
    SPDX-FileCopyrightText: 2026 Rishon Jonathan R
    SPDX-License-Identifier: GPL-2.0-or-later
*/

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts

import org.kde.kcmutils as KCM
import org.kde.kirigami as Kirigami

KCM.SimpleKCM {
    id: page

    title: i18nc("@title", "Recognition and Security")

    readonly property var settings: kcm.settings
    // Above this the match is looser than SFace's recommended cosine of 0.363 (6.37)
    readonly property real lenientCertainty: 6.4

    component HeaderMessage: Kirigami.InlineMessage {
        Layout.fillWidth: true
        position: Kirigami.InlineMessage.Position.Header
    }

    header: ColumnLayout {
        spacing: 0

        HeaderMessage {
            type: Kirigami.MessageType.Warning
            visible: certainty.value > page.lenientCertainty
            text: i18n("A lenient threshold accepts looser matches, so people who look like you, such as close relatives, may be recognized as you.")
        }
        HeaderMessage {
            type: Kirigami.MessageType.Warning
            visible: !settings["core/abort_if_ssh"]
            text: i18n("Over SSH the person signing in is somewhere else, but the camera looks at whoever is in front of this computer. Anyone sitting here could approve a remote sudo.")
        }
        HeaderMessage {
            type: Kirigami.MessageType.Warning
            visible: settings["core/allow_outside_session"]
            text: i18n("Any program running as you can start sudo outside a sign-in session, including one started over SSH, and the camera will answer with whoever is in front of this computer.")
        }
        HeaderMessage {
            type: Kirigami.MessageType.Warning
            visible: !settings["core/password_after_boot"]
            text: i18n("Face unlock works right after a restart, so someone with your laptop and a face like yours, or with an ordinary webcam a photo of you, could sign in without ever knowing your password.")
        }
    }

    Kirigami.FormLayout {
        RowLayout {
            Kirigami.FormData.label: i18n("Match threshold:")
            QQC2.Label {
                text: i18nc("stricter face matching", "Strict")
            }
            QQC2.Slider {
                id: certainty
                Layout.preferredWidth: Kirigami.Units.gridUnit * 10
                from: 4
                to: 8
                stepSize: 0.1
                value: settings["video/certainty"]
                onMoved: kcm.setValue("video/certainty", Math.round(value * 10) / 10)
            }
            QQC2.Label {
                text: i18nc("more lenient face matching", "Lenient")
            }
            Kirigami.ContextualHelpButton {
                toolTipText: i18n("Currently %1. Lower values only accept closer matches; around 6 suits most faces.", certainty.value.toFixed(1))
            }
        }
        QQC2.SpinBox {
            Kirigami.FormData.label: i18n("Give up after:")
            from: 1
            to: 30
            value: settings["video/timeout"]
            textFromValue: (value, locale) => i18np("%1 second", "%1 seconds", value)
            valueFromText: (text, locale) => parseInt(text)
            onValueModified: kcm.setValue("video/timeout", value)
        }
        RowLayout {
            Kirigami.FormData.label: i18n("Skip frames darker than:")
            spacing: 0
            QQC2.SpinBox {
                from: 0
                to: 100
                value: settings["video/dark_threshold"]
                textFromValue: (value, locale) => i18nc("percent", "%1%", value)
                valueFromText: (text, locale) => parseInt(text)
                onValueModified: kcm.setValue("video/dark_threshold", value)
            }
            Kirigami.ContextualHelpButton {
                toolTipText: i18n("The share of nearly black pixels above which an ordinary webcam's picture counts as too dark. A dark frame is still used if a face is clearly found in it. Infrared cameras light your face themselves, so a dark room doesn't matter to them; they only report too dark when covered.")
            }
        }

        Item {
            Kirigami.FormData.isSection: true
        }

        RowLayout {
            Kirigami.FormData.label: i18n("Messages:")
            spacing: 0
            QQC2.CheckBox {
                text: i18n("Say when face recognition starts")
                checked: settings["core/detection_notice"]
                onToggled: kcm.setValue("core/detection_notice", checked)
            }
            Kirigami.ContextualHelpButton {
                toolTipText: i18n("The login and lock screens start their animation from this message. Without it they look like they are only waiting for a password while the camera is on.")
            }
        }
        QQC2.CheckBox {
            text: i18n("Say who was recognized")
            checked: !settings["core/no_confirmation"]
            onToggled: kcm.setValue("core/no_confirmation", !checked)
        }

        Item {
            Kirigami.FormData.isSection: true
        }

        RowLayout {
            Kirigami.FormData.label: i18n("Don't use:")
            spacing: 0
            QQC2.CheckBox {
                text: i18n("When the lid is closed")
                checked: settings["core/abort_if_lid_closed"]
                onToggled: kcm.setValue("core/abort_if_lid_closed", checked)
            }
            Kirigami.ContextualHelpButton {
                toolTipText: i18n("With the lid closed the camera can't see anyone, so every sign-in would wait for face recognition to give up before asking for the password.")
            }
        }
        QQC2.CheckBox {
            text: i18n("Over SSH")
            checked: settings["core/abort_if_ssh"]
            onToggled: kcm.setValue("core/abort_if_ssh", checked)
        }
        RowLayout {
            spacing: 0
            QQC2.CheckBox {
                text: i18n("For sudo and administrator prompts outside a sign-in session")
                // Unchecked means allowed, like the other "Don't use" boxes here.
                checked: !settings["core/allow_outside_session"]
                enabled: settings["core/abort_if_ssh"]
                onToggled: kcm.setValue("core/allow_outside_session", !checked)
            }
            Kirigami.ContextualHelpButton {
                toolTipText: i18n("On Plasma 6, terminals such as Konsole run outside a sign-in session, so with this checked sudo there doesn't use your face. Kenning can't tell them apart from a program started over SSH. The lock screen and fingerprints aren't affected: a finger has to be put on the reader on purpose.")
            }
        }
        RowLayout {
            spacing: 0
            QQC2.CheckBox {
                text: i18n("After a restart, until you sign in with your password")
                checked: settings["core/password_after_boot"]
                onToggled: kcm.setValue("core/password_after_boot", checked)
            }
            Kirigami.ContextualHelpButton {
                toolTipText: i18n("Like a phone: if your computer is stolen while switched off, your face alone can't sign in. Once you have signed in with your password, face unlock works until the next restart, also after signing out.")
            }
        }

        Item {
            Kirigami.FormData.isSection: true
        }

        QQC2.Label {
            Kirigami.FormData.label: i18n("Camera:")
            text: kcm.devicePath ? (kcm.colourCamera ? i18n("Ordinary webcam") : i18n("Infrared camera")) : i18n("Not configured")
        }
        QQC2.Label {
            visible: kcm.devicePath !== ""
            text: kcm.devicePath
            elide: Text.ElideMiddle
            Layout.maximumWidth: Kirigami.Units.gridUnit * 16
            opacity: 0.7
            font: Kirigami.Theme.smallFont

            QQC2.ToolTip.text: kcm.devicePath
            QQC2.ToolTip.visible: truncated && hoverHandler.hovered
            HoverHandler {
                id: hoverHandler
            }
        }
    }
}
