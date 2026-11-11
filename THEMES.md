# Scan animation themes

A theme draws the face scan animation on the login screen, the lock screen, in
Kenning's polkit dialog and in System Settings > Face & Fingerprint. The same animation
shows a fingerprint being read, if the theme draws one (see `method` below). Themes are QML, like SDDM themes, so they run
on the login and lock screens: only install themes you trust.

## Layout

```
my-theme/
├── metadata.json
└── contents/ui/main.qml
```

`metadata.json`:

```json
{
    "KPlugin": {
        "Id": "my-theme",
        "Name": "My Theme",
        "Description": "One line shown as a tooltip in the theme picker.",
        "Authors": [{ "Name": "Your Name" }],
        "License": "LGPL-2.0-or-later",
        "Version": "1.0"
    },
    "X-Kenning-APIVersion": 1
}
```

`Id` is the folder name: letters, digits, `.`, `_` and `-`, starting with a
letter or digit.

## Theme API 1

`main.qml` has an `Item` at its root. The host sets these properties and
updates them as the scan progresses:

| Property | Type | Meaning |
|---|---|---|
| `faceState` | int | 0 idle, 1 scanning, 2 recognized, 3 failed |
| `message` | string | Text to show, e.g. "Looking for you…" or "Welcome, Rishon" |
| `highlightColor` | color | Accent colour for scanning |
| `successColor` | color | Colour for recognized |
| `failureColor` | color | Colour for failed |
| `textColor` | color | Colour for the message |

Declare each of them as a `property` in your root item.

A theme may also declare `property string method: "face"`. The host then sets it
to `"fingerprint"` while it waits for a finger, so the theme can draw a fingerprint
in place of a face. A theme without it shows its face animation for both. Set `implicitWidth` and
`implicitHeight`: the host uses them for layout and hides the theme while
`faceState` is 0. Show `message` somewhere, since it carries the reason a scan
failed. You may import `QtQuick`, `QtQuick.Layouts`, `QtQuick.Shapes`,
`org.kde.kirigami` and `org.kde.plasma.components`.

The packaged themes in `kcm/themes/` (`hello`, `ring`, `ripple`, `text`) are
complete examples.

## Installing

Pack the theme folder (`tar czf my-theme.tar.gz my-theme`) and use **Install
from File…** in System Settings > Face & Fingerprint. Installed themes go to
`/usr/local/share/kenning/themes`; packaged ones live in
`/usr/share/kenning/themes`. A theme in the first replaces a packaged theme
with the same id.
