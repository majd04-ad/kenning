# SPDX-FileCopyrightText: 2026 Majd Abu Diab
# SPDX-License-Identifier: CC0-1.0
#
# Kenning's polkit dialog, from polkit-kde-agent-1: Plasma's, with Fingerprint and Face beside the password.

set(KDE_COMPILERSETTINGS_LEVEL "5.82") # as polkit-kde-agent-1, whose code this keeps
find_package(ECM 6.10.0 REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)

find_package(Qt6 6.8.0 CONFIG REQUIRED COMPONENTS Core DBus Widgets Quick Qml)
find_package(KF6 6.10.0 REQUIRED COMPONENTS Config CoreAddons DBusAddons I18n WindowSystem)
find_package(PolkitQt6-1 0.103.0 REQUIRED)

include(CheckIncludeFileCXX)
include(CheckCXXSymbolExists)
check_include_file_cxx("sys/prctl.h" HAVE_SYS_PRCTL_H)
check_cxx_symbol_exists(PR_SET_DUMPABLE "sys/prctl.h" HAVE_PR_SET_DUMPABLE)
check_include_file_cxx("sys/procctl.h" HAVE_SYS_PROCCTL_H)
check_cxx_symbol_exists(PROC_TRACE_CTL "sys/procctl.h" HAVE_PROC_TRACE_CTL)
# Started instead of this dialog when it is switched off
set(STOCK_POLKIT_AGENT ${KDE_INSTALL_FULL_LIBEXECDIR}/polkit-kde-authentication-agent-1)
set(KENNING_SYSTEM_CONFIG ${CMAKE_INSTALL_FULL_SYSCONFDIR}/xdg/kenningrc)
configure_file(agentconfig.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/agentconfig.h)

add_definitions(-DQT_NO_KEYWORDS)
qt_add_dbus_adaptor(agent_SRCS org.kde.Polkit1AuthAgent.xml policykitlistener.h PolicyKitListener)

add_executable(kenning-polkit-agent ${agent_SRCS}
    IdentitiesModel.cpp IdentitiesModel.h
    kenningservice.cpp kenningservice.h
    main.cpp
    policykitlistener.cpp policykitlistener.h
    QuickAuthDialog.cpp QuickAuthDialog.h
    QuickAuthDialog.qrc
)
target_include_directories(kenning-polkit-agent PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_compile_definitions(kenning-polkit-agent PRIVATE KENNING_VERSION="${PROJECT_VERSION}")
target_link_libraries(kenning-polkit-agent
    kenning-config
    KF6::ConfigCore
    KF6::CoreAddons
    KF6::DBusAddons
    KF6::I18n
    KF6::I18nQml
    KF6::WindowSystem
    Qt::DBus
    Qt::Qml
    Qt::Quick
    Qt::Widgets
    PolkitQt6-1::Agent
)
install(TARGETS kenning-polkit-agent DESTINATION ${KENNING_LIBEXECDIR})

# Runs in place of Plasma's agent from the next login (or systemctl --user restart plasma-polkit-agent)
configure_file(plasma-polkit-agent-kenning.conf.in plasma-polkit-agent-kenning.conf @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/plasma-polkit-agent-kenning.conf
        DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/systemd/user/plasma-polkit-agent.service.d RENAME kenning.conf)

if(BUILD_TESTING)
    add_test(NAME polkit_agent_smoketest COMMAND kenning-polkit-agent --smoke-test)
    set_tests_properties(polkit_agent_smoketest PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
