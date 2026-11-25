# All validation executables consume the production targets above.
# BUILD_TESTING=OFF does not discover test daemons or test interpreters.
function(huhucapture_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE huhucapture_core)
    set_target_properties(${name} PROPERTIES CXX_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${name} PRIVATE /W4 /utf-8 /permissive-)
    else()
        target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME ${name} COMMAND ${name} ${ARGN})
    set_tests_properties(${name} PROPERTIES TIMEOUT 60)
endfunction()

if(BUILD_TESTING)
    huhucapture_test(qr_recognition_tests tests/qr_recognition_tests.cpp)
    target_include_directories(qr_recognition_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(qr_recognition_tests PRIVATE huhucapture_qr_core)
    huhucapture_test(media_frame_tests tests/media_frame_tests.cpp)
    target_link_libraries(media_frame_tests PRIVATE huhucapture_media_frames)
    huhucapture_test(frame_queue_tests tests/frame_queue_tests.cpp)
    target_link_libraries(frame_queue_tests PRIVATE huhucapture_media_frames)
    huhucapture_test(frame_normalizer_tests tests/frame_normalizer_tests.cpp)
    target_link_libraries(frame_normalizer_tests PRIVATE huhucapture_frame_normalizer)
    huhucapture_test(pixel_buffer tests/PixelBufferTests.cpp)
    huhucapture_test(annotations tests/annotation_tests.cpp)
    huhucapture_test(annotation_regions tests/annotation_region_tests.cpp)
    huhucapture_test(annotation_panel tests/annotation_panel_layout_tests.cpp)
    huhucapture_test(history tests/history_tests.cpp)
    huhucapture_test(scroll_stitch tests/scroll_stitch_tests.cpp)
    huhucapture_test(scroll_capture_model tests/scroll_capture_model_tests.cpp)
    target_link_libraries(scroll_capture_model PRIVATE huhucapture_scroll_model)
    huhucapture_test(toolbar_layout tests/toolbar_layout_tests.cpp)
    huhucapture_test(capture_selection tests/capture_selection_tests.cpp)
    huhucapture_test(media_file_names tests/media_file_name_tests.cpp)
    huhucapture_test(release_versions tests/release_version_tests.cpp)
    huhucapture_test(update_package_policy tests/update_package_policy_tests.cpp)
    huhucapture_test(shortcut_keys tests/shortcut_keys_tests.cpp)
    huhucapture_test(startup tests/startup_tests.cpp)
    huhucapture_test(annotation_model tests/annotation_model_tests.cpp)
    huhucapture_test(box_model tests/box_model_tests.cpp)
    huhucapture_test(stroke_model tests/stroke_model_tests.cpp)
    huhucapture_test(arrow_model tests/arrow_model_tests.cpp)
    huhucapture_test(number_model tests/number_model_tests.cpp)
    huhucapture_test(text_model tests/text_model_tests.cpp)
    huhucapture_test(editor_document tests/editor_document_tests.cpp)
    huhucapture_test(mosaic_pixels tests/mosaic_pixels_tests.cpp)
    huhucapture_test(recording_results tests/recording_result_tests.cpp)
    huhucapture_test(recording_geometry tests/recording_geometry_tests.cpp)
    if(TARGET huhucapture_file_committer)
        find_package(Threads REQUIRED)
        huhucapture_test(file_commit tests/platform/file_commit_tests.cpp)
        target_link_libraries(file_commit PRIVATE huhucapture_file_committer Threads::Threads)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            target_compile_definitions(file_commit PRIVATE HUHUCAPTURE_TEST_WRAP_FSYNC)
            target_link_options(file_commit PRIVATE -Wl,--wrap=fsync)
        endif()
    endif()
endif()

if(HUHUCAPTURE_QT_VALIDATION)
    huhucapture_test(qt_qr_service tests/qt/qr_service_tests.cpp)
    target_include_directories(qt_qr_service PRIVATE "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(qt_qr_service PRIVATE huhucapture_qr_service Threads::Threads)
    if(WIN32)
        # The offscreen Windows plugin supplies a placeholder font engine.
        # Use the native font database without constructing visible windows.
        set(HUHUCAPTURE_TEST_QPA windows)
    else()
        set(HUHUCAPTURE_TEST_QPA offscreen)
    endif()
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    add_executable(qt_single_instance_probe tests/qt/single_instance_probe.cpp)
    target_link_libraries(qt_single_instance_probe PRIVATE huhucapture_startup)
    set_target_properties(qt_single_instance_probe PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME qt_single_instance COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/scripts/test-single-instance.py" $<TARGET_FILE:qt_single_instance_probe>)
    set_tests_properties(qt_single_instance PROPERTIES TIMEOUT 90)
    huhucapture_test(qt_capture_session tests/qt/capture_session_tests.cpp)
    target_link_libraries(qt_capture_session PRIVATE huhucapture_capture)
    set_tests_properties(qt_capture_session PROPERTIES TIMEOUT 120
        ENVIRONMENT "QT_QPA_PLATFORM=${HUHUCAPTURE_TEST_QPA}")
    huhucapture_test(qt_image_contract tests/qt/image_contract_tests.cpp)
    target_link_libraries(qt_image_contract PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Svg)
    set_tests_properties(qt_image_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${HUHUCAPTURE_TEST_QPA}")
    huhucapture_test(qt_annotation_render tests/qt/annotation_render_tests.cpp
        "${PROJECT_BINARY_DIR}/annotation-render/$<CONFIG>")
    target_link_libraries(qt_annotation_render PRIVATE huhucapture_annotation_renderer)
    set_tests_properties(qt_annotation_render PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${HUHUCAPTURE_TEST_QPA}")
    huhucapture_test(qt_editor_components tests/qt/editor_component_tests.cpp
        "${PROJECT_BINARY_DIR}/editor-render/$<CONFIG>")
    target_link_libraries(qt_editor_components PRIVATE huhucapture_editor)
    set_tests_properties(qt_editor_components PROPERTIES TIMEOUT 120
        ENVIRONMENT "QT_QPA_PLATFORM=${HUHUCAPTURE_TEST_QPA}")
    huhucapture_test(qt_desktop_components tests/qt/desktop_component_tests.cpp
        "${PROJECT_BINARY_DIR}/desktop-render/$<CONFIG>")
    target_link_libraries(qt_desktop_components PRIVATE huhucapture_desktop)
    set_tests_properties(qt_desktop_components PROPERTIES TIMEOUT 120
        ENVIRONMENT "QT_QPA_PLATFORM=${HUHUCAPTURE_TEST_QPA}")
    if(WIN32)
        add_test(NAME qt_annotation_missing_font
            COMMAND qt_annotation_render --expect-font-unavailable)
        set_tests_properties(qt_annotation_missing_font PROPERTIES TIMEOUT 60
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_FONTDIR=${PROJECT_BINARY_DIR}/intentionally-empty-fonts")
    endif()
    huhucapture_test(qt_settings_store tests/qt/settings_store_tests.cpp)
    target_link_libraries(qt_settings_store PRIVATE huhucapture_settings)
    huhucapture_test(qt_language_catalog tests/qt/language_catalog_tests.cpp)
    target_link_libraries(qt_language_catalog PRIVATE huhucapture_languages)
    huhucapture_test(qt_standard_translations tests/qt/qt_standard_translation_tests.cpp)
    target_link_libraries(qt_standard_translations PRIVATE huhucapture_languages Qt6::Widgets)
    set_tests_properties(qt_standard_translations PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${HUHUCAPTURE_TEST_QPA}")
    huhucapture_test(qt_updates tests/qt/update_service_tests.cpp)
    target_link_libraries(qt_updates PRIVATE huhucapture_updates)
    set_tests_properties(qt_updates PROPERTIES TIMEOUT 180)
    huhucapture_test(qt_update_presentation tests/qt/update_presentation_tests.cpp
        "${PROJECT_BINARY_DIR}/update-render/$<CONFIG>")
    target_link_libraries(qt_update_presentation PRIVATE huhucapture_updates huhucapture_desktop huhucapture_languages)
    set_tests_properties(qt_update_presentation PROPERTIES TIMEOUT 90
        ENVIRONMENT "QT_QPA_PLATFORM=${HUHUCAPTURE_TEST_QPA}")
    huhucapture_test(qt_recording_window tests/qt/recording_window_tests.cpp
        "${PROJECT_BINARY_DIR}/recording-render/$<CONFIG>")
    target_link_libraries(qt_recording_window PRIVATE huhucapture_recording_ui huhucapture_languages)
    set_tests_properties(qt_recording_window PROPERTIES TIMEOUT 90
        ENVIRONMENT "QT_QPA_PLATFORM=${HUHUCAPTURE_TEST_QPA}")
    huhucapture_test(qt_recording_admission tests/qt/recording_admission_tests.cpp)
    target_link_libraries(qt_recording_admission PRIVATE huhucapture_recording_session huhucapture_media_frames)
    huhucapture_test(qt_media_archive tests/qt/media_archive_tests.cpp)
    target_link_libraries(qt_media_archive PRIVATE huhucapture_media_archive Threads::Threads)
    set_tests_properties(qt_media_archive PROPERTIES TIMEOUT 120)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_compile_definitions(qt_media_archive PRIVATE HUHUCAPTURE_TEST_WRAP_FSYNC)
        target_link_options(qt_media_archive PRIVATE -Wl,--wrap=fsync)
    endif()
    if(HUHUCAPTURE_DESKTOP_VALIDATION)
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
        huhucapture_test(qt_media_workspace tests/qt/media_workspace_tests.cpp)
        target_link_libraries(qt_media_workspace PRIVATE huhucapture_media_workspace huhucapture_languages)
        set_tests_properties(qt_media_workspace PROPERTIES TIMEOUT 180
            ENVIRONMENT "QT_QPA_PLATFORM=${HUHUCAPTURE_TEST_QPA}")
        huhucapture_test(qt_editor_preferences tests/qt/editor_preferences_tests.cpp)
        target_link_libraries(qt_editor_preferences PRIVATE huhucapture_media_workspace huhucapture_languages)
        set_tests_properties(qt_editor_preferences PROPERTIES TIMEOUT 180
            ENVIRONMENT "QT_QPA_PLATFORM=${HUHUCAPTURE_TEST_QPA}")
        huhucapture_test(system_clipboard_shutdown_tests tests/qt/system_clipboard_shutdown_tests.cpp)
        target_link_libraries(system_clipboard_shutdown_tests PRIVATE huhucapture_desktop_services
            Qt6::Widgets Qt6::Concurrent Threads::Threads)
        set_tests_properties(system_clipboard_shutdown_tests PROPERTIES TIMEOUT 60
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        huhucapture_test(desktop_files_tests tests/qt/desktop_files_tests.cpp)
        target_link_libraries(desktop_files_tests PRIVATE huhucapture_desktop_services)
        huhucapture_test(file_revealer_tests tests/qt/file_revealer_tests.cpp)
        target_link_libraries(file_revealer_tests PRIVATE huhucapture_desktop_services Qt6::Widgets)
        set_tests_properties(file_revealer_tests PROPERTIES TIMEOUT 60
            ENVIRONMENT "QT_QPA_PLATFORM=${HUHUCAPTURE_TEST_QPA}")
        add_executable(desktop_clipboard_tests tests/qt/desktop_clipboard_tests.cpp)
        target_link_libraries(desktop_clipboard_tests PRIVATE huhucapture_desktop_services Qt6::Widgets)
        set_target_properties(desktop_clipboard_tests PROPERTIES CXX_EXTENSIONS OFF)
        if(WIN32)
            add_test(NAME desktop_clipboard_windows COMMAND desktop_clipboard_tests)
            set_tests_properties(desktop_clipboard_windows PROPERTIES TIMEOUT 120
                ENVIRONMENT "QT_QPA_PLATFORM=windows")
            add_test(NAME file_revealer_windows_desktop COMMAND file_revealer_tests --real-desktop)
            set_tests_properties(file_revealer_windows_desktop PROPERTIES TIMEOUT 120
                ENVIRONMENT "QT_QPA_PLATFORM=windows")
        else()
            find_program(XVFB_RUN xvfb-run)
            if(NOT XVFB_RUN)
                # Keep the real X11 test registered. A host without Xvfb can
                # build the production services, but this CTest still fails to
                # start and cannot be reported as passed or silently skipped.
                set(XVFB_RUN xvfb-run)
            endif()
            add_test(NAME desktop_clipboard_no_backend COMMAND desktop_clipboard_tests --reject-backend)
            set_tests_properties(desktop_clipboard_no_backend PROPERTIES TIMEOUT 60
                ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
            add_test(NAME desktop_clipboard_x11 COMMAND "${XVFB_RUN}" -a
                "${CMAKE_COMMAND}" -E env "QT_QPA_PLATFORM=xcb" $<TARGET_FILE:desktop_clipboard_tests>)
            set_tests_properties(desktop_clipboard_x11 PROPERTIES TIMEOUT 120)
        endif()
    endif()
    if(HUHUCAPTURE_RUST_VALIDATION)
        huhucapture_test(qt_gif_encoder tests/qt/gif_encoder_tests.cpp)
        target_link_libraries(qt_gif_encoder PRIVATE huhucapture_gif_encoder huhucapture_gif_inspection)
        set_tests_properties(qt_gif_encoder PROPERTIES TIMEOUT 120
            ENVIRONMENT "QT_QPA_PLATFORM=${HUHUCAPTURE_TEST_QPA}")
        huhucapture_test(qt_transfer_ui tests/qt/transfer_ui_tests.cpp
            "${PROJECT_BINARY_DIR}/transfer-render/$<CONFIG>")
        target_link_libraries(qt_transfer_ui PRIVATE huhucapture_transfer_ui
            huhucapture_receive_archive huhucapture_languages)
        set_tests_properties(qt_transfer_ui PROPERTIES TIMEOUT 240
            ENVIRONMENT "QT_QPA_PLATFORM=${HUHUCAPTURE_TEST_QPA}")
        huhucapture_test(qt_lan_controller tests/qt/lan_controller_tests.cpp
            "${PROJECT_BINARY_DIR}/lan-controller-render/$<CONFIG>")
        target_link_libraries(qt_lan_controller PRIVATE huhucapture_lan_controller huhucapture_languages)
        set_tests_properties(qt_lan_controller PROPERTIES TIMEOUT 300
            ENVIRONMENT "QT_QPA_PLATFORM=${HUHUCAPTURE_TEST_QPA}")
        huhucapture_test(qt_rust_bridge tests/qt/rust_bridge_probe.cpp
            "${PROJECT_BINARY_DIR}/rust-bridge/$<CONFIG>")
        target_link_libraries(qt_rust_bridge PRIVATE huhucapture_gif_inspection)
        huhucapture_test(qt_transfer_service tests/qt/transfer_service_tests.cpp)
        target_link_libraries(qt_transfer_service PRIVATE huhucapture_receive_archive
            huhucapture_gif_inspection Qt6::Network)
        set_tests_properties(qt_transfer_service PROPERTIES TIMEOUT 240
            ENVIRONMENT "QT_QPA_PLATFORM=${HUHUCAPTURE_TEST_QPA}")
        if(MSVC)
            target_link_options(qt_rust_bridge PRIVATE /WX)
            target_link_options(qt_transfer_service PRIVATE /WX)
            target_link_options(qt_transfer_ui PRIVATE /WX)
            target_link_options(qt_lan_controller PRIVATE /WX)
        endif()
        set_tests_properties(qt_rust_bridge PROPERTIES TIMEOUT 180
            ENVIRONMENT "QT_QPA_PLATFORM=${HUHUCAPTURE_TEST_QPA}")
    endif()
    if(WIN32)
        huhucapture_test(windows_audio_format tests/platform/windows_audio_format_tests.cpp)
        target_link_libraries(qt_image_contract PRIVATE huhucapture_native_media)
        huhucapture_test(windows_file_stream tests/platform/windows_file_stream_tests.cpp)
        target_link_libraries(windows_file_stream PRIVATE huhucapture_native_media)
        huhucapture_test(qt_recording_session tests/qt/recording_session_tests.cpp)
        target_link_libraries(qt_recording_session PRIVATE
            huhucapture_recording_session huhucapture_native_media)
        set_tests_properties(qt_recording_session PROPERTIES TIMEOUT 120)
        if(HUHUCAPTURE_RUST_VALIDATION)
            huhucapture_test(native_gif_recording_probe tests/qt/native_recording_probe.cpp
                "${PROJECT_BINARY_DIR}/native-gif-recording/$<CONFIG>")
            target_compile_definitions(native_gif_recording_probe PRIVATE HUHUCAPTURE_GIF_RECORDING_PROBE=1)
            target_link_libraries(native_gif_recording_probe PRIVATE Qt6::Widgets Qt6::Concurrent
                huhucapture_recording_session huhucapture_native_gif huhucapture_gif_inspection)
            set_tests_properties(native_gif_recording_probe PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE
                ENVIRONMENT "QT_QPA_PLATFORM=windows")
        endif()
    endif()
    if(HUHUCAPTURE_PORTAL_VALIDATION)
        find_program(DBUS_RUN_SESSION dbus-run-session REQUIRED)
        add_executable(portal_fault_tests tests/platform/portal_fault_tests.cpp)
        target_compile_definitions(portal_fault_tests PRIVATE QT_NO_KEYWORDS)
        target_link_libraries(portal_fault_tests PRIVATE huhucapture_portal PkgConfig::LIBPORTAL PkgConfig::GIO)
        add_test(NAME portal_missing_service
            COMMAND "${DBUS_RUN_SESSION}" -- $<TARGET_FILE:portal_fault_tests>)
        add_test(NAME portal_missing_session_bus COMMAND portal_fault_tests --no-bus)
        add_test(NAME portal_shutdown_initializing
            COMMAND "${DBUS_RUN_SESSION}" -- $<TARGET_FILE:portal_fault_tests> --shutdown-initializing)
        add_test(NAME portal_shutdown_initializing_no_bus
            COMMAND portal_fault_tests --no-bus --shutdown-initializing)
        add_test(NAME portal_shutdown_closed_admission
            COMMAND portal_fault_tests --shutdown-with-late-request)
        add_test(NAME portal_remote_without_session
            COMMAND portal_fault_tests --remote-without-session)
        set_tests_properties(portal_missing_service portal_missing_session_bus
            portal_shutdown_initializing portal_shutdown_initializing_no_bus
            portal_shutdown_closed_admission portal_remote_without_session PROPERTIES
            TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        add_executable(portal_shortcuts_fault_tests tests/platform/portal_shortcuts_fault_tests.cpp)
        target_compile_definitions(portal_shortcuts_fault_tests PRIVATE QT_NO_KEYWORDS)
        target_link_libraries(portal_shortcuts_fault_tests PRIVATE huhucapture_portal PkgConfig::LIBPORTAL PkgConfig::GIO)
        add_test(NAME portal_shortcuts_missing_service
            COMMAND "${DBUS_RUN_SESSION}" -- $<TARGET_FILE:portal_shortcuts_fault_tests>)
        add_test(NAME portal_shortcuts_missing_session_bus COMMAND portal_shortcuts_fault_tests --no-bus)
        add_test(NAME portal_session_bus_disconnection COMMAND portal_shortcuts_fault_tests --disconnect-bus)
        add_test(NAME portal_shortcuts_shutdown
            COMMAND "${DBUS_RUN_SESSION}" -- $<TARGET_FILE:portal_shortcuts_fault_tests> --shutdown)
        add_test(NAME portal_shortcuts_shutdown_no_bus
            COMMAND portal_shortcuts_fault_tests --shutdown --no-bus)
        set_tests_properties(portal_shortcuts_missing_service portal_shortcuts_missing_session_bus
            portal_session_bus_disconnection PROPERTIES TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        set_tests_properties(portal_shortcuts_shutdown portal_shortcuts_shutdown_no_bus
            PROPERTIES TIMEOUT 25 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        add_executable(portal_capture_probe tests/platform/portal_capture_probe.cpp)
        target_compile_definitions(portal_capture_probe PRIVATE QT_NO_KEYWORDS)
        target_link_libraries(portal_capture_probe PRIVATE huhucapture_portal)
        add_executable(portal_shortcuts_probe tests/platform/portal_shortcuts_probe.cpp)
        target_compile_definitions(portal_shortcuts_probe PRIVATE QT_NO_KEYWORDS)
        target_link_libraries(portal_shortcuts_probe PRIVATE huhucapture_portal)
        add_executable(portal_parent_probe tests/platform/portal_parent_probe.cpp)
        target_compile_definitions(portal_parent_probe PRIVATE QT_NO_KEYWORDS)
        target_link_libraries(portal_parent_probe PRIVATE huhucapture_portal Qt6::Widgets
            PkgConfig::WAYLAND_CLIENT PkgConfig::LIBPORTAL PkgConfig::GIO)
        # This executable requires a real desktop and user authorization. It is
        # deliberately not reported as a passing offscreen CTest capture test.
    endif()
    if(HUHUCAPTURE_PIPEWIRE_VALIDATION)
        huhucapture_test(pipewire_fd_tests tests/platform/pipewire_fd_tests.cpp)
        target_link_libraries(pipewire_fd_tests PRIVATE huhucapture_pipewire PkgConfig::PIPEWIRE)
        add_executable(pipewire_capture_probe tests/platform/pipewire_capture_probe.cpp)
        target_compile_definitions(pipewire_capture_probe PRIVATE QT_NO_KEYWORDS)
        target_link_libraries(pipewire_capture_probe PRIVATE huhucapture_pipewire
            huhucapture_portal Qt6::Widgets PkgConfig::PIPEWIRE)
        huhucapture_test(pipewire_audio_tests tests/platform/pipewire_audio_tests.cpp)
        target_link_libraries(pipewire_audio_tests PRIVATE huhucapture_pipewire_audio)
        add_executable(pipewire_audio_signal tests/platform/pipewire_audio_signal.cpp)
        target_link_libraries(pipewire_audio_signal PRIVATE huhucapture_core PkgConfig::PIPEWIRE Threads::Threads)
        add_executable(pipewire_audio_graph_probe tests/platform/pipewire_audio_graph_probe.cpp)
        target_link_libraries(pipewire_audio_graph_probe PRIVATE huhucapture_pipewire_audio PkgConfig::PIPEWIRE Threads::Threads)
        set_target_properties(pipewire_audio_signal pipewire_audio_graph_probe PROPERTIES CXX_EXTENSIONS OFF)
        # Actual Portal/PipeWire frames require an owned compositor and daemon;
        # the isolated wrapper in CI produces the corresponding evidence.
    endif()
    if(WIN32 OR HUHUCAPTURE_PORTAL_VALIDATION)
        huhucapture_test(qt_capture_window tests/qt/capture_window_tests.cpp
            "${PROJECT_BINARY_DIR}/capture-render/$<CONFIG>")
        target_link_libraries(qt_capture_window PRIVATE huhucapture_capture_ui huhucapture_languages)
        set_tests_properties(qt_capture_window PROPERTIES TIMEOUT 120
            ENVIRONMENT "QT_QPA_PLATFORM=${HUHUCAPTURE_TEST_QPA}")
        add_executable(window_activation_tests tests/qt/window_activation_tests.cpp)
        target_link_libraries(window_activation_tests PRIVATE huhucapture_window_activation Qt6::Widgets)
        set_target_properties(window_activation_tests PROPERTIES CXX_EXTENSIONS OFF)
        add_test(NAME window_activation_unavailable COMMAND window_activation_tests)
        set_tests_properties(window_activation_unavailable PROPERTIES TIMEOUT 30
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        if(WIN32)
            add_test(NAME window_activation_native COMMAND window_activation_tests)
            set_tests_properties(window_activation_native PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=windows")
        else()
            find_program(XVFB_RUN xvfb-run)
            if(NOT XVFB_RUN)
                set(XVFB_RUN xvfb-run)
            endif()
            add_test(NAME window_activation_native COMMAND "${XVFB_RUN}" -a
                "${CMAKE_COMMAND}" -E env "QT_QPA_PLATFORM=xcb" $<TARGET_FILE:window_activation_tests>)
        endif()
        set_tests_properties(window_activation_native PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)
        add_executable(screen_capture_tests tests/qt/screen_capture_tests.cpp)
        target_link_libraries(screen_capture_tests PRIVATE huhucapture_screen_capture Qt6::Widgets)
        set_target_properties(screen_capture_tests PROPERTIES CXX_EXTENSIONS OFF)
        add_test(NAME screen_capture_unavailable COMMAND screen_capture_tests --no-backend)
        set_tests_properties(screen_capture_unavailable PROPERTIES TIMEOUT 45 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        if(WIN32)
            add_test(NAME screen_capture_native COMMAND screen_capture_tests
                --artifacts "${PROJECT_BINARY_DIR}/native-capture/$<CONFIG>")
            set_tests_properties(screen_capture_native PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=windows")
        else()
            add_test(NAME screen_capture_native COMMAND "${XVFB_RUN}" -a
                "${CMAKE_COMMAND}" -E env "QT_QPA_PLATFORM=xcb" $<TARGET_FILE:screen_capture_tests>
                --artifacts "${PROJECT_BINARY_DIR}/native-capture/$<CONFIG>")
            add_executable(x11_controls_probe tests/platform/x11_controls_probe.cpp)
            target_link_libraries(x11_controls_probe PRIVATE huhucapture_x11_capture
                Qt6::Widgets Qt6::Concurrent PkgConfig::XCB_CAPTURE)
            set_target_properties(x11_controls_probe PROPERTIES CXX_EXTENSIONS OFF)
            add_test(NAME x11_controls_native COMMAND "${XVFB_RUN}" -a
                "${CMAKE_COMMAND}" -E env "QT_QPA_PLATFORM=xcb" $<TARGET_FILE:x11_controls_probe>)
            set_tests_properties(x11_controls_native PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
        endif()
        set_tests_properties(screen_capture_native PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
        if(TARGET huhucapture_capture_workspace)
            add_executable(capture_workspace_tests tests/qt/capture_workspace_tests.cpp)
            target_link_libraries(capture_workspace_tests PRIVATE huhucapture_recording_workspace huhucapture_languages)
            set_target_properties(capture_workspace_tests PROPERTIES CXX_EXTENSIONS OFF)
            add_test(NAME capture_workspace_unavailable COMMAND capture_workspace_tests --unavailable)
            set_tests_properties(capture_workspace_unavailable PROPERTIES TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
            if(WIN32)
                add_test(NAME capture_workspace_native COMMAND capture_workspace_tests)
                set_tests_properties(capture_workspace_native PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=windows")
            else()
                add_test(NAME capture_workspace_native COMMAND "${XVFB_RUN}" -a
                    "${CMAKE_COMMAND}" -E env "QT_QPA_PLATFORM=xcb" $<TARGET_FILE:capture_workspace_tests>)
            endif()
            set_tests_properties(capture_workspace_native PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
        endif()
        huhucapture_test(qt_shortcut_shutdown tests/qt/shortcut_shutdown_tests.cpp)
        target_link_libraries(qt_shortcut_shutdown PRIVATE huhucapture_settings_controller)
        set_tests_properties(qt_shortcut_shutdown PROPERTIES TIMEOUT 60
            ENVIRONMENT "QT_QPA_PLATFORM=${HUHUCAPTURE_TEST_QPA}")
        if(HUHUCAPTURE_NATIVE_INPUT_VALIDATION)
            add_executable(qt_settings_controller tests/qt/settings_controller_tests.cpp)
            target_link_libraries(qt_settings_controller PRIVATE huhucapture_settings_controller)
            set_target_properties(qt_settings_controller PROPERTIES CXX_EXTENSIONS OFF)
            if(WIN32)
                target_link_libraries(qt_settings_controller PRIVATE user32 advapi32 shell32)
                add_test(NAME qt_settings_controller COMMAND qt_settings_controller
                    "${PROJECT_BINARY_DIR}/settings-controller/$<CONFIG>")
                set_tests_properties(qt_settings_controller PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=windows")
            else()
                pkg_check_modules(XCB_XTEST REQUIRED IMPORTED_TARGET xcb-xtest)
                target_compile_definitions(qt_settings_controller PRIVATE QT_NO_KEYWORDS)
                target_link_libraries(qt_settings_controller PRIVATE PkgConfig::XCB_XTEST PkgConfig::XCB_INPUT PkgConfig::GIO)
                add_test(NAME qt_settings_controller COMMAND "${XVFB_RUN}" -a
                    "${CMAKE_COMMAND}" -E env "QT_QPA_PLATFORM=xcb" $<TARGET_FILE:qt_settings_controller>
                    "${PROJECT_BINARY_DIR}/settings-controller/$<CONFIG>")
                add_test(NAME qt_settings_controller_unavailable COMMAND qt_settings_controller
                    "${PROJECT_BINARY_DIR}/settings-controller-unavailable/$<CONFIG>" --unavailable)
                set_tests_properties(qt_settings_controller_unavailable PROPERTIES TIMEOUT 90
                    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
            endif()
            set_tests_properties(qt_settings_controller PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
        else()
            message(STATUS "Real native shortcut/input validation explicitly disabled for this component build; not a full Qt test run")
        endif()
    endif()
    if(HUHUCAPTURE_FFMPEG_VALIDATION)
        add_library(ffmpeg_mux_example OBJECT third_party/ffmpeg-examples/mux.c)
        target_compile_definitions(ffmpeg_mux_example PRIVATE main=huhucapture_ffmpeg_mux_example_main)
        target_link_libraries(ffmpeg_mux_example PRIVATE PkgConfig::FFMPEG)
        huhucapture_test(qt_software_codec tests/qt/software_codec_probe.cpp
            "${PROJECT_BINARY_DIR}/software-codec/$<CONFIG>")
        target_link_libraries(qt_software_codec PRIVATE Qt6::Gui PkgConfig::FFMPEG
            huhucapture_file_committer ffmpeg_mux_example m)
        set_tests_properties(qt_software_codec PROPERTIES TIMEOUT 120 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()
    if(HUHUCAPTURE_VIDEO_VALIDATION)
        set(HUHUCAPTURE_VIDEO_FIXTURES "${PROJECT_SOURCE_DIR}/artifacts/video-fixtures"
            CACHE PATH "Identical real encoded MP4 fixtures generated by the pinned Linux encoder")
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            add_executable(video_inspection_fixtures tests/qt/video_inspection_fixtures.cpp)
            target_link_libraries(video_inspection_fixtures PRIVATE huhucapture_core Qt6::Core
                PkgConfig::FFMPEG ffmpeg_mux_example m)
            set_target_properties(video_inspection_fixtures PROPERTIES CXX_EXTENSIONS OFF)
            huhucapture_test(qt_mp4_encoder tests/qt/mp4_encoder_tests.cpp
                "${PROJECT_BINARY_DIR}/mp4-encoder/$<CONFIG>")
            target_link_libraries(qt_mp4_encoder PRIVATE huhucapture_mp4_encoder
                huhucapture_video_inspection PkgConfig::FFMPEG Qt6::Core)
            set_tests_properties(qt_mp4_encoder PROPERTIES TIMEOUT 180)
        endif()
        huhucapture_test(qt_video_inspection tests/qt/video_inspection_tests.cpp
            "${HUHUCAPTURE_VIDEO_FIXTURES}" "${PROJECT_BINARY_DIR}/video-inspection/$<CONFIG>")
        target_link_libraries(qt_video_inspection PRIVATE huhucapture_video_inspection Threads::Threads)
        add_test(NAME qt_hevc_inspection COMMAND qt_video_inspection
            "${HUHUCAPTURE_VIDEO_FIXTURES}" "${PROJECT_BINARY_DIR}/hevc-inspection/$<CONFIG>" --hevc)
        set_tests_properties(qt_video_inspection qt_hevc_inspection PROPERTIES TIMEOUT 240)
        add_executable(legacy_mp3_recording_tests tests/qt/legacy_mp3_recording_tests.cpp)
        set_target_properties(legacy_mp3_recording_tests PROPERTIES CXX_EXTENSIONS OFF)
        target_link_libraries(legacy_mp3_recording_tests PRIVATE huhucapture_video_inspection)
        set(HUHUCAPTURE_LEGACY_MP3_FIXTURES "" CACHE PATH "Actual Windows MF H264 plus MP3 fixture directory")
        if(HUHUCAPTURE_LEGACY_MP3_FIXTURES)
            add_test(NAME qt_legacy_mp3_recording COMMAND legacy_mp3_recording_tests "${HUHUCAPTURE_LEGACY_MP3_FIXTURES}")
            set_tests_properties(qt_legacy_mp3_recording PROPERTIES TIMEOUT 180)
        endif()
        if(WIN32)
            target_link_libraries(legacy_mp3_recording_tests PRIVATE
                huhucapture_native_media huhucapture_video_ffmpeg)
            if(HUHUCAPTURE_LEGACY_MP3_FIXTURES)
                set_tests_properties(qt_legacy_mp3_recording PROPERTIES ENVIRONMENT_MODIFICATION
                    "PATH=path_list_prepend:$<TARGET_FILE_DIR:huhucapture_ffmpeg_avformat>")
            endif()
            set_tests_properties(qt_video_inspection qt_hevc_inspection PROPERTIES ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:huhucapture_ffmpeg_avformat>")
            huhucapture_test(native_recording_probe tests/qt/native_recording_probe.cpp
                "${PROJECT_BINARY_DIR}/native-recording/$<CONFIG>")
            target_link_libraries(native_recording_probe PRIVATE Qt6::Widgets Qt6::Concurrent
                huhucapture_recording_session huhucapture_native_media huhucapture_video_inspection
                huhucapture_video_ffmpeg)
            set_tests_properties(native_recording_probe PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE
                ENVIRONMENT "QT_QPA_PLATFORM=windows"
                ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:huhucapture_ffmpeg_avformat>")
        else()
            target_link_libraries(legacy_mp3_recording_tests PRIVATE PkgConfig::FFMPEG)
        endif()
    endif()
    if(TARGET huhucapture_scroll_ui)
        add_executable(scroll_capture_tests tests/qt/scroll_capture_tests.cpp)
        target_link_libraries(scroll_capture_tests PRIVATE huhucapture_scroll_ui)
        set_target_properties(scroll_capture_tests PROPERTIES CXX_EXTENSIONS OFF)
        add_test(NAME scroll_capture_unavailable COMMAND scroll_capture_tests --unavailable)
        set_tests_properties(scroll_capture_unavailable PROPERTIES TIMEOUT 45
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        if(WIN32)
            add_test(NAME scroll_capture_native COMMAND scroll_capture_tests
                "${PROJECT_BINARY_DIR}/scroll-render/$<CONFIG>")
            set_tests_properties(scroll_capture_native PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=windows")
        else()
            add_test(NAME scroll_capture_native COMMAND "${XVFB_RUN}" -a -s "-screen 0 1024x768x24"
                "${CMAKE_COMMAND}" -E env --unset=WAYLAND_DISPLAY --unset=XDG_SESSION_TYPE "QT_QPA_PLATFORM=xcb"
                $<TARGET_FILE:scroll_capture_tests> "${PROJECT_BINARY_DIR}/scroll-render/$<CONFIG>")
        endif()
        set_tests_properties(scroll_capture_native PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
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
    if(TARGET huhucapture_x11_recording AND TARGET huhucapture_video_inspection)
        huhucapture_test(live_recording_tests tests/qt/live_recording_tests.cpp)
        target_link_libraries(live_recording_tests PRIVATE huhucapture_live_recording
            huhucapture_gif_inspection huhucapture_video_inspection)
        set_tests_properties(live_recording_tests PROPERTIES TIMEOUT 180)
        huhucapture_test(x11_recording_failure_tests tests/qt/x11_recording_failure_tests.cpp)
        target_link_libraries(x11_recording_failure_tests PRIVATE huhucapture_x11_recording)
        add_executable(x11_recording_probe tests/qt/x11_recording_probe.cpp)
        target_link_libraries(x11_recording_probe PRIVATE huhucapture_x11_recording
            huhucapture_gif_inspection huhucapture_video_inspection Qt6::Widgets Qt6::Concurrent PkgConfig::XCB_CAPTURE)
        set_target_properties(x11_recording_probe PROPERTIES CXX_EXTENSIONS OFF)
        add_test(NAME x11_recording_native COMMAND "${XVFB_RUN}" -a -s "-screen 0 1024x768x24"
            "${CMAKE_COMMAND}" -E env --unset=WAYLAND_DISPLAY --unset=XDG_SESSION_TYPE "QT_QPA_PLATFORM=xcb"
            $<TARGET_FILE:x11_recording_probe> "${PROJECT_BINARY_DIR}/x11-recording/$<CONFIG>")
        set_tests_properties(x11_recording_native PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
    endif()
endif()


if(HUHUCAPTURE_BUILD_APP)
    # This external driver launches the unchanged installed product and reads
    # its actual desktop and loaded modules. It is never linked or installed
    # into the application; native package lifecycle runs after packaging.
    add_executable(installation_gui_probe tests/qt/installation_gui_probe.cpp)
    target_link_libraries(installation_gui_probe PRIVATE huhucapture_screen_capture Qt6::Widgets)
    set_target_properties(installation_gui_probe PROPERTIES CXX_EXTENSIONS OFF)
    if(WIN32)
        target_link_libraries(installation_gui_probe PRIVATE user32 psapi ole32 shell32 uuid)
    else()
        target_link_libraries(installation_gui_probe PRIVATE PkgConfig::XCB_CAPTURE)
    endif()
    add_executable(package_runtime_probe tests/qt/package_runtime_probe.cpp)
    target_link_libraries(package_runtime_probe PRIVATE huhucapture_core Qt6::Network)
    if(WIN32)
        target_link_libraries(package_runtime_probe PRIVATE psapi)
    else()
        target_link_libraries(package_runtime_probe PRIVATE ${CMAKE_DL_LIBS})
    endif()
    set_target_properties(package_runtime_probe PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME package_metadata COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/package_metadata_tests.py")
    set_tests_properties(package_metadata PROPERTIES TIMEOUT 90)
    huhucapture_test(qt_application_lifecycle tests/qt/application_lifecycle_tests.cpp
        "${PROJECT_BINARY_DIR}/application-lifecycle/$<CONFIG>")
    target_link_libraries(qt_application_lifecycle PRIVATE huhucapture_application)
    set_tests_properties(qt_application_lifecycle PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    if(WIN32)
        set_tests_properties(qt_application_lifecycle PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:huhucapture_ffmpeg_avformat>")
    else()
        add_test(NAME qt_application_clipboard_owner COMMAND "${XVFB_RUN}" -a
            "${CMAKE_COMMAND}" -E env "QT_QPA_PLATFORM=xcb" $<TARGET_FILE:qt_application_lifecycle>
            "${PROJECT_BINARY_DIR}/application-clipboard-owner/$<CONFIG>" --clipboard-owner)
        set_tests_properties(qt_application_clipboard_owner PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
    endif()
endif()
