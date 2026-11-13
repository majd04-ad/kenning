#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Rishon Jonathan R
# SPDX-License-Identifier: GPL-2.0-or-later
# Adds face unlock to the Plasma lock screen. Run by a pacman hook after plasma-desktop updates.
#
# usage: kenning-lockscreen.py [lockscreen dir] [FaceScanIndicator.qml]
#        kenning-lockscreen.py --restore [lockscreen dir]   put the stock files back

import glob
import os
import shutil
import subprocess
import sys
from pathlib import Path

# VERSION changes whenever the edits do, so an older patch is redone from the saved original.
MARKER = "kenning lock screen patch"
LEGACY_MARKERS = ("lazysetup face unlock", "plasma-faceunlock lock screen patch")
VERSION = "face unlock patch 18"

RESTORE = len(sys.argv) > 1 and sys.argv[1] == "--restore"
if RESTORE:
    sys.argv.pop(1)

lockscreen = Path(sys.argv[1] if len(sys.argv) > 1 else
                  "/usr/share/plasma/shells/org.kde.plasma.desktop/contents/lockscreen")  # see SYSTEM_LOCKSCREEN
indicator = Path(sys.argv[2] if len(sys.argv) > 2 else Path(__file__).with_name("FaceScanIndicator.qml"))

CONFIG_XML = ("  </group>", f"""    <!-- {MARKER}, {VERSION} -->
    <entry name="faceUnlockShowButton" type="Bool">
      <label>Ask for an Unlock button press after face recognition.</label>
      <default>false</default>
    </entry>
    <entry name="faceAnimation" type="String">
      <label>Face scan animation: hello, ring or text.</label>
      <default>hello</default>
    </entry>
    <entry name="unlockDelay" type="Int">
      <label>Milliseconds to show a greeting before unlocking after face recognition.</label>
      <default>500</default>
      <min>0</min>
      <max>3000</max>
    </entry>
    <entry name="preferredMethod" type="String">
      <label>Sign-in method to start with: face or fingerprint.</label>
      <default>face</default>
    </entry>
    <entry name="useLastMethod" type="Bool">
      <label>Start with the sign-in method used last instead of preferredMethod.</label>
      <default>false</default>
    </entry>
  </group>""")

CONFIG_QML = ("""        KCM.SettingHighlighter {
            highlight: configForm.cfg_showMediaControlsDefault != configForm.cfg_showMediaControls
        }
    }
""", f"""        KCM.SettingHighlighter {{
            highlight: configForm.cfg_showMediaControlsDefault != configForm.cfg_showMediaControls
        }}
    }}

    // {MARKER}, {VERSION}
    property alias cfg_faceUnlockShowButton: faceUnlockShowButton.checked
    property bool cfg_faceUnlockShowButtonDefault: false
    property alias cfg_unlockDelay: unlockDelay.value
    property int cfg_unlockDelayDefault: 500

    QQC2.CheckBox {{
        id: faceUnlockShowButton
        Kirigami.FormData.label: i18nc("@title: group", "Face unlock:")
        text: i18nc("@option:check", "Show an Unlock button after recognizing me")

        KCM.SettingHighlighter {{
            highlight: configForm.cfg_faceUnlockShowButtonDefault != configForm.cfg_faceUnlockShowButton
        }}
    }}

    QQC2.SpinBox {{
        id: unlockDelay
        Kirigami.FormData.label: i18nc("@label:spinbox", "Pause before unlocking:")
        enabled: !faceUnlockShowButton.checked
        from: 0
        to: 3000
        stepSize: 100
        textFromValue: (value, locale) => i18nc("@item:valuesuffix milliseconds", "%1 ms", value)
        valueFromText: (text, locale) => parseInt(text)

        KCM.SettingHighlighter {{
            highlight: configForm.cfg_unlockDelayDefault != configForm.cfg_unlockDelay
        }}
    }}
""")

UI_SUCCEEDED = ("""            if (authenticator.hadPrompt) {
                Qt.quit();
            } else {
                mainStack.replace(null, Qt.resolvedUrl("NoPasswordUnlock.qml"),""",
"""            if (authenticator.hadPrompt) {
                Qt.quit();
            } else if (!config.faceUnlockShowButton) {
                lockScreenUi.finishFaceUnlock();
            } else {
                mainStack.replace(null, Qt.resolvedUrl("NoPasswordUnlock.qml"),""")

