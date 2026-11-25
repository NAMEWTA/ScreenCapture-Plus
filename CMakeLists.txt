cmake_minimum_required(VERSION 3.25)
project(huhucapture LANGUAGES CXX)

# During P00/P03 this builds the shared contracts and migration validation only.
# The shipping Windows application remains on its verified build until P12.
# Do not mistake a passing core-only configuration for a Qt product build.
include(CTest)
option(HUHUCAPTURE_QT_VALIDATION "Build the pinned Qt migration validation targets" OFF)
option(HUHUCAPTURE_PORTAL_VALIDATION "Build the real Linux Portal control-plane validation" OFF)
option(HUHUCAPTURE_FFMPEG_VALIDATION "Build the pinned software-codec API validation" OFF)
if(HUHUCAPTURE_PORTAL_VALIDATION AND NOT HUHUCAPTURE_QT_VALIDATION)
    message(FATAL_ERROR "Portal validation requires HUHUCAPTURE_QT_VALIDATION=ON")
endif()
if(HUHUCAPTURE_FFMPEG_VALIDATION AND NOT HUHUCAPTURE_QT_VALIDATION)
    message(FATAL_ERROR "Software-codec validation requires HUHUCAPTURE_QT_VALIDATION=ON")
endif()

add_library(huhucapture_core INTERFACE)
target_include_directories(huhucapture_core INTERFACE "${PROJECT_SOURCE_DIR}/src")
target_compile_features(huhucapture_core INTERFACE cxx_std_20)
if(MSVC)
    target_compile_options(huhucapture_core INTERFACE /utf-8 /permissive-)
endif()

# Filesystem publication is an OS seam; the archive naming/ownership rules stay
# shared. This target never depends on Qt or the presentation layer.
if(WIN32)
    add_library(huhucapture_file_committer STATIC src/platform/windows/FileCommitter.cpp)
    target_link_libraries(huhucapture_file_committer PRIVATE ole32)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(huhucapture_file_committer STATIC src/platform/linux/FileCommitter.cpp)
endif()
if(TARGET huhucapture_file_committer)
    target_link_libraries(huhucapture_file_committer PUBLIC huhucapture_core)
    set_target_properties(huhucapture_file_committer PROPERTIES CXX_EXTENSIONS OFF)
