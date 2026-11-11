# SPDX-FileCopyrightText: 2026 Rishon Jonathan R
# SPDX-License-Identifier: CC0-1.0

Name:           kenning
Version:        0.8.1
Release:        1%{?dist}
Summary:        Face unlock for KDE Plasma
License:        GPL-2.0-or-later AND LGPL-2.0-or-later AND MIT AND Apache-2.0
URL:            https://github.com/RishonDev/kenning
Source0:        %{name}-%{version}.tar.gz
Source1:        https://github.com/opencv/opencv_zoo/raw/main/models/face_detection_yunet/face_detection_yunet_2023mar.onnx
Source2:        https://github.com/opencv/opencv_zoo/raw/main/models/face_recognition_sface/face_recognition_sface_2021dec.onnx

BuildRequires:  cmake
BuildRequires:  extra-cmake-modules
BuildRequires:  gcc-c++
BuildRequires:  gettext
BuildRequires:  kf6-kauth-devel
BuildRequires:  libarchive-devel
BuildRequires:  kf6-kcmutils-devel
BuildRequires:  kf6-kconfig-devel
BuildRequires:  kf6-kcoreaddons-devel
BuildRequires:  kf6-kdbusaddons-devel
BuildRequires:  kf6-ki18n-devel
BuildRequires:  kf6-kirigami-devel
BuildRequires:  kf6-kwindowsystem-devel
BuildRequires:  opencv-devel
BuildRequires:  pam-devel
BuildRequires:  polkit-qt6-1-devel
BuildRequires:  qt6-qtbase-devel
BuildRequires:  qt6-qtdeclarative-devel
BuildRequires:  systemd-devel
BuildRequires:  systemd-rpm-macros

Requires:       bsdtar
Requires:       kf6-kcmutils
Requires:       kf6-kirigami
Requires:       libnotify
Requires:       python3
Requires:       qt6-qtdeclarative
Requires:       systemd
Recommends:     plasma-desktop
# Fingerprints on the lock screen and in administrator prompts
Recommends:     fprintd
# Reads the setup steps aloud
Recommends:     qt6-qtspeech
Recommends:     speech-dispatcher

%description
Kenning signs you in with your face: on the lock screen, for sudo and, with its
version of Plasma Login Manager, on the login screen. It works best with an
infrared camera; an ordinary webcam can be used with a warning.

It includes a System Settings page to set it up, an animated face scan on the
lock screen (Plasma 6.8 or newer) and installable animation themes. Recognition
runs in a small service that owns the camera and the face models.

%prep
%autosetup
cp %{SOURCE1} %{SOURCE2} .

%build
%cmake -DKENNING_NETWORKS_DIR=%{_builddir}/%{name}-%{version}
%cmake_build

%install
%cmake_install

%post
# The service user owns the face models, which nobody else can read.
systemd-sysusers kenning.conf >/dev/null 2>&1 || :
mkdir -p %{_sysconfdir}/kenning/models
chown -R kenning:kenning %{_sysconfdir}/kenning/models || :
chmod 700 %{_sysconfdir}/kenning/models
find %{_sysconfdir}/kenning/models -type f -exec chmod 600 {} +
# Photos of failed attempts are no longer kept; delete any saved before.
rm -rf /var/lib/kenning/snapshots
rmdir /var/lib/kenning 2>/dev/null || :
/usr/lib/kenning/pam-tidy tidy || :
if [ -d /run/systemd/system ]; then
    systemctl daemon-reload || :
    systemctl start kenning.socket || :
    systemctl try-restart kenning.service || :
fi

%preun
if [ $1 -eq 0 ]; then
    # Nothing may point at pam_kenning.so or the add-on once they are gone.
    /usr/lib/kenning/pam-tidy remove || :
    python3 -I /usr/lib/kenning/kenning-lockscreen.py --restore || :
    if [ -d /run/systemd/system ]; then
        systemctl stop kenning.socket kenning.service || :
    fi
fi

# Re-apply the lock screen add-on whenever Plasma's lock screen files change,
# including when this package is installed after Plasma.
%transfiletriggerin -- /usr/share/plasma/shells/org.kde.plasma.desktop/contents/lockscreen
python3 -I /usr/lib/kenning/kenning-lockscreen.py || :
/usr/lib/kenning/pam-tidy tidy || :