UI_FUNCTION = ("""    property string pendingPassword
""", f"""    property string pendingPassword

    // {MARKER}, {VERSION}
    property int faceState: 0
    property string faceMessage
    // What the scan animation shows being read: "face", or "fingerprint" once Kenning's reader starts.
    property string scanMethod: "face"
    // False after a failure a new scan can't fix (lockout, no face model).
    property bool faceRetryable: true

    // Authenticator type to start with: 3 Plasma's Face method, 1 fingerprint, -1 the one used last.
    readonly property int preferredMethod: config.useLastMethod ? -1
                                           : config.preferredMethod === "fingerprint" ? 1 : 3
    property bool faceOffered: false
    signal switchMethod(int type)

    // Switching deletes the current authenticator, so wait for its signal to return.
    function switchMethodLater(type) {{
        Qt.callLater(() => lockScreenUi.switchMethod(type));
    }}

    // Plasma starts the fingerprint reader before its PAM worker is ready and doesn't retry.
    Timer {{
        property int tries: 0
        interval: 500
        repeat: true
        running: lockScreenUi.preferredMethod === 1
        onTriggered: {{
            if (++tries > 6 || authenticator.authenticator !== 1 || authenticator.unlocked) {{
                stop();
            }} else if (!authenticator.busy) {{
                authenticator.startAuthenticating();
            }}
        }}
    }}

    // Kenning's reason the last fingerprint failed, kept for the message after switching.
    property string fingerprintError

    // Fingerprint failed. Started first by preference: fall back to the face and the password.
    // Chosen by the user: back to the password, the animation saying why; the Fingerprint button tries again.
    function fingerprintFallback() {{
        const reason = fingerprintError;
        fingerprintError = "";
        fingerprintRetryTimer.stop();
        if (lockScreenUi.preferredMethod !== 1) {{
            scanMethod = "fingerprint";
            faceState = 3;
            faceMessage = reason || i18nc("@info:status", "Fingerprint not recognized");
            faceResetTimer.restart();
        }}
        Qt.callLater(() => {{
            if (lockScreenUi.preferredMethod === 1) {{
                lockScreenUi.switchMethod(lockScreenUi.faceOffered ? 3 : 0);
                lockScreenUi.handleMessage(i18nc("@info:status", "Fingerprint not recognized. Look at the camera or type your password."));
            }} else {{
                lockScreenUi.switchMethod(0);
            }}
        }});
    }}

    // Kenning's reader asked for the finger: the scan animation, with a finger.
    function startFingerprint() {{
        faceResetTimer.stop();
        fingerprintRetryTimer.stop();
        fingerprintError = "";
        scanMethod = "fingerprint";
        faceState = 1;
        faceMessage = i18nc("@info:status", "Place your finger on the fingerprint reader");
        lockScreenRoot.uiVisible = true;
    }}

    // What the reader says while it waits; a touch to repeat flashes the failure look.
    function handleFingerprintMessage(msg) {{
        if (scanMethod !== "fingerprint" || authenticator.authenticator !== 1 || faceState === 0 || !msg) {{
            return false;
        }}
        faceMessage = msg;
        if (/again/.test(msg)) {{
            faceState = 3;
            fingerprintRetryTimer.restart();
        }} else {{
            faceState = 1;
        }}
        return true;
    }}

    // Another method chosen while the reader waits: the animation goes.
    readonly property int currentAuthenticator: authenticator.authenticator ?? -1
    onCurrentAuthenticatorChanged: {{
        if (scanMethod === "fingerprint" && currentAuthenticator !== 1 && (faceState === 1 || fingerprintRetryTimer.running)) {{
            fingerprintRetryTimer.stop();
            faceState = 0;
        }}
    }}

    Timer {{
        id: fingerprintRetryTimer
        interval: 900
        onTriggered: {{
            if (lockScreenUi.scanMethod === "fingerprint" && lockScreenUi.faceState === 3 && authenticator.authenticator === 1) {{
                lockScreenUi.faceState = 1;
            }}
        }}
    }}

    // Plasma's Face method failed: fall back to the password instead of Plasma's immediate retry.
    function faceFallback() {{
        if (faceState !== 3) {{
            handleFaceMessage("Failure, general abort");
        }}
        faceNeedsRetry = true;
        switchMethodLater(0);
    }}

    function handleFaceMessage(msg) {{
        if (msg.startsWith("Attempting facial authentication")) {{
            scanMethod = "face";
            faceState = 1;
            faceRetryable = true;
            faceMessage = i18nc("@info:status", "Looking for you…");
            lockScreenRoot.uiVisible = true;
            return true;
        }}
        if (msg.startsWith("Identified face as")) {{
            faceState = 2;
            faceMessage = i18nc("@info:status", "Welcome back");
            return true;
        }}
        if (/^(Failure, |Face detection image too dark)/.test(msg)) {{
            faceState = 3;
            faceRetryable = !msg.includes("too many attempts") && !msg.includes("no face model");
            faceMessage = msg.includes("too dark") ? i18nc("@info:status", "Too dark to see you")
                        : msg.includes("too bright") ? i18nc("@info:status", "Too bright to see you")
                        : msg.includes("camera") ? i18nc("@info:status", "The camera is off or in use")
                        : msg.includes("too many attempts") ? i18nc("@info:status", "Too many attempts. Use your password.")
                        : msg.includes("no face model") ? i18nc("@info:status", "Face unlock isn't set up")
                        : i18nc("@info:status", "Couldn't recognize you");
            faceResetTimer.restart();
            faceNeedsRetry = true;
            return true;
        }}
        return false;
    }}

    Timer {{
        id: faceResetTimer
        interval: 3000
        onTriggered: lockScreenUi.faceState = 0
    }}

    // pam_howdy runs once per PAM conversation, so a new scan needs a fresh one. Never while typing.
    property bool faceNeedsRetry: false

    // Returns whether a new scan was started.
    function rescanFace() {{
        if (authenticator.unlocked || mainBlock.mainPasswordBox.text.length > 0) {{
            return false;
        }}
        faceNeedsRetry = false;
        if (authenticator.authenticator === 0 && faceOffered) {{
            switchMethod(3);
            return true;
        }}
        authenticator.stopAuthenticating();
        authenticator.startAuthenticating();
        return true;
    }}

    Timer {{
        id: resumeRescanTimer
        interval: 1500
        onTriggered: lockScreenUi.rescanFace()
    }}

    Connections {{
        target: sessionManagement
        function onResumingFromSuspend() {{
            resumeRescanTimer.restart();
        }}
    }}

    function finishFaceUnlock() {{
        faceState = 2;
        faceMessage = i18nc("@info:status", "Welcome back");
        if (config.unlockDelay > 0) {{
            faceUnlockTimer.start();
        }} else {{
            Qt.quit();
        }}
    }}

    Timer {{
        id: faceUnlockTimer
        interval: config.unlockDelay
        onTriggered: Qt.quit()
    }}
""")

