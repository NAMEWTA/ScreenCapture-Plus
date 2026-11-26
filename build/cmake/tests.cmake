# Additional shared-service contracts from the unified implementation. These
# run against the production targets even when the complete Rust product is
# not requested. Native desktop probes remain separate from offscreen tests.
if(NOT BUILD_TESTING)
    return()
endif()

# These shutdown modes exercise the same retained Portal implementations as
# the missing-service cases. Keep every mode registered after target moves.
if(TARGET portal_fault_tests)
    add_test(NAME portal_shutdown_initializing COMMAND "${DBUS_RUN_SESSION}" --
        $<TARGET_FILE:portal_fault_tests> --shutdown-initializing)
    add_test(NAME portal_shutdown_initializing_no_bus COMMAND portal_fault_tests
        --no-bus --shutdown-initializing)
    add_test(NAME portal_shutdown_closed_admission COMMAND portal_fault_tests --shutdown-with-late-request)
    add_test(NAME portal_remote_without_session COMMAND portal_fault_tests --remote-without-session)
    set_tests_properties(portal_shutdown_initializing portal_shutdown_initializing_no_bus
        portal_shutdown_closed_admission portal_remote_without_session PROPERTIES
        TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
if(TARGET portal_shortcuts_fault_tests)
    add_test(NAME portal_shortcuts_shutdown COMMAND "${DBUS_RUN_SESSION}" --
        $<TARGET_FILE:portal_shortcuts_fault_tests> --shutdown)
    add_test(NAME portal_shortcuts_shutdown_no_bus COMMAND portal_shortcuts_fault_tests --shutdown --no-bus)
    set_tests_properties(portal_shortcuts_shutdown portal_shortcuts_shutdown_no_bus PROPERTIES
        TIMEOUT 25 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
if(TARGET huhucapture_desktop_settings)
    add_executable(window_activation_tests tests/qt/window_activation_tests.cpp)
    target_link_libraries(window_activation_tests PRIVATE
        huhucapture_capture_session huhucapture_desktop_settings Qt6::Widgets)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_libraries(window_activation_tests PRIVATE PkgConfig::XCB_CAPTURE)
    endif()
    set_target_properties(window_activation_tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME window_activation_unavailable COMMAND window_activation_tests)
    set_tests_properties(window_activation_unavailable PROPERTIES TIMEOUT 30
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    if(WIN32)
        add_test(NAME window_activation_native COMMAND window_activation_tests --native)
        set_tests_properties(window_activation_native PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE
            ENVIRONMENT "QT_QPA_PLATFORM=windows")
    elseif(HUHUCAPTURE_X11_TESTS)
        add_test(NAME window_activation_native COMMAND "${XVFB_RUN}" -a
            "${CMAKE_COMMAND}" -E env "QT_QPA_PLATFORM=xcb" $<TARGET_FILE:window_activation_tests> --native)
        set_tests_properties(window_activation_native PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)
    endif()
endif()

huhucapture_test(qt_standard_translations tests/qt/qt_standard_translation_tests.cpp)
target_link_libraries(qt_standard_translations PRIVATE huhucapture_languages Qt6::Widgets)
set_tests_properties(qt_standard_translations PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=${HUHUCAPTURE_TEST_QPA}")
huhucapture_test(qt_update_presentation tests/qt/update_presentation_tests.cpp
    "${PROJECT_BINARY_DIR}/update-render/$<CONFIG>")
target_link_libraries(qt_update_presentation PRIVATE huhucapture_updates huhucapture_desktop huhucapture_languages)
set_tests_properties(qt_update_presentation PROPERTIES TIMEOUT 90
    ENVIRONMENT "QT_QPA_PLATFORM=${HUHUCAPTURE_TEST_QPA}")
add_executable(qt_complete_update_probe tests/qt/complete_update_probe.cpp)
target_link_libraries(qt_complete_update_probe PRIVATE huhucapture_updates huhucapture_desktop huhucapture_languages)
set_target_properties(qt_complete_update_probe PROPERTIES CXX_EXTENSIONS OFF)
huhucapture_test(qt_recording_admission tests/qt/recording_admission_tests.cpp)
target_link_libraries(qt_recording_admission PRIVATE huhucapture_recording_session)
huhucapture_test(qt_recording_window tests/qt/recording_window_tests.cpp
    "${PROJECT_BINARY_DIR}/recording-render/$<CONFIG>")
target_link_libraries(qt_recording_window PRIVATE huhucapture_recording_ui huhucapture_languages)
set_tests_properties(qt_recording_window PROPERTIES TIMEOUT 90
    ENVIRONMENT "QT_QPA_PLATFORM=${HUHUCAPTURE_TEST_QPA}")
huhucapture_test(qt_capture_window tests/qt/capture_window_tests.cpp
    "${PROJECT_BINARY_DIR}/capture-window/$<CONFIG>")
target_link_libraries(qt_capture_window PRIVATE huhucapture_capture_session)
set_tests_properties(qt_capture_window PROPERTIES TIMEOUT 90
    ENVIRONMENT "QT_QPA_PLATFORM=${HUHUCAPTURE_TEST_QPA}")
huhucapture_test(qt_scroll_progress tests/qt/scroll_progress_tests.cpp)
target_link_libraries(qt_scroll_progress PRIVATE Qt6::Core Qt6::Gui Threads::Threads)
set_tests_properties(qt_scroll_progress PROPERTIES TIMEOUT 20)

if(TARGET huhucapture_desktop_services)
    huhucapture_test(desktop_capture_action_tests tests/qt/desktop_capture_action_tests.cpp)
    target_link_libraries(desktop_capture_action_tests PRIVATE huhucapture_desktop_services)
    set_tests_properties(desktop_capture_action_tests PROPERTIES TIMEOUT 30
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        huhucapture_test(desktop_capture_action_probe tests/platform/desktop_capture_action_probe.cpp)
        target_link_libraries(desktop_capture_action_probe PRIVATE huhucapture_desktop_services
            huhucapture_startup huhucapture_launch_command Qt6::Concurrent PkgConfig::GIO)
        add_test(NAME desktop_capture_action_monitor COMMAND desktop_capture_action_probe --monitor-override)
        set_tests_properties(desktop_capture_action_monitor PROPERTIES TIMEOUT 30)
        find_program(DBUS_RUN_SESSION dbus-run-session REQUIRED)
        add_test(NAME desktop_capture_action_roundtrip COMMAND "${DBUS_RUN_SESSION}" --
            $<TARGET_FILE:desktop_capture_action_probe> --gio-roundtrip)
        set_tests_properties(desktop_capture_action_roundtrip PROPERTIES TIMEOUT 40)
    endif()
    huhucapture_test(qt_editor_preferences tests/qt/editor_preferences_tests.cpp)
    target_link_libraries(qt_editor_preferences PRIVATE huhucapture_media_workspace huhucapture_languages)
    set_tests_properties(qt_editor_preferences PROPERTIES TIMEOUT 180
        ENVIRONMENT "QT_QPA_PLATFORM=${HUHUCAPTURE_TEST_QPA}")
    huhucapture_test(system_clipboard_shutdown_tests tests/qt/system_clipboard_shutdown_tests.cpp)
    target_link_libraries(system_clipboard_shutdown_tests PRIVATE huhucapture_desktop_services
        Qt6::Widgets Qt6::Concurrent Threads::Threads)
    set_tests_properties(system_clipboard_shutdown_tests PROPERTIES TIMEOUT 60
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
if(TARGET huhucapture_desktop_settings)
    huhucapture_test(qt_shortcut_shutdown tests/qt/shortcut_shutdown_tests.cpp)
    target_link_libraries(qt_shortcut_shutdown PRIVATE huhucapture_desktop_settings)
    set_tests_properties(qt_shortcut_shutdown PROPERTIES TIMEOUT 60
        ENVIRONMENT "QT_QPA_PLATFORM=${HUHUCAPTURE_TEST_QPA}")
    if(NOT TARGET qt_settings_controller)
        add_executable(qt_settings_controller tests/qt/settings_controller_tests.cpp)
        set_target_properties(qt_settings_controller PROPERTIES CXX_EXTENSIONS OFF)
        target_link_libraries(qt_settings_controller PRIVATE huhucapture_desktop_settings)
        if(WIN32)
            target_link_libraries(qt_settings_controller PRIVATE user32 advapi32 shell32)
            add_test(NAME qt_settings_controller COMMAND qt_settings_controller
                "${PROJECT_BINARY_DIR}/settings-controller-render/$<CONFIG>")
            set_tests_properties(qt_settings_controller PROPERTIES TIMEOUT 180
                ENVIRONMENT "QT_QPA_PLATFORM=windows")
        else()
            target_compile_definitions(qt_settings_controller PRIVATE QT_NO_KEYWORDS)
            target_link_libraries(qt_settings_controller PRIVATE PkgConfig::XCB_CAPTURE
                PkgConfig::XCB_SHORTCUT PkgConfig::GIO)
            add_test(NAME qt_settings_autostart COMMAND qt_settings_controller --startup-only)
            add_test(NAME qt_settings_controller_unavailable COMMAND qt_settings_controller
                "${PROJECT_BINARY_DIR}/settings-unavailable-render/$<CONFIG>" --unavailable)
            set_tests_properties(qt_settings_autostart qt_settings_controller_unavailable PROPERTIES
                TIMEOUT 90 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
            if(HUHUCAPTURE_X11_TESTS)
                add_test(NAME qt_settings_controller_x11 COMMAND "${XVFB_RUN}" -a
                    "${CMAKE_COMMAND}" -E env "QT_QPA_PLATFORM=xcb" $<TARGET_FILE:qt_settings_controller>
                    "${PROJECT_BINARY_DIR}/settings-controller-render/$<CONFIG>")
                set_tests_properties(qt_settings_controller_x11 PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
            endif()
        endif()
    endif()
endif()
if(TARGET huhucapture_gif_encoder)
    huhucapture_test(qt_gif_encoder tests/qt/gif_encoder_tests.cpp)
    target_link_libraries(qt_gif_encoder PRIVATE huhucapture_gif_encoder huhucapture_gif_inspection)
    set_tests_properties(qt_gif_encoder PROPERTIES TIMEOUT 120
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
if(TARGET huhucapture_mp4_encoder AND TARGET huhucapture_video_inspection)
    huhucapture_test(qt_mp4_encoder tests/qt/mp4_encoder_tests.cpp
        "${PROJECT_BINARY_DIR}/mp4-encoder/$<CONFIG>")
    target_link_libraries(qt_mp4_encoder PRIVATE huhucapture_mp4_encoder
        huhucapture_video_inspection PkgConfig::FFMPEG Qt6::Core)
    set_tests_properties(qt_mp4_encoder PROPERTIES TIMEOUT 180)
endif()
if(TARGET huhucapture_pipewire_audio)
    add_executable(pipewire_audio_signal tests/platform/pipewire_audio_signal.cpp)
    target_link_libraries(pipewire_audio_signal PRIVATE huhucapture_core PkgConfig::PIPEWIRE Threads::Threads)
    add_executable(pipewire_audio_graph_probe tests/platform/pipewire_audio_graph_probe.cpp)
    target_link_libraries(pipewire_audio_graph_probe PRIVATE huhucapture_pipewire_audio PkgConfig::PIPEWIRE Threads::Threads)
    set_target_properties(pipewire_audio_signal pipewire_audio_graph_probe PROPERTIES CXX_EXTENSIONS OFF)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TARGET huhucapture_recording AND TARGET huhucapture_desktop_capture)
    huhucapture_test(live_recording_tests tests/qt/live_recording_tests.cpp
        "${PROJECT_BINARY_DIR}/live-recording/$<CONFIG>")
    target_link_libraries(live_recording_tests PRIVATE huhucapture_recording huhucapture_live_capture
        huhucapture_gif_inspection huhucapture_video_inspection Threads::Threads)
    set_tests_properties(live_recording_tests PROPERTIES TIMEOUT 180)
    huhucapture_test(x11_recording_failure_tests tests/qt/x11_recording_failure_tests.cpp)
    target_link_libraries(x11_recording_failure_tests PRIVATE huhucapture_recording)
    set_tests_properties(x11_recording_failure_tests PROPERTIES TIMEOUT 45)
    add_executable(x11_recording_probe tests/qt/x11_recording_probe.cpp)
    target_link_libraries(x11_recording_probe PRIVATE huhucapture_recording huhucapture_gif_inspection
        huhucapture_video_inspection Qt6::Widgets Qt6::Concurrent PkgConfig::XCB_CAPTURE)
    set_target_properties(x11_recording_probe PROPERTIES CXX_EXTENSIONS OFF)
    if(HUHUCAPTURE_X11_TESTS)
        add_test(NAME x11_recording_probe COMMAND "${XVFB_RUN}" -a
            "${CMAKE_COMMAND}" -E env "QT_QPA_PLATFORM=xcb" $<TARGET_FILE:x11_recording_probe>
            "${PROJECT_BINARY_DIR}/x11-recording/$<CONFIG>")
        set_tests_properties(x11_recording_probe PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
    endif()
endif()
if(TARGET huhucapture_application)
    add_test(NAME package_metadata COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/package_metadata_tests.py")
    set_tests_properties(package_metadata PROPERTIES TIMEOUT 90)
    add_executable(qt_application_lifecycle tests/qt/application_lifecycle_tests.cpp)
    target_link_libraries(qt_application_lifecycle PRIVATE huhucapture_application Qt6::Concurrent)
    set_target_properties(qt_application_lifecycle PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME qt_application_lifecycle COMMAND qt_application_lifecycle
        "${PROJECT_BINARY_DIR}/application-lifecycle/$<CONFIG>")
    set_tests_properties(qt_application_lifecycle PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND HUHUCAPTURE_X11_TESTS)
        add_test(NAME qt_application_clipboard_owner COMMAND "${XVFB_RUN}" -a
            "${CMAKE_COMMAND}" -E env "QT_QPA_PLATFORM=xcb" $<TARGET_FILE:qt_application_lifecycle>
            "${PROJECT_BINARY_DIR}/application-clipboard-owner/$<CONFIG>" --clipboard-owner)
        set_tests_properties(qt_application_clipboard_owner PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
    endif()
    add_executable(capture_workspace_tests tests/qt/capture_workspace_tests.cpp)
    target_link_libraries(capture_workspace_tests PRIVATE huhucapture_application)
    set_target_properties(capture_workspace_tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME capture_workspace_unavailable COMMAND capture_workspace_tests --unavailable)
    set_tests_properties(capture_workspace_unavailable PROPERTIES TIMEOUT 90
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    if(WIN32)
        add_test(NAME capture_workspace_tests COMMAND capture_workspace_tests)
        set_tests_properties(capture_workspace_tests PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE
            ENVIRONMENT "QT_QPA_PLATFORM=windows")
    elseif(HUHUCAPTURE_X11_TESTS)
        add_test(NAME capture_workspace_tests COMMAND "${XVFB_RUN}" -a
            "${CMAKE_COMMAND}" -E env "QT_QPA_PLATFORM=xcb" $<TARGET_FILE:capture_workspace_tests>)
        set_tests_properties(capture_workspace_tests PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
    endif()
    add_executable(scroll_capture_tests tests/qt/scroll_capture_tests.cpp)
    target_link_libraries(scroll_capture_tests PRIVATE huhucapture_application)
    set_target_properties(scroll_capture_tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME scroll_capture_unavailable COMMAND scroll_capture_tests --unavailable)
    set_tests_properties(scroll_capture_unavailable PROPERTIES TIMEOUT 90
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    if(WIN32)
        target_link_libraries(scroll_capture_tests PRIVATE dwmapi)
        add_test(NAME scroll_capture_tests COMMAND scroll_capture_tests
            "${PROJECT_BINARY_DIR}/scroll-capture/$<CONFIG>")
        set_tests_properties(scroll_capture_tests PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE
            ENVIRONMENT "QT_QPA_PLATFORM=windows")
    elseif(HUHUCAPTURE_X11_TESTS)
        add_test(NAME scroll_capture_tests COMMAND "${XVFB_RUN}" -a
            "${CMAKE_COMMAND}" -E env "QT_QPA_PLATFORM=xcb" $<TARGET_FILE:scroll_capture_tests>
            "${PROJECT_BINARY_DIR}/scroll-capture/$<CONFIG>")
        set_tests_properties(scroll_capture_tests PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
    endif()
endif()
if(TARGET huhucapture_desktop_capture)
    add_executable(screen_capture_tests tests/qt/screen_capture_tests.cpp)
    target_link_libraries(screen_capture_tests PRIVATE huhucapture_desktop_capture Qt6::Widgets)
    set_target_properties(screen_capture_tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME screen_capture_unavailable COMMAND screen_capture_tests --no-backend)
    set_tests_properties(screen_capture_unavailable PROPERTIES TIMEOUT 60
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    if(WIN32)
        add_test(NAME screen_capture_tests COMMAND screen_capture_tests --artifacts
            "${PROJECT_BINARY_DIR}/screen-capture/$<CONFIG>")
        set_tests_properties(screen_capture_tests PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE
            ENVIRONMENT "QT_QPA_PLATFORM=windows")
    elseif(HUHUCAPTURE_X11_TESTS)
        add_test(NAME screen_capture_tests COMMAND "${XVFB_RUN}" -a
            "${CMAKE_COMMAND}" -E env "QT_QPA_PLATFORM=xcb" $<TARGET_FILE:screen_capture_tests>
            --artifacts "${PROJECT_BINARY_DIR}/screen-capture/$<CONFIG>")
        set_tests_properties(screen_capture_tests PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
    endif()
endif()
if(TARGET huhucapture_live_capture)
    huhucapture_test(live_capture_source_tests tests/qt/live_capture_source_tests.cpp)
    target_link_libraries(live_capture_source_tests PRIVATE huhucapture_live_capture)
    set_tests_properties(live_capture_source_tests PROPERTIES TIMEOUT 45
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(live_capture_source_probe tests/qt/live_capture_source_probe.cpp)
        target_link_libraries(live_capture_source_probe PRIVATE huhucapture_live_capture Qt6::Widgets)
        add_executable(live_capture_refresh_probe tests/qt/live_capture_refresh_probe.cpp)
        target_link_libraries(live_capture_refresh_probe PRIVATE huhucapture_live_capture Qt6::Widgets)
        set_target_properties(live_capture_source_probe live_capture_refresh_probe PROPERTIES CXX_EXTENSIONS OFF)
    endif()
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TARGET huhucapture_desktop_capture)
    add_executable(x11_controls_probe tests/platform/x11_controls_probe.cpp)
    target_link_libraries(x11_controls_probe PRIVATE huhucapture_desktop_capture
        Qt6::Widgets Qt6::Concurrent PkgConfig::XCB_CAPTURE)
    set_target_properties(x11_controls_probe PROPERTIES CXX_EXTENSIONS OFF)
    if(HUHUCAPTURE_X11_TESTS)
        add_test(NAME x11_controls_native COMMAND "${XVFB_RUN}" -a
            "${CMAKE_COMMAND}" -E env "QT_QPA_PLATFORM=xcb" $<TARGET_FILE:x11_controls_probe>)
        set_tests_properties(x11_controls_native PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
    endif()
endif()