endif()

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
    huhucapture_test(pixel_buffer tests/PixelBufferTests.cpp)
    huhucapture_test(annotations tests/annotation_tests.cpp)
    huhucapture_test(annotation_regions tests/annotation_region_tests.cpp)
    huhucapture_test(annotation_panel tests/annotation_panel_layout_tests.cpp)
    huhucapture_test(history tests/history_tests.cpp)
    huhucapture_test(scroll_stitch tests/scroll_stitch_tests.cpp)
    huhucapture_test(toolbar_layout tests/toolbar_layout_tests.cpp)
    huhucapture_test(media_file_names tests/media_file_name_tests.cpp)
    huhucapture_test(release_versions tests/release_version_tests.cpp)
    huhucapture_test(annotation_model tests/annotation_model_tests.cpp)
    huhucapture_test(box_model tests/box_model_tests.cpp)
    huhucapture_test(stroke_model tests/stroke_model_tests.cpp)
    huhucapture_test(arrow_model tests/arrow_model_tests.cpp)
    huhucapture_test(number_model tests/number_model_tests.cpp)
    huhucapture_test(text_model tests/text_model_tests.cpp)
    huhucapture_test(editor_document tests/editor_document_tests.cpp)
    huhucapture_test(recording_results tests/recording_result_tests.cpp)
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
    if(NOT BUILD_TESTING)
        message(FATAL_ERROR "Qt migration validation requires BUILD_TESTING=ON")
    endif()
    # No fallback to a distribution Qt with a different private ABI.
    find_package(Qt6 6.12.1 EXACT REQUIRED COMPONENTS Core Gui Widgets Network Svg)
    if(WIN32)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
        # The offscreen Windows plugin supplies a placeholder font engine.
        # Use the native font database without constructing visible windows.
        set(HUHUCAPTURE_TEST_QPA windows)
    else()
        set(HUHUCAPTURE_TEST_QPA offscreen)
    endif()
    huhucapture_test(qt_image_contract tests/qt/image_contract_tests.cpp)
    target_link_libraries(qt_image_contract PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Svg)
    set_tests_properties(qt_image_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${HUHUCAPTURE_TEST_QPA}")
    add_library(huhucapture_annotation_renderer STATIC src/services/rendering/QtAnnotationRenderer.cpp)
    target_link_libraries(huhucapture_annotation_renderer PUBLIC huhucapture_core Qt6::Gui)
    huhucapture_test(qt_annotation_render tests/qt/annotation_render_tests.cpp
        "${PROJECT_BINARY_DIR}/annotation-render/$<CONFIG>")
    target_link_libraries(qt_annotation_render PRIVATE huhucapture_annotation_renderer)
    set_tests_properties(qt_annotation_render PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${HUHUCAPTURE_TEST_QPA}")
    if(WIN32)
        add_test(NAME qt_annotation_missing_font
            COMMAND qt_annotation_render --expect-font-unavailable)
        set_tests_properties(qt_annotation_missing_font PROPERTIES TIMEOUT 60
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_FONTDIR=${PROJECT_BINARY_DIR}/intentionally-empty-fonts")
    endif()
    add_library(huhucapture_json STATIC src/services/config/JsonText.cpp)
    target_link_libraries(huhucapture_json PUBLIC huhucapture_core Qt6::Core)
    add_library(huhucapture_settings STATIC src/services/config/SettingsStore.cpp)
    target_link_libraries(huhucapture_settings PUBLIC huhucapture_json
        PRIVATE huhucapture_file_committer)
    huhucapture_test(qt_settings_store tests/qt/settings_store_tests.cpp)
    target_link_libraries(qt_settings_store PRIVATE huhucapture_settings)
    add_library(huhucapture_languages STATIC
        src/services/localization/LanguageCatalog.h
        src/services/localization/LanguageCatalog.cpp
        resources/qt/huhucapture_languages.qrc)
    set_target_properties(huhucapture_languages PROPERTIES AUTOMOC ON AUTORCC ON CXX_EXTENSIONS OFF)
    target_link_libraries(huhucapture_languages PUBLIC huhucapture_json)
    huhucapture_test(qt_language_catalog tests/qt/language_catalog_tests.cpp)
    target_link_libraries(qt_language_catalog PRIVATE huhucapture_languages)
    if(WIN32)
        huhucapture_test(windows_audio_format tests/platform/windows_audio_format_tests.cpp)
        # Compile the retained native backends without Ling, its PCH, or /MT.
        # This verifies the Qt/native link boundary, not hardware capture.
        add_library(huhucapture_native_media STATIC
            src/platform/windows/GdiCapture.cpp
            src/recording/VideoMp4.cpp)
        target_link_libraries(huhucapture_native_media PUBLIC huhucapture_core
            gdi32 user32 ole32 oleaut32 uuid shell32 shlwapi propsys
            d3d11 dxgi windowscodecs mf mfplat mfreadwrite mfuuid
            wininet comctl32)
        target_compile_definitions(huhucapture_native_media PUBLIC
            UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
        target_compile_options(huhucapture_native_media PRIVATE /W4 /utf-8 /permissive-)
        target_link_libraries(qt_image_contract PRIVATE huhucapture_native_media)
    endif()
    if(HUHUCAPTURE_PORTAL_VALIDATION)
        if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
            message(FATAL_ERROR "Portal validation is only supported on Linux")
        endif()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(LIBPORTAL REQUIRED IMPORTED_TARGET libportal=0.11.0)
        pkg_check_modules(LIBPORTAL_QT REQUIRED IMPORTED_TARGET libportal-qt6=0.11.0)
        pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-unix-2.0>=2.80)
        find_program(DBUS_RUN_SESSION dbus-run-session REQUIRED)
        add_library(huhucapture_portal STATIC
            src/platform/linux/portal/PortalTypes.h
            src/platform/linux/portal/PortalSupport.h
            src/platform/linux/portal/PortalCapture.h
            src/platform/linux/portal/PortalCapture.cpp
            src/platform/linux/portal/PortalShortcuts.h
            src/platform/linux/portal/PortalShortcuts.cpp)
        set_target_properties(huhucapture_portal PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
        target_compile_definitions(huhucapture_portal PRIVATE QT_NO_KEYWORDS)
        target_link_libraries(huhucapture_portal PUBLIC huhucapture_core Qt6::Core Qt6::Gui
            PRIVATE Qt6::Widgets PkgConfig::LIBPORTAL PkgConfig::LIBPORTAL_QT PkgConfig::GIO)
        target_compile_options(huhucapture_portal PRIVATE -Wall -Wextra -Wpedantic)
        add_executable(portal_fault_tests tests/platform/portal_fault_tests.cpp)
        target_compile_definitions(portal_fault_tests PRIVATE QT_NO_KEYWORDS)
        target_link_libraries(portal_fault_tests PRIVATE huhucapture_portal PkgConfig::LIBPORTAL PkgConfig::GIO)
        add_test(NAME portal_missing_service
            COMMAND "${DBUS_RUN_SESSION}" -- $<TARGET_FILE:portal_fault_tests>)
        add_test(NAME portal_missing_session_bus COMMAND portal_fault_tests --no-bus)
        set_tests_properties(portal_missing_service portal_missing_session_bus PROPERTIES
            TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        add_executable(portal_shortcuts_fault_tests tests/platform/portal_shortcuts_fault_tests.cpp)
        target_compile_definitions(portal_shortcuts_fault_tests PRIVATE QT_NO_KEYWORDS)
        target_link_libraries(portal_shortcuts_fault_tests PRIVATE huhucapture_portal PkgConfig::LIBPORTAL PkgConfig::GIO)
        add_test(NAME portal_shortcuts_missing_service
            COMMAND "${DBUS_RUN_SESSION}" -- $<TARGET_FILE:portal_shortcuts_fault_tests>)
        add_test(NAME portal_shortcuts_missing_session_bus COMMAND portal_shortcuts_fault_tests --no-bus)
        add_test(NAME portal_session_bus_disconnection COMMAND portal_shortcuts_fault_tests --disconnect-bus)
        set_tests_properties(portal_shortcuts_missing_service portal_shortcuts_missing_session_bus
            portal_session_bus_disconnection PROPERTIES TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        add_executable(portal_capture_probe tests/platform/portal_capture_probe.cpp)
        target_compile_definitions(portal_capture_probe PRIVATE QT_NO_KEYWORDS)
        target_link_libraries(portal_capture_probe PRIVATE huhucapture_portal)
        add_executable(portal_shortcuts_probe tests/platform/portal_shortcuts_probe.cpp)
        target_compile_definitions(portal_shortcuts_probe PRIVATE QT_NO_KEYWORDS)
        target_link_libraries(portal_shortcuts_probe PRIVATE huhucapture_portal)
        # This executable requires a real desktop and user authorization. It is
        # deliberately not reported as a passing offscreen CTest capture test.
    endif()
    if(HUHUCAPTURE_FFMPEG_VALIDATION)
        if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
            message(FATAL_ERROR "The software-codec validation dependency build currently targets Linux")
        endif()
        enable_language(C)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
            libavcodec=63.1.102 libavformat=63.1.102 libavutil=61.1.102
            libswscale=10.1.102 libswresample=7.1.102)
        add_library(ffmpeg_mux_example OBJECT third_party/ffmpeg-examples/mux.c)
        target_compile_definitions(ffmpeg_mux_example PRIVATE main=huhucapture_ffmpeg_mux_example_main)
        target_link_libraries(ffmpeg_mux_example PRIVATE PkgConfig::FFMPEG)
        huhucapture_test(qt_software_codec tests/qt/software_codec_probe.cpp
            "${PROJECT_BINARY_DIR}/software-codec/$<CONFIG>")
        target_link_libraries(qt_software_codec PRIVATE Qt6::Gui PkgConfig::FFMPEG
            huhucapture_file_committer ffmpeg_mux_example m)
        set_tests_properties(qt_software_codec PROPERTIES TIMEOUT 120 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()
endif()