UI_FALLBACK = ("""        function onFailed(kind) {
            if (kind != 0) { // if this is coming from the noninteractive authenticators
                return;
            }
""", """        function onFailed(kind) {
            if (kind != 0) { // if this is coming from the noninteractive authenticators
                return;
            }
            // Unlocked already: a worker ended with the lock screen, not a method that failed.
            if (authenticator.unlocked) {
                return;
            }
            if (authenticator.authenticator === 3) {
                lockScreenUi.faceFallback();
                return;
            }
            if (authenticator.authenticator === 1) {
                lockScreenUi.fingerprintFallback();
                return;
            }
""")

UI_METHOD = ("""                            Component.onCompleted: {
                                if (checked) {
                                    clicked()
                                }
                            }
""", """                            Component.onCompleted: {
                                if (type === 3) {
                                    lockScreenUi.faceOffered = functional
                                }
                                if (type === lockScreenUi.preferredMethod) {
                                    clicked()
                                } else if (checked) {
                                    // After every button exists, unless the preferred one took over.
                                    Qt.callLater(() => {
                                        if (authenticator.authenticator === type) {
                                            clicked()
                                        }
                                    })
                                }
                            }

                            Connections {
                                target: lockScreenUi
                                function onSwitchMethod(to) {
                                    if (to === type && functional) {
                                        clicked()
                                    }
                                }
                            }
""")

