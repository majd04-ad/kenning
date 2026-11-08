# SPDX-FileCopyrightText: 2026 Rishon Jonathan R
# SPDX-License-Identifier: CC0-1.0
#
# One build for every package format; distribution-specific pieces live under packaging/.

cmake_minimum_required(VERSION 3.20)
project(kenning VERSION 0.8.1 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(GNUInstallDirs)

option(KENNING_BUILD_SETTINGS "Build the System Settings page (needs KDE Frameworks 6)" ON)
set(KENNING_NETWORKS_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE PATH
    "Directory with face_detection_yunet_2023mar.onnx and face_recognition_sface_2021dec.onnx")

include(CTest)

# Helper scripts; not libexec, so the paths in the packaging scripts are the same everywhere.
set(KENNING_LIBEXECDIR ${CMAKE_INSTALL_PREFIX}/lib/kenning)

add_subdirectory(engine/native)
if(KENNING_BUILD_SETTINGS)
    add_subdirectory(kcm)
endif()
if(BUILD_TESTING)
    add_subdirectory(autotests)
endif()

find_package(ECM NO_MODULE QUIET)
if(ECM_FOUND)
    list(APPEND CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
    include(KDEClangFormat)
    include(KDEGitCommitHooks)
    file(GLOB_RECURSE formatted_sources CONFIGURE_DEPENDS
        autotests/*.cpp autotests/*.h engine/native/src/*.cpp engine/native/src/*.h kcm/src/*.cpp kcm/src/*.h)
    kde_clang_format(${formatted_sources})
    kde_configure_git_pre_commit_hook(CHECKS CLANG_FORMAT)
endif()

# The recognition networks are too large for the repository, so packages download them.
foreach(network face_detection_yunet_2023mar.onnx face_recognition_sface_2021dec.onnx)
    if(NOT EXISTS "${KENNING_NETWORKS_DIR}/${network}")
        message(FATAL_ERROR "${network} not found in KENNING_NETWORKS_DIR (${KENNING_NETWORKS_DIR}). "
                            "Download it from https://github.com/opencv/opencv_zoo (models/face_detection_yunet, models/face_recognition_sface).")
    endif()
    # Package sources are often symlinks (makepkg's are); install the file itself.
    file(REAL_PATH "${KENNING_NETWORKS_DIR}/${network}" network_file)
    install(FILES "${network_file}" DESTINATION ${CMAKE_INSTALL_DATADIR}/kenning/models RENAME ${network})
endforeach()

install(PROGRAMS packaging/pam-tidy packaging/setup-prompt lockscreen/kenning-lockscreen.py DESTINATION ${KENNING_LIBEXECDIR})
install(FILES kcm/src/ui/FaceScanIndicator.qml DESTINATION ${KENNING_LIBEXECDIR})
install(FILES packaging/org.kde.kenning.setupprompt.desktop DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/xdg/autostart)

set(wants "lib/systemd/system/sockets.target.wants")
install(CODE "
    file(MAKE_DIRECTORY \"\$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/${wants}\")
    execute_process(COMMAND \"${CMAKE_COMMAND}\" -E create_symlink ../kenning.socket
                            \"\$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/${wants}/kenning.socket\")
")
