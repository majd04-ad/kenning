#!/bin/bash
# SPDX-FileCopyrightText: 2026 Rishon Jonathan R
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Inside a distribution container, with the source at /src: installs the Plasma lock
# screen, runs the real greeter offscreen on the stock lock screen and then on the one
# the add-on patched, and fails if the patched one has QML errors the stock one
# didn't. Shows that the add-on's edits load on this Plasma version, which patching
# cleanly alone doesn't. Prints "RESULT <test>: PASS|FAIL <detail>".
set -u
result() { echo "RESULT $1: $2${3:+ ($3)}"; }
. /etc/os-release
lockscreen=/usr/share/plasma/shells/org.kde.plasma.desktop/contents/lockscreen

case "$ID" in
debian | ubuntu)
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq && apt-get install -y -qq --no-install-recommends libkscreenlocker6 plasma-desktop plasma-workspace dbus-bin dbus-daemon python3 \
        > /tmp/install.log 2>&1
    ;;
fedora)
    dnf install -y -q --setopt=install_weak_deps=False kscreenlocker plasma-desktop plasma-workspace python3 /usr/bin/dbus-run-session > /tmp/install.log 2>&1
    ;;
arch)
    pacman -Syu --noconfirm --needed kscreenlocker plasma-desktop plasma-workspace python > /tmp/install.log 2>&1
    ;;
esac
[[ $? -eq 0 && -f $lockscreen/LockScreenUi.qml ]] || { result "install lock screen" FAIL; tail -5 /tmp/install.log; exit 1; }
greeter=$(find /usr/lib /usr/libexec -name kscreenlocker_greet -type f 2> /dev/null | head -1)
[[ -n $greeter ]] || { result "find greeter" FAIL; exit 1; }
plasma=$(grep -o "6\.[0-9]*\.[0-9]*" <<< "$(dpkg -s plasma-desktop 2> /dev/null || rpm -q plasma-desktop 2> /dev/null || pacman -Q plasma-desktop)" | head -1)
result "install lock screen" PASS "Plasma $plasma"

# Runs the greeter for a few seconds and prints the QML problems it reports, with
# line numbers and addresses stripped so the stock and patched runs compare.
qml_problems() { # log file
    runuser -u tester -- env QT_QPA_PLATFORM=offscreen QT_FORCE_STDERR_LOGGING=1 XDG_RUNTIME_DIR=/tmp/runtime \
        timeout 10 dbus-run-session -- "$greeter" --testing > "$1" 2>&1
    grep -E "\.qml:[0-9]+|TypeError|ReferenceError|is not a type|Cannot assign|Unable to assign|is not defined|Component is not ready" "$1" |
        grep -v "has no notify signal" | sed -E 's/:[0-9]+(:[0-9]+)?:/:/; s/0x[0-9a-f]+/0x/g' | sort -u
}
# The greeter runs as an ordinary user, as it does in a session.
useradd -m tester 2> /dev/null
mkdir -p /tmp/runtime && chmod 700 /tmp/runtime && chown tester /tmp/runtime
command -v dbus-run-session > /dev/null || { result "greeter" FAIL "no dbus-run-session"; exit 1; }

qml_problems /tmp/stock.log > /tmp/stock.txt
out=$(python3 -I /src/lockscreen/kenning-lockscreen.py "$lockscreen" /src/kcm/src/ui/FaceScanIndicator.qml 2>&1)
if grep -q "patched" <<< "$out"; then
    result "patch" PASS "${out##*(}"
else
    result "patch" FAIL "$out"
    exit 1
fi
mkdir -p /usr/share/kenning
cp -r /src/kcm/themes /usr/share/kenning/
qml_problems /tmp/patched.log > /tmp/patched.txt
# Proof the patched lock screen actually loaded, rather than the greeter failing before
# it got that far: a user-installed theme (looked for first) that says when it's created.
# A separate run, so the comparison below sees the real built-in themes.
marker=/usr/local/share/kenning/themes/hello/contents/ui
mkdir -p "$marker"
# console.warn, not console.log: Fedora's Qt switches debug messages off (*.debug=false).
printf 'import QtQuick\nItem { property int faceState; property string message; property color highlightColor; property color successColor; property color failureColor; property color textColor; Component.onCompleted: console.warn("kenning-theme-loaded") }\n' > "$marker/main.qml"
qml_problems /tmp/marker.log > /dev/null
rm -rf /usr/local/share/kenning
if grep -q "kenning-theme-loaded" /tmp/marker.log; then
    result "patched lock screen ran" PASS
else
    result "patched lock screen ran" FAIL "no sign of the scan animation loading"
    tail -15 /tmp/marker.log | sed 's/^/   /'
fi

new=$(comm -13 /tmp/stock.txt /tmp/patched.txt)
if [[ -z $new ]]; then
    result "patched lock screen loads" PASS "$(wc -l < /tmp/stock.txt) warnings, same as stock"
else
    result "patched lock screen loads" FAIL "new QML problems:"
    sed 's/^/   /' <<< "$new"
fi