# Plasma's method buttons show only an icon; named, Password, Fingerprint and Face tell apart at a glance.
UI_METHOD_NAMES = ("""                            icon.name: iconName
""", """                            icon.name: iconName
                            text: tooltip
""")

# A method that stops working falls back: the face quietly to the password, fingerprint to the face.
UI_DEFUNCT = ("""                            onFunctionalChanged: {
                                if (functional) {
                                    return
                                }
""", """                            onFunctionalChanged: {
                                if (functional) {
                                    return
                                }
                                if (type === 3) {
                                    lockScreenUi.faceOffered = false
                                    if (authenticator.authenticator === 3) {
                                        lockScreenUi.switchMethodLater(0)
                                        return
                                    }
                                }
                                if (type === 1 && authenticator.authenticator === 1 && lockScreenUi.preferredMethod === 1) {
                                    lockScreenUi.fingerprintFallback()
                                    return
                                }
""")

UI_FACE_SUCCEEDED = ("""            if (!lockScreenUi.expectingPrompt) {
                Qt.quit()
            }
""", """            // Plasma's Face method: the greeting and pause, or the Unlock button (System Settings).
            if (authenticator.authenticator === 3) {
                if (config.faceUnlockShowButton) {
                    mainStack.replace(null, Qt.resolvedUrl("NoPasswordUnlock.qml"), { userListModel: users }, StackView.Immediate);
                    mainStack.forceActiveFocus();
                } else {
                    lockScreenUi.finishFaceUnlock();
                }
                return;
            }
            // Kenning's fingerprint: the same greeting and pause.
            if (authenticator.authenticator === 1 && lockScreenUi.scanMethod === "fingerprint" && lockScreenUi.faceState !== 0) {
                lockScreenUi.finishFaceUnlock();
                return;
            }
            if (!lockScreenUi.expectingPrompt) {
                Qt.quit()
            }
""")

UI_WAKE = ("""            } else if (uiVisible) {
                fadeoutTimer.restart();
            }
            authenticator.startAuthenticating();""", """            } else if (uiVisible) {
                fadeoutTimer.restart();
            }
            if (!(uiVisible && lockScreenUi.faceNeedsRetry && lockScreenUi.rescanFace())) {
                authenticator.startAuthenticating();
            }""")

# Typing a password drops the pending face retry, so the keypress that wakes the screen doesn't scan.
UI_KEYS = ("""        Keys.onPressed: event => {
            uiVisible = true;""", """        Keys.onPressed: event => {
            if (event.text.trim().length > 0) {
                lockScreenUi.faceNeedsRetry = false;
                // Typing while the Face or Fingerprint method waits means a password: switch to it, keeping the key.
                if (authenticator.authenticator === 3 || authenticator.authenticator === 1) {
                    lockScreenUi.faceState = 0;
                    lockScreenUi.switchMethod(0);
                    mainBlock.mainPasswordBox.text += event.text;
                    mainBlock.mainPasswordBox.forceActiveFocus();
                    uiVisible = true;
                    event.accepted = true;
                    return;
                }
            }
            uiVisible = true;""")

# A password submitted before its prompt is there would be dropped: during the scan (PAM is in
# the face module), or typed and sent faster than the password method starts after a switch. Hold it.
UI_PASSWORD = ("""                    if (authenticator.pamTimeout) {
                        lockScreenUi.pendingPassword = password""", """                    if (authenticator.pamTimeout || (lockScreenUi.faceState === 1 && lockScreenUi.scanMethod === "face")
                            || (authenticator.authenticator === 0 && !authenticator.promptForSecret)) {
                        lockScreenUi.pendingPassword = password""")

UI_PROMPT = ("""        function onPromptForSecretChanged() {
            mainBlock.showPassword = false;""", """        function onPromptForSecretChanged() {
            // Also sent, empty, when the method changes: only a real prompt takes the held password,
            // and only once this handler has returned (the greeter drops an answer given before).
            if (!authenticator.pamTimeout && lockScreenUi.pendingPassword.length > 0 && authenticator.promptForSecret) {
                const held = lockScreenUi.pendingPassword;
                lockScreenUi.pendingPassword = "";
                mainBlock.enabled = true;
                Qt.callLater(() => authenticator.respond(held));
                return;
            }
            mainBlock.showPassword = false;""")

