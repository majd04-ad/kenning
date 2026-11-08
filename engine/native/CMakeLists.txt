# SPDX-FileCopyrightText: 2026 Rishon Jonathan R
# SPDX-License-Identifier: CC0-1.0

find_package(OpenCV REQUIRED COMPONENTS core imgproc objdetect videoio dnn)
find_package(Qt6 REQUIRED COMPONENTS Core)
find_library(PAM_LIBRARY pam REQUIRED)
# logind tells the PAM module whether a request comes from a remote session
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBSYSTEMD REQUIRED IMPORTED_TARGET libsystemd)

set(KENNING_BINARY ${CMAKE_INSTALL_FULL_BINDIR}/kenning)
# Where PAM looks for modules: lib/security, lib64/security or the multiarch lib/<triplet>/security
set(KENNING_PAM_MODULE ${CMAKE_INSTALL_FULL_LIBDIR}/security/pam_kenning.so)
set(KENNING_SYSCONFDIR ${CMAKE_INSTALL_FULL_SYSCONFDIR}/kenning CACHE PATH "Config and face models")
set(KENNING_DATADIR ${CMAKE_INSTALL_FULL_DATADIR}/kenning CACHE PATH "Recognition networks")
set(KENNING_SOCKET /run/kenning.sock CACHE STRING "Socket of the face unlock service")
set(KENNING_SERVICE_USER kenning CACHE STRING "System user the service runs as and that owns the face models")

add_library(kenning-config INTERFACE)
target_include_directories(kenning-config INTERFACE src)
target_compile_definitions(kenning-config INTERFACE
    KENNING_SYSCONFDIR="${KENNING_SYSCONFDIR}"
    KENNING_DATADIR="${KENNING_DATADIR}"
    KENNING_SOCKET="${KENNING_SOCKET}"
    KENNING_SERVICE_USER="${KENNING_SERVICE_USER}"
    KENNING_BINARY="${KENNING_BINARY}"
    KENNING_PAM_MODULE="${KENNING_PAM_MODULE}"
    KENNING_LIBEXECDIR="${KENNING_LIBEXECDIR}"
)

add_library(kenning-engine STATIC
    src/config.h
    src/engine.cpp src/engine.h
    src/facemodels.cpp src/facemodels.h
    src/failurelimiter.cpp src/failurelimiter.h
    src/headpose.cpp src/headpose.h
    src/pipeline.cpp src/pipeline.h
    src/service.h
    src/signedin.h
    src/turnqueue.h
)
target_link_libraries(kenning-engine PUBLIC ${OpenCV_LIBS} Qt6::Core kenning-config)
target_include_directories(kenning-engine PUBLIC ${OpenCV_INCLUDE_DIRS})

add_executable(kenning src/main.cpp src/daemon.cpp src/daemon.h)
target_compile_definitions(kenning PRIVATE KENNING_VERSION="${PROJECT_VERSION}")
target_link_libraries(kenning PRIVATE kenning-engine PkgConfig::LIBSYSTEMD)

# The PAM module only asks the service; OpenCV stays out of the programs that authenticate.
add_library(pam_kenning MODULE src/pam_kenning.cpp)
set_target_properties(pam_kenning PROPERTIES PREFIX "")
target_link_libraries(pam_kenning PRIVATE kenning-config ${PAM_LIBRARY} PkgConfig::LIBSYSTEMD)

install(TARGETS kenning RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS pam_kenning LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/security)
configure_file(systemd/kenning.service.in kenning.service @ONLY)
configure_file(systemd/kenning-sysusers.conf.in kenning-sysusers.conf @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/kenning-sysusers.conf DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/sysusers.d RENAME kenning.conf)
configure_file(systemd/kenning.socket.in kenning.socket @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/kenning.service ${CMAKE_CURRENT_BINARY_DIR}/kenning.socket
        DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/systemd/system)
install(FILES config.ini DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/kenning)
install(FILES models/LICENSE.yunet models/LICENSE.sface DESTINATION ${CMAKE_INSTALL_DATADIR}/kenning/models)