%transfiletriggerin -- /usr/lib/kenning
python3 -I /usr/lib/kenning/kenning-lockscreen.py || :

%files
%license LICENSES/* engine/native/models/LICENSE.yunet engine/native/models/LICENSE.sface
%doc README.md THEMES.md
%{_bindir}/kenning
%{_libdir}/security/pam_kenning.so
%{_unitdir}/kenning.service
%{_unitdir}/kenning.socket
%{_unitdir}/sockets.target.wants/kenning.socket
%{_sysusersdir}/kenning.conf
%dir %{_sysconfdir}/kenning
%config(noreplace) %{_sysconfdir}/kenning/config.ini
%config(noreplace) %{_sysconfdir}/xdg/autostart/org.kde.kenning.setupprompt.desktop
%{_datadir}/kenning/
/usr/lib/kenning/
%{_qt6_plugindir}/plasma/kcms/systemsettings/kcm_kenning.so
%{_libexecdir}/kf6/kauth/kcmkenning_authhelper
%{_datadir}/dbus-1/system-services/org.kde.kcontrol.kcmkenning.service
%{_datadir}/dbus-1/system.d/org.kde.kcontrol.kcmkenning.conf
%{_datadir}/polkit-1/actions/org.kde.kcontrol.kcmkenning.policy
%{_datadir}/polkit-1/rules.d/50-kenning.rules
%{_userunitdir}/plasma-polkit-agent.service.d/kenning.conf
%{_datadir}/applications/kcm_kenning.desktop

%changelog
* Tue Nov 03 2026 Rishon Jonathan R <mithicalaviator85@gmail.com> - 0.8.1-1
- Login screen: after a restart, open on the password box instead of a face scan that is refused anyway
- Lock screen: wait until the computer has woken up before scanning, instead of scanning on the way into suspend

* Sun Nov 01 2026 Rishon Jonathan R <mithicalaviator85@gmail.com> - 0.8.0-1
- Face unlock waits for a password sign-in after each restart, like a phone (on by default, can be turned off)
- README privacy section and docs/DESIGN.md, a design document for review

* Sun Nov 01 2026 Rishon Jonathan R <mithicalaviator85@gmail.com> - 0.7.1-1
- Removed the optional photos of failed attempts, which could photograph people who never agreed to it; photos already saved are deleted on upgrade

* Sun Nov 01 2026 Rishon Jonathan R <mithicalaviator85@gmail.com> - 0.7.0-1
- Works in dim rooms: dark infrared frames that show a face are used, about a third faster
- Separate too dark and too bright messages; the lock screen keeps the failure reason visible
- Photos of failed attempts (off by default) and Try Face Again on the login screen
- Gentler Set Up tilts, with tilting up optional, and reasons for skipped steps
- Security fixes: SSH requests outside a local session, theme archive limits, a lockout that survives restarts

* Sat Oct 31 2026 Rishon Jonathan R <mithicalaviator85@gmail.com> - 0.6.0-1
- Guided face setup with head turns, a countdown and spoken steps
- Set Up previews the ordinary webcam instead of the infrared picture
- Settings page laid out like Plasma's own
- "Try Face Again" button on the lock screen after face unlock fails
- Frames washed out by bright light are skipped like dark ones

* Sat Oct 31 2026 Rishon Jonathan R <mithicalaviator85@gmail.com> - 0.5.2-1
- Lock screen scan animation on Plasma 6.3 to 6.7

* Sat Oct 31 2026 Rishon Jonathan R <mithicalaviator85@gmail.com> - 0.5.1-1
- Lock face unlock for a few minutes after repeated unrecognized faces, like pam_faillock

* Sat Oct 31 2026 Rishon Jonathan R <mithicalaviator85@gmail.com> - 0.5.0-1
- Use Plasma 6.8's Face sign-in method (PAM service kde-face) on the lock screen
- Choose face or fingerprint first on the lock screen when a fingerprint is enrolled

* Fri Oct 30 2026 Rishon Jonathan R <mithicalaviator85@gmail.com> - 0.4.0-1
- First Fedora package