UI_FAILED = ("""            const msg = i18ndc("plasma_shell_org.kde.plasma.desktop", "@info:status", "Unlocking failed");
            lockScreenUi.handleMessage(msg);""",
"""            if (lockScreenUi.faceState === 1 && lockScreenUi.scanMethod === "face") {
                lockScreenUi.handleFaceMessage("Failure, general abort");
            } else {
                const msg = i18ndc("plasma_shell_org.kde.plasma.desktop", "@info:status", "Unlocking failed");
                lockScreenUi.handleMessage(msg);
            }""")

UI_INFO = ("""        function onInfoMessageChanged() {
            lockScreenUi.handleMessage(authenticator.infoMessage);
        }

        function onErrorMessageChanged() {
            lockScreenUi.handleMessage(authenticator.errorMessage);
        }""",
"""        function onInfoMessageChanged() {
            if (!lockScreenUi.handleFingerprintMessage(authenticator.infoMessage)
                && !lockScreenUi.handleFaceMessage(authenticator.infoMessage)) {
                lockScreenUi.handleMessage(authenticator.infoMessage);
            }
        }

        function onErrorMessageChanged() {
            if (authenticator.authenticator === 1 && authenticator.errorMessage) {
                lockScreenUi.fingerprintError = authenticator.errorMessage;
                // Kenning's reader: shown by the scan animation when the method gives up.
                if (lockScreenUi.scanMethod === "fingerprint" && lockScreenUi.faceState !== 0) {
                    return;
                }
            }
            if (!lockScreenUi.handleFaceMessage(authenticator.errorMessage)) {
                lockScreenUi.handleMessage(authenticator.errorMessage);
            }
        }""")

# Kenning's fingerprint service asks before reading the finger. Only the method the user chose gets
# here: Plasma never passes on prompts from the fingerprint service it runs unasked beside the others.
UI_FINGERPRINT_START = ("""        function onPromptChanged() {
            lockScreenUi.handleMessage(authenticator.prompt);
        }""", """        function onPromptChanged() {
            if (authenticator.prompt === "kenning-start-fingerprint") {
                lockScreenUi.startFingerprint();
                // Once this handler has returned: the greeter only takes an answer after it has
                // told everyone about the prompt, and drops one given while it still does.
                Qt.callLater(() => {
                    if (authenticator.authenticator === 1 && authenticator.prompt === "kenning-start-fingerprint") {
                        authenticator.respond("start");
                    }
                });
                return;
            }
            lockScreenUi.handleMessage(authenticator.prompt);
        }""")

UI_INDICATOR = ("""                authenticationTypeItem: RowLayout {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.fillWidth: true
                    visible: authenticationTypeRepeater.count > 1
""", """                authenticationTypeItem: ColumnLayout {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.fillWidth: true

                    FaceScanIndicator {
                        faceState: lockScreenUi.faceState
                        message: lockScreenUi.faceMessage
                        method: lockScreenUi.scanMethod
                        style: config.faceAnimation || "hello"
                    }

                    PlasmaComponents3.Label {
                        Layout.alignment: Qt.AlignHCenter
                        visible: lockScreenUi.faceNeedsRetry && lockScreenUi.faceState === 0 && lockScreenUi.faceMessage !== ""
                                 && mainBlock.mainPasswordBox.text.length === 0
                        text: lockScreenUi.faceMessage
                        textFormat: Text.PlainText
                    }

                    PlasmaComponents3.Button {
                        Layout.alignment: Qt.AlignHCenter
                        visible: lockScreenUi.faceNeedsRetry && lockScreenUi.faceRetryable && lockScreenUi.faceState !== 1
                                 && mainBlock.mainPasswordBox.text.length === 0
                        icon.name: "view-refresh"
                        text: i18nc("@action:button", "Try Face Again")
                        onClicked: lockScreenUi.rescanFace()
                    }

                    RowLayout {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.fillWidth: true
                    visible: authenticationTypeRepeater.count > 1
""")

