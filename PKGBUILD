# Kenning, face unlock for Plasma, in one build: `makepkg -si` installs both packages.
#   kenning                       face recognition engine and service (engine/native, OpenCV),
#                                 System Settings page, lock screen add-on, themes
#   plasma-login-manager-kenning  Plasma Login Manager that scans as soon as it appears

pkgbase=kenning
pkgname=(kenning plasma-login-manager-kenning)
pkgver=0.8.1
_plmver=6.8.2
pkgrel=1
arch=(x86_64 aarch64)
url='https://github.com/RishonDev/kenning'
license=(GPL-2.0-or-later MIT Apache-2.0)
makedepends=(cmake extra-cmake-modules meson ninja qt6-tools
             # plasma-login-manager
             kauth kcmutils kconfig kcoreaddons kdbusaddons ki18n kio kirigami kpackage kservice
             layer-shell-qt libkscreen libplasma libxau pam plasma-workspace qt6-5compat qt6-base qt6-declarative systemd-libs
             # engine, polkit dialog
             opencv libarchive kwindowsystem polkit-qt6)
source=(https://download.kde.org/stable/plasma/$_plmver/plasma-login-manager-$_plmver.tar.xz{,.sig}
        https://github.com/opencv/opencv_zoo/raw/main/models/face_detection_yunet/face_detection_yunet_2023mar.onnx
        https://github.com/opencv/opencv_zoo/raw/main/models/face_recognition_sface/face_recognition_sface_2021dec.onnx)
sha256sums=('4a6e2b65b5d462fcf43b2a5be3c3e6244f479451b6dcfa00dcbf33fbc5cd093a'
            'SKIP'
            '8f2383e4dd3cfbb4553ea8718107fc0423210dc964f9f4280604804ed2552fa4'
            '0ba9fbfa01b5270c96627c4ef784da859931e02f04419c829e83484087c34e79')
# Their public keys are in keys/pgp: `gpg --import keys/pgp/*.asc` before the first build
validpgpkeys=('E0A3EB202F8E57528E13E72FD7574483BB57B18D'  # Jonathan Esk-Riddell <jr@jriddell.org>
              '0AAC775BB6437A8D9AF7A3ACFE0784117FBCE11D'  # Bhushan Shah <bshah@kde.org>
              'D07BD8662C56CB291B316EB2F5675605C74E02CF'  # David Edmundson <davidedmundson@kde.org>
              '1FA881591C26B276D7A5518EEAAF29B42A678C20') # Marco Martin <notmart@gmail.com>

prepare() {
  "$startdir/login-manager/apply.sh" plasma-login-manager-$_plmver
}

build() {
  cmake -B kenning-build -S "$startdir" -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib \
    -DKDE_INSTALL_LIBEXECDIR=lib -DKENNING_NETWORKS_DIR="$srcdir" -DCMAKE_BUILD_TYPE=RelWithDebInfo
  cmake --build kenning-build

  cmake -B plm-build -S plasma-login-manager-$_plmver -DKDE_INSTALL_LIBEXECDIR=lib \
    -DDBUS_CONFIG_FILENAME=plasma_org.freedesktop.DisplayManager.conf
  cmake --build plm-build
}

package_kenning() {
  pkgdesc='Kenning: face unlock for KDE Plasma'
  depends=(kauth kcmutils kconfig kcoreaddons kdbusaddons ki18n kirigami kwindowsystem libplasma plasma-desktop polkit-qt6
           qt6-base qt6-declarative libarchive libnotify opencv pam python systemd-libs)
  optdepends=('plasma-login-manager-kenning: start face unlock as soon as the login screen appears'
              'fprintd: fingerprints as a choice of their own on the lock screen and in administrator prompts'
              'qt6-speech: read the setup steps aloud'
              'speech-dispatcher: read the setup steps aloud (with a voice such as espeak-ng)')
  backup=(etc/kenning/config.ini)
  # Earlier name; the install script moves its settings and face models across
  conflicts=(plasma-faceunlock)
  replaces=(plasma-faceunlock)
  install=packaging/arch/kenning.install

  DESTDIR="$pkgdir" cmake --install kenning-build
  # Re-applies the lock screen add-on after plasma-desktop updates
  install -Dm644 -t "$pkgdir/usr/share/libalpm/hooks" "$startdir"/packaging/arch/*.hook
}

package_plasma-login-manager-kenning() {
  pkgdesc='Plasma Login Manager that starts face unlock as soon as it appears'
  depends=(glibc kauth kcmutils kconfig kcoreaddons kdbusaddons ki18n kio kirigami kpackage kservice layer-shell-qt
           libkscreen libplasma libstdc++ libxau pam plasma-workspace qt6-5compat qt6-base qt6-declarative sh systemd-libs)
  optdepends=('kenning: face unlock settings and themes')
  provides=("plasma-login-manager=$_plmver")
  conflicts=(plasma-login-manager plasma-login-manager-faceunlock)
  replaces=(plasma-login-manager-faceunlock)
  groups=(plasma)

  DESTDIR="$pkgdir" cmake --install plm-build
  # Tells Kenning's settings page that this login manager supports face unlock
  install -Dm644 /dev/stdin "$pkgdir/usr/share/kenning/login-manager" <<< "plasma-login-manager-kenning $_plmver"
}
