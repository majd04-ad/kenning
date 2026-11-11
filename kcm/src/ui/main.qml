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
    id: root

    readonly property var settings: kcm.settings
    readonly property bool faceUnlockOn: !settings["core/disabled"]
    readonly property bool hasFaces: kcm.faces.length > 0
    readonly property var selectedTheme: kcm.themes.find(theme => theme.id === settings["Appearance/AnimationStyle"])

    component HeaderMessage: Kirigami.InlineMessage {
        Layout.fillWidth: true
        position: Kirigami.InlineMessage.Position.Header
    }

    // Above this the match is looser than SFace's recommended cosine of 0.363 (6.37)
    readonly property real lenientCertainty: 6.4

// "", "ok" or "fail"
    property string testResult: ""
    property string testMessage: ""

    function testOutcome(exitCode, message) {
        switch (exitCode) {
        case 0: return i18n("Recognized you. Face unlock is working.");
        case 10: return i18n("No face is set up yet.");
        case 11: return i18n("Didn't see a matching face in time. Try adding another look.");
        case 13: return kcm.colourCamera ? i18n("The camera picture was too dark. Is the camera covered? Otherwise, light your face from the front.")
                                         : i18n("The camera saw nothing. Is it covered?");
        case 15: return kcm.colourCamera ? i18n("The camera picture was too bright. Light your face from the front, with no bright window behind you.")
                                         : i18n("The camera picture was too bright. Move out of direct sunlight or away from a bright lamp.");
        case 14: return i18n("Face unlock is paused for a few minutes after too many faces that weren't yours.");
        case 16: return i18n("Face unlock waits until you have signed in with your password once after a restart.");
        case 17: return i18n("The camera couldn't be used. Is it switched off, unplugged or in use by another app?");
        case -1: return message;
        default: return i18n("Face recognition stopped (%1).", message);
        }
    }

    function setUp() {
        if (!hasFaces && kcm.colourCamera) {
            webcamDialog.open();
        } else {
            kcm.push("SetupPage.qml", {improving: hasFaces});
        }
    }

    Connections {
        target: kcm
        function onOperationFinished(operation, exitCode, message) {
            if (operation === "test") {
                root.testResult = exitCode === 0 ? "ok" : "fail";
                root.testMessage = root.testOutcome(exitCode, message);
            }
        }
        function onErrorOccurred(message) {
            errorMessage.text = message;
            errorMessage.visible = true;
        }
    }

    actions: [
        Kirigami.Action {
            displayComponent: QQC2.Switch {
                text: i18nc("@option:check face unlock on/off", "Face unlock")
                checked: root.faceUnlockOn
                enabled: kcm.engineInstalled
                onToggled: kcm.setValue("core/disabled", !checked)
            }
        }
    ]

    Kirigami.PromptDialog {
        id: webcamDialog
        parent: QQC2.Overlay.overlay
        title: i18nc("@title:window", "Use the Ordinary Webcam?")
        subtitle: i18n("This computer has no infrared camera, so face recognition would use the ordinary webcam. That is less safe:\n\n• A photo or video of you may be enough to unlock it.\n• It is less accurate, and needs your face lit from the front; it won't work in the dark or with a bright window behind you.\n\nIt will only be used to sign in. Installing software and other changes to the system will still need your password.")
        standardButtons: Kirigami.Dialog.Cancel
        customFooterActions: [
            Kirigami.Action {
                text: i18nc("@action:button", "Use Webcam")
                icon.name: "camera-web"
                onTriggered: {
                    webcamDialog.close();
                    kcm.push("SetupPage.qml", {improving: false, webcamAccepted: true});
                }
            }
        ]
    }

    header: ColumnLayout {
        spacing: 0

        HeaderMessage {
            type: Kirigami.MessageType.Error
            visible: !kcm.engineInstalled
            text: i18n("The face recognition engine is not installed.")
        }
        HeaderMessage {
            id: errorMessage
            type: Kirigami.MessageType.Error
            showCloseButton: true
            visible: false
        }
        HeaderMessage {
            type: Kirigami.MessageType.Warning
            visible: kcm.engineInstalled && kcm.devicePath === "" && !kcm.cameraAway
            text: i18n("No camera was found. If yours has a switch or a key that turns it off, turn it on and open this page again. An infrared camera, as on laptops that support face sign-in, works best and is the safest.")
        }
        HeaderMessage {
            type: Kirigami.MessageType.Warning
            visible: kcm.engineInstalled && kcm.cameraAway
            text: kcm.devicePath === ""
                ? i18n("The camera is switched off or unplugged, so face unlock can't see you. It works again once the camera is back; your password and fingerprint work meanwhile.")
                : i18n("The camera face unlock was set up with is switched off or unplugged. It works again once that camera is back. To use the camera that is connected now instead, add a look with it.")
        }
        HeaderMessage {
            type: Kirigami.MessageType.Error
            visible: kcm.colourCamera && (settings["polkit/face"] || settings["pam/sudo"])
            text: i18n("Your face can approve changes to the system, and this camera is an ordinary webcam: a photo or video of you could approve them too. Leave these on the password unless you accept that risk.")
        }
        HeaderMessage {
            type: Kirigami.MessageType.Warning
            visible: kcm.colourCamera && !(settings["polkit/face"] || settings["pam/sudo"])
            text: i18n("This is an ordinary webcam, not an infrared camera. It is less accurate, a photo or video of you may fool it, and it needs your face lit from the front.")
        }
        HeaderMessage {
            type: Kirigami.MessageType.Warning
            visible: settings["video/certainty"] > root.lenientCertainty
            text: i18n("The match threshold is lenient, so people who look like you, such as close relatives, may be recognized as you.")
            actions: Kirigami.Action {
                text: i18nc("@action:button", "Adjust…")
                onTriggered: kcm.push("AdvancedPage.qml")
            }
        }
        HeaderMessage {
            type: Kirigami.MessageType.Warning
            visible: !settings["core/abort_if_ssh"]
            text: i18n("Face recognition is used over SSH: anyone sitting at this computer could approve a remote sudo.")
            actions: Kirigami.Action {
                text: i18nc("@action:button", "Adjust…")
                onTriggered: kcm.push("AdvancedPage.qml")
            }
        }
        HeaderMessage {
            visible: root.testResult !== ""
            type: root.testResult === "ok" ? Kirigami.MessageType.Positive : Kirigami.MessageType.Warning
            text: root.testMessage
        }
    }

    Kirigami.FormLayout {
        enabled: kcm.engineInstalled

        RowLayout {
            Kirigami.FormData.label: i18n("Your face:")
            spacing: Kirigami.Units.smallSpacing

            QQC2.Label {
                visible: root.hasFaces
                text: i18np("%1 look saved", "%1 looks saved", kcm.faces.length)
            }
            QQC2.Button {
                visible: !root.hasFaces
                icon.name: "list-add"
                text: i18nc("@action:button", "Set Up…")
                enabled: !kcm.busy && kcm.devicePath !== ""
                onClicked: root.setUp()
            }
            QQC2.Button {
                visible: root.hasFaces
                icon.name: "configure"
                text: i18nc("@action:button", "Manage…")
                enabled: !kcm.busy
                onClicked: kcm.push("FacesPage.qml")
            }
            QQC2.Button {
                visible: root.hasFaces && kcm.devicePath !== ""
                icon.name: "dialog-ok-apply"
                text: i18nc("@action:button", "Test")
                enabled: !kcm.busy
                onClicked: {
                    root.testResult = "";
                    kcm.testRecognition();
                }
            }
            QQC2.BusyIndicator {
                visible: kcm.busy
                running: visible
                Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
                Layout.preferredWidth: Layout.preferredHeight
            }
            QQC2.Label {
                visible: kcm.busy
                text: i18n("Look at the camera…")
            }
        }

        Item {
            Kirigami.FormData.isSection: true
        }

        // The same places, in the same order, for the face and the fingerprint; each row explains itself.
        RowLayout {
            Kirigami.FormData.label: i18n("Use your face for:")
            spacing: 0
            QQC2.CheckBox {
                text: i18n("Login screen")
                enabled: kcm.loginScreenSupported || settings["pam/login"]
                checked: settings["pam/login"]
                onToggled: kcm.setValue("pam/login", checked)
            }
            Kirigami.ContextualHelpButton {
                toolTipText: kcm.loginScreenSupported
                    ? i18n("The login screen looks for your face, with Try Face Again as a button. Your password always works.")
                    : i18n("Needs the Kenning version of Plasma Login Manager (plasma-login-manager-kenning). Other login managers would look for your face before checking a password you typed.")
            }
        }
        RowLayout {
            spacing: 0
            QQC2.CheckBox {
                text: i18n("Lock screen")
                checked: settings["pam/lockscreen"]
                onToggled: kcm.setValue("pam/lockscreen", checked)
            }
            Kirigami.ContextualHelpButton {
                toolTipText: i18n("The lock screen looks for your face when the screen wakes. Your password always works.")
            }
        }
        RowLayout {
            spacing: 0
            QQC2.CheckBox {
                text: i18n("Administrator prompts")
                enabled: settings["Polkit/Agent"] || settings["polkit/face"]
                checked: settings["polkit/face"]
                onToggled: kcm.setValue("polkit/face", checked)
            }
            Kirigami.ContextualHelpButton {
                toolTipText: settings["Polkit/Agent"]
                    ? i18n("Administrator prompts show Face as a button next to the password. Your face can then approve changes to the system, such as installing software. Someone who looks like you, or holds the computer up to your face, could do the same.")
                    : i18n("Needs Kenning's dialog for administrator prompts, below.")
            }
        }
        RowLayout {
            spacing: 0
            QQC2.CheckBox {
                text: i18n("sudo in the terminal")
                checked: settings["pam/sudo"]
                onToggled: kcm.setValue("pam/sudo", checked)
            }
            Kirigami.ContextualHelpButton {
                toolTipText: i18n("sudo looks for your face before it asks for your password. Your face can then approve changes to the system, such as installing software. Someone who looks like you, or holds the computer up to your face, could do the same.")
            }
        }

        Item {
            Kirigami.FormData.isSection: true
        }

        // Fingerprints are read by fprintd and set up in Users; Kenning decides where they can be used.
        RowLayout {
            Kirigami.FormData.label: i18n("Your fingerprint:")
            spacing: Kirigami.Units.smallSpacing
            QQC2.Label {
                text: !kcm.fingerprintReader ? i18n("No fingerprint reader found")
                    : kcm.fingerprintEnrolled ? i18n("Set up in Users") : i18n("Not set up yet")
            }
            QQC2.Button {
                visible: kcm.fingerprintReader
                icon.name: kcm.fingerprintEnrolled ? "configure" : "list-add"
                text: kcm.fingerprintEnrolled ? i18nc("@action:button", "Manage…") : i18nc("@action:button", "Set Up…")
                onClicked: kcm.manageFingerprints()
            }
            Kirigami.ContextualHelpButton {
                visible: !kcm.fingerprintReader
                toolTipText: i18n("Fingerprints are read by fprintd. Install it, check that it supports your reader (fprintd-list), and open this page again.")
            }
        }
        RowLayout {
            Kirigami.FormData.label: i18n("Use your fingerprint for:")
            visible: kcm.fingerprintReader
            spacing: 0
            QQC2.CheckBox {
                text: i18n("Login screen")
                enabled: (kcm.loginScreenSupported && settings["pam/login"]) || settings["LoginScreen/Fingerprint"]
                checked: settings["LoginScreen/Fingerprint"]
                onToggled: kcm.setValue("LoginScreen/Fingerprint", checked)
            }
            Kirigami.ContextualHelpButton {
                toolTipText: !kcm.loginScreenSupported
                    ? i18n("Needs the Kenning version of Plasma Login Manager (plasma-login-manager-kenning).")
                    : !settings["pam/login"]
                    ? i18n("Uses the login screen's face unlock step, so switch on your face for the login screen first.")
                    : i18n("The login screen shows Use Fingerprint as a button. Your password always works.")
            }
        }
        RowLayout {
            visible: kcm.fingerprintReader
            spacing: 0
            QQC2.CheckBox {
                text: i18n("Lock screen")
                enabled: kcm.fingerprintLockScreenSupported || settings["pam/fingerprint"]
                checked: settings["pam/fingerprint"]
                onToggled: kcm.setValue("pam/fingerprint", checked)
            }
            Kirigami.ContextualHelpButton {
                toolTipText: kcm.fingerprintLockScreenSupported
                    ? i18n("The lock screen shows Fingerprint as a button next to Password, and reads your finger only once you choose it. Without this, Plasma reads the reader quietly while you type your password.")
                    : i18n("Needs Plasma 6.8 or newer, with Kenning's lock screen add-on applied.")
            }
        }
        RowLayout {
            visible: kcm.fingerprintReader
            spacing: 0
            QQC2.CheckBox {
                text: i18n("Administrator prompts")
                enabled: settings["Polkit/Agent"] || settings["polkit/fingerprint"]
                checked: settings["polkit/fingerprint"]
                onToggled: kcm.setValue("polkit/fingerprint", checked)
            }
            Kirigami.ContextualHelpButton {
                toolTipText: settings["Polkit/Agent"]
                    ? i18n("Administrator prompts show Fingerprint as a button next to the password. Your fingerprint can then approve changes to the system, such as installing software. Anyone who can put your finger on the reader could do the same.")
                    : i18n("Needs Kenning's dialog for administrator prompts, below.")
            }
        }
        RowLayout {
            visible: kcm.fingerprintReader
            spacing: 0
            QQC2.CheckBox {
                text: i18n("sudo in the terminal")
                checked: settings["pam/sudo-fingerprint"]
                onToggled: kcm.setValue("pam/sudo-fingerprint", checked)
            }
            Kirigami.ContextualHelpButton {
                toolTipText: i18n("sudo asks for your finger before your password, after your face if that is on for sudo too. Press Ctrl+C while it waits to type your password at once. Your fingerprint can then approve changes to the system, such as installing software. Anyone who can put your finger on the reader could do the same.")
            }
        }
        QQC2.SpinBox {
            Kirigami.FormData.label: i18n("Wait for a finger:")
            visible: kcm.fingerprintReader
            from: 5
            to: 120
            stepSize: 5
            value: settings["fingerprint/timeout"]
            textFromValue: (value, locale) => i18ncp("seconds", "%1 second", "%1 seconds", value)
            valueFromText: (text, locale) => parseInt(text)
            onValueModified: kcm.setValue("fingerprint/timeout", value)
        }
        RowLayout {
            Kirigami.FormData.label: i18n("Tries:")
            visible: kcm.fingerprintReader
            spacing: 0
            QQC2.SpinBox {
                from: 1
                to: 10
                value: settings["fingerprint/max_tries"]
                onValueModified: kcm.setValue("fingerprint/max_tries", value)
            }
            Kirigami.ContextualHelpButton {
                toolTipText: i18n("Fingers that aren't recognized before Kenning asks for your password instead. A touch that was too short or off-centre doesn't count.")
            }
        }

        Item {
            visible: kcm.fingerprintReader
            Kirigami.FormData.isSection: true
        }

        // Where face and fingerprint are both offered: the lock screen and administrator prompts.
        RowLayout {
            Kirigami.FormData.label: i18n("Start with:")
            visible: kcm.fingerprintReader
            spacing: 0
            QQC2.ComboBox {
                id: startMethod
                readonly property var methods: ["face", "fingerprint", "last"]
                model: [i18n("Face"), i18n("Fingerprint"), i18n("Whichever I used last time")]
                currentIndex: Math.max(0, methods.indexOf(settings["user/startMethod"]))
                onActivated: kcm.setValue("user/startMethod", methods[currentIndex])
            }
            Kirigami.ContextualHelpButton {
                toolTipText: startMethod.currentIndex === 2
                    ? i18n("The lock screen and administrator prompts start with face or fingerprint, whichever you used there last time. Your password always works.")
                    : startMethod.currentIndex === 1
                    ? i18n("The lock screen and administrator prompts read your finger first. On the lock screen, if your finger isn't read in time, it looks for your face. Your password always works.")
                    : i18n("The lock screen and administrator prompts look for your face first; choose Fingerprint or type your password instead at any time.")
            }
        }

        Item {
            Kirigami.FormData.isSection: true
        }

        RowLayout {
            Kirigami.FormData.label: i18n("Administrator prompts:")
            spacing: 0
            QQC2.CheckBox {
                text: i18n("Use Kenning's dialog")
                checked: settings["Polkit/Agent"]
                onToggled: kcm.setValue("Polkit/Agent", checked)
            }
            Kirigami.ContextualHelpButton {
                toolTipText: i18n("Plasma's dialog for administrator prompts, with Fingerprint and Face as buttons next to the password. It starts with the method chosen in Start with. Without it, administrator prompts only take your password.")
            }
        }

        Item {
            Kirigami.FormData.isSection: true
        }

        RowLayout {
            Kirigami.FormData.label: i18n("Lock screen:")
            spacing: 0
            QQC2.CheckBox {
                id: autoDismiss
                text: i18n("Unlock as soon as I'm recognized")
                enabled: settings["pam/lockscreen"] && kcm.lockScreenPatched
                checked: !settings["lock/faceUnlockShowButton"]
                onToggled: kcm.setValue("lock/faceUnlockShowButton", !checked)
            }
            Kirigami.ContextualHelpButton {
                toolTipText: kcm.lockScreenPatched
                    ? i18n("The screen unlocks as soon as the camera recognizes you, even if you only glanced at it. Without this, you press Unlock after you are recognized.")
                    : i18n("The lock screen animation and its options aren't available for this Plasma version. Face unlock still works on this lock screen: look at the camera, then press Unlock.")
            }
        }
        QQC2.SpinBox {
            Kirigami.FormData.label: i18n("Pause before unlocking:")
            enabled: settings["pam/lockscreen"] && kcm.lockScreenPatched && autoDismiss.checked
            from: 0
            to: 3000
            stepSize: 100
            value: settings["lock/unlockDelay"]
            textFromValue: (value, locale) => i18nc("milliseconds", "%1 ms", value)
            valueFromText: (text, locale) => parseInt(text)
            onValueModified: kcm.setValue("lock/unlockDelay", value)
        }


        Item {
            visible: kcm.loginScreenSupported
            Kirigami.FormData.isSection: true
        }
        QQC2.CheckBox {
            visible: kcm.loginScreenSupported
            Kirigami.FormData.label: i18n("Login screen:")
            text: i18n("Look for my face right away")
            enabled: settings["pam/login"]
            checked: settings["LoginScreen/AutoScan"]
            onToggled: kcm.setValue("LoginScreen/AutoScan", checked)
        }
        QQC2.SpinBox {
            visible: kcm.loginScreenSupported
            Kirigami.FormData.label: i18n("Show welcome for:")
            enabled: settings["pam/login"]
            from: 0
            to: 3000
            stepSize: 100
            value: settings["LoginScreen/WelcomePause"]
            textFromValue: (value, locale) => i18nc("milliseconds", "%1 ms", value)
            valueFromText: (text, locale) => parseInt(text)
            onValueModified: kcm.setValue("LoginScreen/WelcomePause", value)
        }

        Item {
            Kirigami.FormData.isSection: true
        }

        RowLayout {
            Kirigami.FormData.label: i18n("Animation:")
            QQC2.Label {
                text: root.selectedTheme ? root.selectedTheme.name : settings["Appearance/AnimationStyle"]
            }
            QQC2.Button {
                icon.name: "preferences-desktop-theme"
                text: i18nc("@action:button", "Choose…")
                onClicked: kcm.push("ThemesPage.qml")
            }
        }

        QQC2.Button {
            Kirigami.FormData.label: i18n("Recognition and security:")
            icon.name: "configure"
            text: i18nc("@action:button", "Configure…")
            onClicked: kcm.push("AdvancedPage.qml")
        }
    }
}