UI_INDICATOR_END = ("""                    }
                }

                notificationMessage: {""", """                    }
                }
                }

                notificationMessage: {""")

# Plasma before 6.8: face unlock runs in the password service, before its prompt. These
# replace the 6.8 edits that don't fit; the rest stay idle without authenticator.authenticator.

UI_FUNCTION_BEFORE_6_8 = ("""    Kirigami.Theme.inherit: false
""", UI_FUNCTION[1] + """
    Kirigami.Theme.inherit: false
""")

# Hold a password submitted during the scan, and put the scan animation under the password field.
UI_PASSWORD_BEFORE_6_8 = ("""                onPasswordResult: password => {
                    authenticator.respond(password)
                }
""", """                onPasswordResult: password => {
                    if (lockScreenUi.faceState === 1) {
                        lockScreenUi.pendingPassword = password;
                        return;
                    }
                    authenticator.respond(password)
                }

                FaceScanIndicator {
                    Layout.alignment: Qt.AlignHCenter
                    faceState: lockScreenUi.faceState
                    message: lockScreenUi.faceMessage
                    style: config.faceAnimation || "hello"
                }

                PlasmaComponents3.Label {
                    Layout.alignment: Qt.AlignHCenter
                    visible: lockScreenUi.faceNeedsRetry && lockScreenUi.faceState === 0 && lockScreenUi.faceMessage !== ""
                             && mainBlock.mainPasswordBox.text.length === 0
                    text: lockScreenUi.faceMessage
                    textFormat: Text.PlainText
                }
""")

UI_PROMPT_BEFORE_6_8 = ("""        function onPromptForSecretChanged(msg) {
            mainBlock.showPassword = false;""", """        function onPromptForSecretChanged(msg) {
            if (lockScreenUi.pendingPassword.length > 0) {
                authenticator.respond(lockScreenUi.pendingPassword);
                lockScreenUi.pendingPassword = "";
                return;
            }
            mainBlock.showPassword = false;""")

# Plasma 6.3 to 6.5: an older settings dialog and message call.
CONFIG_QML_6_3 = (CONFIG_QML[0].replace("configForm.", ""), CONFIG_QML[1].replace("configForm.", ""))
_I18N_6_8 = 'i18ndc("plasma_shell_org.kde.plasma.desktop", "@info:status", '
_I18N_6_3 = 'i18nd("plasma_shell_org.kde.plasma.desktop", '
UI_FAILED_6_3 = (UI_FAILED[0].replace(_I18N_6_8, _I18N_6_3), UI_FAILED[1].replace(_I18N_6_8, _I18N_6_3))

_UI_BEFORE_6_8 = [UI_SUCCEEDED, UI_FUNCTION_BEFORE_6_8, UI_INFO, UI_WAKE, UI_KEYS, UI_PASSWORD_BEFORE_6_8,
                  UI_PROMPT_BEFORE_6_8, UI_FALLBACK]

# Edit sets by Plasma version, newest first; the first whose edits all fit is used.
EDIT_SETS = [
    ("6.8", {
        "config.xml": [CONFIG_XML],
        "config.qml": [CONFIG_QML],
        "LockScreenUi.qml": [UI_SUCCEEDED, UI_FUNCTION, UI_FAILED, UI_INFO, UI_INDICATOR, UI_INDICATOR_END, UI_WAKE, UI_KEYS,
                             UI_PASSWORD, UI_PROMPT, UI_FALLBACK, UI_METHOD, UI_METHOD_NAMES, UI_DEFUNCT, UI_FACE_SUCCEEDED,
                             UI_FINGERPRINT_START],
    }),
    ("6.6", {
        "config.xml": [CONFIG_XML],
        "config.qml": [CONFIG_QML],
        "LockScreenUi.qml": _UI_BEFORE_6_8 + [UI_FAILED],
    }),
    ("6.3", {
        "config.xml": [CONFIG_XML],
        "config.qml": [CONFIG_QML_6_3],
        "LockScreenUi.qml": _UI_BEFORE_6_8 + [UI_FAILED_6_3],
    }),
]
FILES = list(EDIT_SETS[0][1])


SYSTEM_LOCKSCREEN = "/usr/share/plasma/shells/org.kde.plasma.desktop/contents/lockscreen"


