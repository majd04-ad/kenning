#!/bin/bash
# SPDX-FileCopyrightText: 2026 Rishon Jonathan R
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Builds and tests Kenning inside a distribution container (see run.sh). The
# source is mounted read-only at /src. No camera: covers building, loading the
# networks with the distribution's OpenCV, the service protocol and access rules,
# the PAM module and pam-tidy. Prints "RESULT <test>: PASS|FAIL|SKIP <detail>".
set -u

result() { echo "RESULT $1: $2${3:+ ($3)}"; }
. /etc/os-release
echo "== $PRETTY_NAME"

# --- dependencies ---------------------------------------------------------------
# A cached image (run.sh) has them already, along with the last run's build.
if [[ ${KENNING_DEPS_INSTALLED:-} == 1 ]]; then
    rm -rf /tmp/build /tmp/kenning /tmp/pamcopy /tmp/kdir
    true
else
case "$ID" in
debian | ubuntu)
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq && apt-get install -y -qq --no-install-recommends \
        build-essential cmake extra-cmake-modules pkg-config gettext python3 \
        qt6-base-dev qt6-declarative-dev libopencv-dev libpam0g-dev libsystemd-dev libarchive-dev \
        libkf6auth-dev libkf6config-dev libkf6coreaddons-dev libkf6i18n-dev libkf6kcmutils-dev libkirigami-dev \
        libkf6dbusaddons-dev libkf6windowsystem-dev libpolkit-qt6-1-dev \
        > /tmp/deps.log 2>&1
    ;;
fedora)
    dnf install -y -q --setopt=install_weak_deps=False gcc-c++ cmake extra-cmake-modules pkgconf gettext python3 util-linux \
        qt6-qtbase-devel qt6-qtdeclarative-devel opencv-devel pam-devel systemd-devel libarchive-devel \
        kf6-kauth-devel kf6-kconfig-devel kf6-kcoreaddons-devel kf6-ki18n-devel kf6-kcmutils-devel kf6-kirigami-devel \
        kf6-kdbusaddons-devel kf6-kwindowsystem-devel polkit-qt6-1-devel \
        > /tmp/deps.log 2>&1
    ;;
opensuse-tumbleweed | opensuse*)
    zypper -n -q install -y --no-recommends gcc-c++ cmake kf6-extra-cmake-modules pkgconf gettext-tools python3 \
        qt6-base-devel qt6-declarative-devel opencv-devel pam-devel systemd-devel \
        kf6-kauth-devel kf6-kconfig-devel kf6-kcoreaddons-devel kf6-ki18n-devel kf6-kcmutils-devel kf6-kirigami-devel \
        kf6-kdbusaddons-devel kf6-kwindowsystem-devel polkit-qt6-1-devel \
        > /tmp/deps.log 2>&1
    ;;
arch)
    pacman -Syu --noconfirm --needed base-devel cmake extra-cmake-modules python \
        qt6-base qt6-declarative opencv pam systemd libarchive kauth kconfig kcoreaddons ki18n kcmutils kirigami \
        kdbusaddons kwindowsystem polkit-qt6 \
        > /tmp/deps.log 2>&1
    ;;
*)
    result dependencies FAIL "unknown distribution $ID"
    exit 1
    ;;
esac
fi
deps=$?
[[ $deps -eq 0 ]] && result dependencies PASS || { result dependencies FAIL "see deps.log"; tail -15 /tmp/deps.log; exit 1; }
opencv=$(for m in opencv5 opencv4 opencv; do pkg-config --modversion $m 2>/dev/null && break; done)
echo "   OpenCV ${opencv:-unknown}, Qt $(pkg-config --modversion Qt6Core 2>/dev/null)"

# --- build ----------------------------------------------------------------------
T=/tmp/kenning
mkdir -p $T/etc/models $T/share/models $T/pam
# One top-level build, as the packages do; the engine is built and checked first.
if cmake -S /src -B /tmp/build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DKENNING_NETWORKS_DIR=/src \
        -DKENNING_SYSCONFDIR=$T/etc -DKENNING_DATADIR=$T/share -DKENNING_SOCKET=$T/k.sock > /tmp/build.log 2>&1 &&
    cmake --build /tmp/build -j"$(nproc)" --target kenning pam_kenning >> /tmp/build.log 2>&1; then
    result "build engine" PASS
else
    result "build engine" FAIL
    grep -m5 -E "error|Error" /tmp/build.log
fi

if cmake --build /tmp/build -j"$(nproc)" >> /tmp/build.log 2>&1; then
    result "build settings page" PASS
else
    result "build settings page" FAIL
    grep -m5 -E "error|Error|Could not find" /tmp/build.log
fi

