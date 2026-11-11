# SPDX-FileCopyrightText: 2026 Rishon Jonathan R
# SPDX-License-Identifier: CC0-1.0

find_package(Qt6 REQUIRED COMPONENTS Test)

function(kenning_add_tests)
    cmake_parse_arguments(ARG "" "" "TESTS;LINK_LIBRARIES" ${ARGN})
    foreach(test ${ARG_TESTS})
        add_executable(${test} ${test}.cpp)
        set_target_properties(${test} PROPERTIES AUTOMOC ON)
        target_link_libraries(${test} PRIVATE Qt6::Test ${ARG_LINK_LIBRARIES})
        add_test(NAME ${test} COMMAND ${test})
    endforeach()
endfunction()

kenning_add_tests(
    TESTS cameratest configtest servicetest signedintest turnqueuetest
    LINK_LIBRARIES kenning-config Qt6::Core
)
kenning_add_tests(
    TESTS facemodelstest failurelimitertest headposetest lightingtest
    LINK_LIBRARIES kenning-engine
)

if(KENNING_BUILD_SETTINGS)
    kenning_add_tests(
        TESTS settingsschematest
        LINK_LIBRARIES kenning_settings_common
    )
endif()

# Needs kcmshell6 and System Settings' QML modules, which package builds don't install.
option(KENNING_KCM_SMOKE_TEST "Test that the settings page loads" OFF)
if(KENNING_BUILD_SETTINGS AND KENNING_KCM_SMOKE_TEST)
    add_test(NAME kcm_smoketest COMMAND kcmshell6 --smoke-test kcm_kenning)
    set_tests_properties(kcm_smoketest PROPERTIES
        ENVIRONMENT_MODIFICATION "QT_PLUGIN_PATH=path_list_prepend:${CMAKE_BINARY_DIR}/bin;QT_QPA_PLATFORM=set:offscreen"
    )
endif()