def is_patched(text: str) -> bool:
    return MARKER in text or any(marker in text for marker in LEGACY_MARKERS)


# Unpatched copies, replaced whenever the package manager writes fresh files.
ORIGINALS = Path(os.environ.get("KENNING_ORIGINALS", "/var/lib/kenning/lockscreen-originals"))


def original(name: str) -> str | None:
    """The unpatched file: the copy saved before patching, or on Arch, from the
    package cache (installs patched by releases that didn't save copies)."""
    saved = ORIGINALS / name
    if saved.is_file():
        return saved.read_text()
    if not shutil.which("pacman"):
        return None
    owner = subprocess.run(["pacman", "-Qo", f"{SYSTEM_LOCKSCREEN}/{name}"], capture_output=True, text=True).stdout.split()
    if len(owner) < 2:
        return None
    package_name, version = owner[-2], owner[-1]
    member = f"{SYSTEM_LOCKSCREEN.lstrip('/')}/{name}"
    for package in glob.glob(f"/var/cache/pacman/pkg/{package_name}-{version}-*.pkg.tar.*"):
        if package.endswith(".sig"):
            continue
        result = subprocess.run(["bsdtar", "-xOf", package, member], capture_output=True, text=True)
        if result.returncode == 0 and result.stdout:
            return result.stdout
    return None


def restore() -> int:
    for name in FILES:
        if not is_patched((lockscreen / name).read_text()):
            continue
        text = original(name)
        if text is None:
            print(f"kenning lock screen: no saved original of {name}; reinstall the package that provides it "
                  "(plasma-desktop, or plasma-desktop-data on Debian and Ubuntu)", file=sys.stderr)
            continue
        (lockscreen / name).write_text(text)
        print(f"kenning lock screen: restored {name}")
    (lockscreen / "FaceScanIndicator.qml").unlink(missing_ok=True)
    shutil.rmtree(ORIGINALS, ignore_errors=True)
    try:
        ORIGINALS.parent.rmdir()  # /var/lib/kenning, unless something else is still kept there
    except OSError:
        pass
    return 0


def main() -> int:
    if not (lockscreen / "LockScreenUi.qml").is_file():
        print(f"kenning lock screen: no Plasma lock screen in {lockscreen}; nothing to do")
        return 0
    if RESTORE:
        return restore()
    current = {name: (lockscreen / name).read_text() for name in FILES}
    if all(VERSION in text for text in current.values()):
        print("kenning lock screen: already patched")
        edits = None
    else:
        stock = {}
        for name, text in current.items():
            if is_patched(text):
                text = original(name)
                if text is None:
                    print(f"kenning lock screen: {name} has an older patch and no saved original; reinstall the "
                          "package that provides it to update it", file=sys.stderr)
                    return 0
            stock[name] = text
        # All or nothing: a Plasma version no edit set fits keeps its stock lock screen.
        fits = [(version, edits) for version, edits in EDIT_SETS
                if all(stock[name].count(old) == 1 for name, file_edits in edits.items() for old, _ in file_edits)]
        if not fits:
            print("kenning lock screen: LockScreenUi.qml doesn't match a Plasma version this add-on supports "
                  "(6.3 or newer); leaving the lock screen stock", file=sys.stderr)
            return 0
        version, edits = fits[0]
        patched = {}
        for name, file_edits in edits.items():
            text = stock[name]
            for old, new in file_edits:
                if text.count(old) != 1:
                    print(f"kenning lock screen: {name} edits overlap for Plasma {version}; leaving the lock screen "
                          "stock", file=sys.stderr)
                    return 0
                text = text.replace(old, new)
            patched[name] = text

    if not indicator.is_file():
        print(f"kenning lock screen: {indicator} missing; leaving the lock screen stock", file=sys.stderr)
        return 0
    shutil.copyfile(indicator, lockscreen / "FaceScanIndicator.qml")

    if edits is None:
        return 0
    ORIGINALS.mkdir(parents=True, exist_ok=True)
    for name, text in stock.items():
        (ORIGINALS / name).write_text(text)
    for name, text in patched.items():
        (lockscreen / name).write_text(text)
    print(f"kenning lock screen: patched {', '.join(patched)} (Plasma {version} lock screen)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