K=/tmp/build/engine/native/kenning
[[ -x $K ]] || exit 1
cp /src/engine/native/config.ini $T/etc/config.ini
cp /src/*.onnx $T/share/models/ 2>/dev/null

# --- networks ---------------------------------------------------------------------
# No camera, so the scan stops at opening it; the networks still load in parallel.
# A scan only starts for someone with a face model: use a dummy one for root.
python3 -c "import json; print(json.dumps([{'id': 0, 'label': 'dummy', 'time': 0, 'data': [[0.0] * 128]}]))" > $T/etc/models/root.dat
out=$($K test 2>&1)
rm -f $T/etc/models/root.dat
if [[ $? -lt 128 ]] && grep -q "not possible to open camera" <<< "$out"; then
    result "load networks" PASS "OpenCV $opencv"
else
    result "load networks" FAIL "$(tail -2 <<< "$out" | tr '\n' ' ')"
fi

# --- service ----------------------------------------------------------------------
ask() { # user-to-run-as request
    runuser -u "$1" -- python3 -I -c "
import socket, sys
s = socket.socket(socket.AF_UNIX); s.settimeout(10); s.connect('$T/k.sock')
s.sendall(sys.argv[1].encode()); print(s.recv(16).decode().strip())" "$2" 2>&1
}
chmod 755 /tmp $T
$K daemon & daemon=$!
sleep 1
chmod 666 $T/k.sock 2>/dev/null
check() { # name expected actual
    [[ $3 == "$2" ]] && result "$1" PASS || result "$1" FAIL "expected $2, got $3"
}
# No login session here, as right after a restart: refused until a password sign-in.
check "service: password first after a restart" 16 "$(ask root $'has-model root\n')"
check "service: login screen asks password-first" 16 "$(ask nobody $'password-first root\n')"
check "service: password-first, unknown user" 12 "$(ask nobody $'password-first no-such-user\n')"
sed -i 's/^password_after_boot *=.*/password_after_boot = false/' $T/etc/config.ini
check "service: password-first when not required" 0 "$(ask nobody $'password-first root\n')"
check "service: has-model, no model" 10 "$(ask root $'has-model root\n')"
check "service: compare, no model" 10 "$(ask root $'compare root\n')"
check "service: malformed request" 12 "$(ask root $'bogus\n')"
check "service: other user's face refused" 12 "$(ask nobody $'has-model root\n')"

# --- PAM module -------------------------------------------------------------------
cat > /tmp/pamharness.c <<'C'
#include <security/pam_appl.h>
#include <stdio.h>
#include <stdlib.h>
static int conv(int n, const struct pam_message **msg, struct pam_response **resp, void *d) {
    *resp = calloc(n, sizeof(struct pam_response));
    for (int i = 0; i < n; i++) printf("message: %s\n", msg[i]->msg);
    return PAM_SUCCESS;
}
int main(int argc, char **argv) {
    struct pam_conv c = {conv, NULL}; pam_handle_t *h;
    if (pam_start_confdir("kenning-test", argv[1], &c, argv[2], &h) != PAM_SUCCESS) return 2;
    int r = pam_authenticate(h, 0);
    printf("result: %s\n", pam_strerror(h, r)); pam_end(h, r); return 0;
}
C
if gcc -o /tmp/pamharness /tmp/pamharness.c -lpam 2> /tmp/harness.log; then
    printf 'auth sufficient /tmp/build/engine/native/pam_kenning.so\nauth required pam_deny.so\n' > $T/pam/kenning-test
    sed -i 's/^disabled *=.*/disabled = false/' $T/etc/config.ini
    out=$(/tmp/pamharness root $T/pam)
    grep -q "Attempting" <<< "$out" && result "PAM: no face model, no scan" FAIL "$out" || result "PAM: no face model, no scan" PASS
    kill $daemon 2>/dev/null; wait $daemon 2>/dev/null
    out=$(/tmp/pamharness root $T/pam)
    grep -q "Authentication failure" <<< "$out" && result "PAM: service down, password fallback" PASS || result "PAM: service down, password fallback" FAIL "$out"
else
    result "PAM harness" SKIP "pam_start_confdir unavailable: $(head -1 /tmp/harness.log)"
    kill $daemon 2>/dev/null
fi

# --- pam-tidy against this distribution's PAM layout --------------------------------
mkdir -p /tmp/pamcopy /tmp/kdir/models
cp -a /etc/pam.d/. /tmp/pamcopy/ 2>/dev/null
touch /tmp/kdir/models/someone.dat
vendor=/usr/lib/pam.d
[[ -d $vendor ]] || vendor=/etc/pam.d
out=$(PAM_DIR=/tmp/pamcopy VENDOR_PAM_DIR=$vendor KENNING_DIR=/tmp/kdir bash /src/packaging/pam-tidy migrate 2>&1)
enabled=$(grep -l "pam_kenning.so" /tmp/pamcopy/* 2>/dev/null | xargs -r -n1 basename | tr '\n' ' ')
if [[ -n $enabled ]]; then
    result "pam-tidy migrate" PASS "face enabled in: $enabled"
elif ! ls /etc/pam.d/{plasmalogin,kde,sudo} $vendor/{plasmalogin,kde,sudo} > /dev/null 2>&1; then
    result "pam-tidy migrate" SKIP "no plasmalogin, kde or sudo PAM file in this container"
else
    result "pam-tidy migrate" FAIL "$out"
fi
PAM_DIR=/tmp/pamcopy bash /src/packaging/pam-tidy remove > /dev/null 2>&1
grep -lq "pam_kenning.so" /tmp/pamcopy/* 2>/dev/null && result "pam-tidy remove" FAIL || result "pam-tidy remove" PASS
echo "   PAM files here: $(ls /etc/pam.d | tr '\n' ' ')"
