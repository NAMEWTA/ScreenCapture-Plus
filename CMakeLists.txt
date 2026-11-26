cmake_minimum_required(VERSION 3.25)
include(build/cmake/version.cmake)
project(huhucapture VERSION "${HUHUCAPTURE_VERSION}" LANGUAGES C CXX)

configure_file(src/app/Version.h.in generated/app/Version.h @ONLY)
add_library(huhucapture_version INTERFACE)
target_include_directories(huhucapture_version INTERFACE "${PROJECT_BINARY_DIR}/generated")
target_compile_features(huhucapture_version INTERFACE cxx_std_20)

# The default builds independent contracts. HUHUCAPTURE_BUILD_APP explicitly
# assembles the unified Qt product with its real platform and media services.
# Stable Windows delivery stays on the retained build until P12 acceptance.
include(CTest)
if(BUILD_TESTING)
    find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
    find_package(Git REQUIRED)
    add_test(NAME version_source COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/version_source_tests.py")
    set_tests_properties(version_source PROPERTIES TIMEOUT 90 ENVIRONMENT "HUHU_CMAKE=${CMAKE_COMMAND}")
    add_test(NAME source_bundle COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/source_bundle_tests.py" -v)
    set_tests_properties(source_bundle PROPERTIES TIMEOUT 180)
    add_test(NAME source_retention COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/source_retention_tests.py" -v)
    set_tests_properties(source_retention PROPERTIES TIMEOUT 90)
    add_test(NAME package_runtime_environment COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/package_runtime_environment_tests.py")
    set_tests_properties(package_runtime_environment PROPERTIES TIMEOUT 15)
    add_test(NAME native_source_materials COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/native_source_materials_tests.py")
    set_tests_properties(native_source_materials PROPERTIES TIMEOUT 60)
    add_test(NAME source_receipts COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/source_receipts_tests.py")
    set_tests_properties(source_receipts PROPERTIES TIMEOUT 90)
    add_test(NAME appimage_runtime_source COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/appimage_runtime_tests.py")
    set_tests_properties(appimage_runtime_source PROPERTIES TIMEOUT 90)
    add_test(NAME application_receipts COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/application_receipts_tests.py")
    set_tests_properties(application_receipts PROPERTIES TIMEOUT 90)
    add_test(NAME windows_qt_builder COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/windows_qt_builder_tests.py")
    set_tests_properties(windows_qt_builder PROPERTIES TIMEOUT 60)
    add_test(NAME linux_qt_builder COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/linux_qt_builder_tests.py")
    set_tests_properties(linux_qt_builder PROPERTIES TIMEOUT 60)
    add_test(NAME bridge_release COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/bridge_release_tests.py")
    set_tests_properties(bridge_release PROPERTIES TIMEOUT 60)
endif()
option(HUHUCAPTURE_BUILD_APP "Build the unified Qt desktop application" OFF)
option(HUHUCAPTURE_X11_TESTS "Run real X11 clipboard, capture and shortcut tests" ON)
option(HUHUCAPTURE_QT_VALIDATION "Build the pinned Qt migration validation targets" OFF)
option(HUHUCAPTURE_PORTAL_VALIDATION "Build the real Linux Portal control-plane validation" OFF)
option(HUHUCAPTURE_FFMPEG_VALIDATION "Build the pinned software-codec API validation" OFF)
option(HUHUCAPTURE_VIDEO_VALIDATION "Inspect the same real complete and malformed MP4 files on both platforms" OFF)
option(HUHUCAPTURE_DESKTOP_VALIDATION "Build real system clipboard, trash and file-manager service validation" OFF)
option(HUHUCAPTURE_PIPEWIRE_VALIDATION "Build the fixed Linux PipeWire data-plane validation" OFF)
option(HUHUCAPTURE_RUST_VALIDATION "Link one Rust aggregate into the real Qt ABI probe" OFF)
if(HUHUCAPTURE_BUILD_APP)
    set(HUHUCAPTURE_QT_VALIDATION ON)
    set(HUHUCAPTURE_DESKTOP_VALIDATION ON)
    set(HUHUCAPTURE_RUST_VALIDATION ON)
    set(HUHUCAPTURE_VIDEO_VALIDATION ON)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(HUHUCAPTURE_PORTAL_VALIDATION ON)
        set(HUHUCAPTURE_PIPEWIRE_VALIDATION ON)
        set(HUHUCAPTURE_FFMPEG_VALIDATION ON)
    endif()
endif()
if(HUHUCAPTURE_PORTAL_VALIDATION AND NOT HUHUCAPTURE_QT_VALIDATION)
    message(FATAL_ERROR "Portal validation requires HUHUCAPTURE_QT_VALIDATION=ON")
endif()
if(HUHUCAPTURE_PIPEWIRE_VALIDATION AND NOT HUHUCAPTURE_PORTAL_VALIDATION)
    message(FATAL_ERROR "PipeWire validation requires HUHUCAPTURE_PORTAL_VALIDATION=ON")
endif()
if(HUHUCAPTURE_FFMPEG_VALIDATION AND NOT HUHUCAPTURE_QT_VALIDATION)
    message(FATAL_ERROR "Software-codec validation requires HUHUCAPTURE_QT_VALIDATION=ON")
endif()
if(HUHUCAPTURE_VIDEO_VALIDATION AND NOT HUHUCAPTURE_QT_VALIDATION)
    message(FATAL_ERROR "Video inspection validation requires HUHUCAPTURE_QT_VALIDATION=ON")
endif()
if(HUHUCAPTURE_DESKTOP_VALIDATION AND NOT HUHUCAPTURE_QT_VALIDATION)
    message(FATAL_ERROR "Desktop service validation requires HUHUCAPTURE_QT_VALIDATION=ON")
endif()
if(HUHUCAPTURE_VIDEO_VALIDATION AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT HUHUCAPTURE_FFMPEG_VALIDATION)
    message(FATAL_ERROR "Linux video inspection requires the pinned FFmpeg dependency set")
endif()
if(HUHUCAPTURE_RUST_VALIDATION AND NOT HUHUCAPTURE_QT_VALIDATION)
    message(FATAL_ERROR "Rust aggregate validation requires HUHUCAPTURE_QT_VALIDATION=ON")
endif()
if(HUHUCAPTURE_RUST_VALIDATION AND WIN32)
    if(CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_CONFIGURATION_TYPES "Release;RelWithDebInfo" CACHE STRING
            "Rust's supplied Windows standard library uses the release dynamic CRT" FORCE)
    elseif(NOT CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo)$")
        message(FATAL_ERROR "Use Release or RelWithDebInfo for the Windows Rust aggregate; Qt Debug/MDd is a separate unverified toolchain")
    endif()
endif()

add_library(huhucapture_core INTERFACE)
target_link_libraries(huhucapture_core INTERFACE huhucapture_version)
target_include_directories(huhucapture_core INTERFACE "${PROJECT_SOURCE_DIR}/src")
target_compile_features(huhucapture_core INTERFACE cxx_std_20)
if(MSVC)
    target_compile_options(huhucapture_core INTERFACE /utf-8 /permissive-)
endif()

find_package(Threads REQUIRED)
add_library(huhucapture_media_frames STATIC src/core/MediaFrame.cpp src/core/FrameQueue.cpp)
target_link_libraries(huhucapture_media_frames PUBLIC huhucapture_core Threads::Threads)
set_target_properties(huhucapture_media_frames PROPERTIES CXX_EXTENSIONS OFF)
add_library(huhucapture_frame_normalizer STATIC src/services/media/FrameNormalizer.cpp)
target_link_libraries(huhucapture_frame_normalizer PUBLIC huhucapture_media_frames)
set_target_properties(huhucapture_frame_normalizer PROPERTIES CXX_EXTENSIONS OFF)
add_library(huhucapture_scroll_model STATIC src/capture/ScrollCaptureModel.cpp)
target_link_libraries(huhucapture_scroll_model PUBLIC huhucapture_core)
set_target_properties(huhucapture_scroll_model PROPERTIES CXX_EXTENSIONS OFF)

# Reuse the production decoder and exactly its 16-bit region configuration in
# both the existing native product and the shared Qt recognition service.
add_library(huhucapture_quirc STATIC third_party/quirc/quirc.c third_party/quirc/identify.c
    third_party/quirc/decode.c third_party/quirc/version_db.c)
target_include_directories(huhucapture_quirc PUBLIC "${PROJECT_SOURCE_DIR}/third_party")
target_compile_definitions(huhucapture_quirc PUBLIC QUIRC_MAX_REGIONS=65534)
set_target_properties(huhucapture_quirc PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
add_library(huhucapture_qr_core STATIC src/core/QrRecognition.cpp src/platform/QrText.cpp)
target_link_libraries(huhucapture_qr_core PUBLIC huhucapture_core huhucapture_quirc)
set_target_properties(huhucapture_qr_core PROPERTIES CXX_EXTENSIONS OFF)
if(WIN32)
    target_sources(huhucapture_qr_core PRIVATE src/platform/windows/QrText.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(huhucapture_qr_core PRIVATE src/platform/linux/QrText.cpp)
    target_link_libraries(huhucapture_quirc PUBLIC m)
else()
    message(FATAL_ERROR "The supported QR text adapters are Windows x64 and Linux amd64")
endif()

# Filesystem publication is an OS seam; the archive naming/ownership rules stay
# shared. This target never depends on Qt or the presentation layer.
if(WIN32)
    add_library(huhucapture_file_committer STATIC
        src/platform/windows/FileCommitter.cpp src/platform/windows/FileIdentity.cpp
        src/platform/windows/FileOutput.cpp src/platform/windows/PrivateStorage.cpp)
    target_link_libraries(huhucapture_file_committer PRIVATE ole32 advapi32)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(huhucapture_file_committer STATIC
        src/platform/linux/FileCommitter.cpp src/platform/linux/FileIdentity.cpp
        src/platform/linux/FileOutput.cpp src/platform/linux/PrivateStorage.cpp)
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
        target_compile_options(${name} PRIVATE /W4 /utf-8 /permissive- /UNDEBUG)
    else()
        target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
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
    huhucapture_test(frame_queue tests/frame_queue_tests.cpp)
    target_link_libraries(frame_queue PRIVATE huhucapture_media_frames)
    huhucapture_test(frame_normalizer tests/frame_normalizer_tests.cpp)
    target_link_libraries(frame_normalizer PRIVATE huhucapture_frame_normalizer)
    huhucapture_test(scroll_capture_model tests/scroll_capture_model_tests.cpp)
    target_link_libraries(scroll_capture_model PRIVATE huhucapture_scroll_model)
    huhucapture_test(update_package_policy tests/update_package_policy_tests.cpp)
    huhucapture_test(pixel_buffer tests/PixelBufferTests.cpp)
    huhucapture_test(annotations tests/annotation_tests.cpp)
    huhucapture_test(annotation_regions tests/annotation_region_tests.cpp)
    huhucapture_test(annotation_panel tests/annotation_panel_layout_tests.cpp)
    huhucapture_test(history tests/history_tests.cpp)
    huhucapture_test(scroll_stitch tests/scroll_stitch_tests.cpp)
    huhucapture_test(toolbar_layout tests/toolbar_layout_tests.cpp)
    huhucapture_test(capture_selection tests/capture_selection_tests.cpp)
    huhucapture_test(media_file_names tests/media_file_name_tests.cpp)
    huhucapture_test(release_versions tests/release_version_tests.cpp)
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
    huhucapture_test(recording_time tests/recording_time_tests.cpp)
    huhucapture_test(recording_geometry tests/recording_geometry_tests.cpp)
    huhucapture_test(recording_frame tests/recording_frame_tests.cpp)
    target_sources(recording_frame PRIVATE src/recording/FrameSource.cpp)
    target_link_libraries(recording_frame PRIVATE huhucapture_frame_normalizer)
    huhucapture_test(scroll_accumulator tests/scroll_accumulator_tests.cpp)
    target_link_libraries(scroll_accumulator PRIVATE huhucapture_scroll_model)
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
    # No fallback to a distribution Qt with a different private ABI.
    find_package(Qt6 6.12.1 EXACT REQUIRED COMPONENTS Core Gui Widgets Network Svg)
    add_library(huhucapture_qr_service STATIC src/services/recognition/QrService.cpp)
    target_link_libraries(huhucapture_qr_service PUBLIC huhucapture_qr_core Qt6::Core Qt6::Gui)
    set_target_properties(huhucapture_qr_service PROPERTIES CXX_EXTENSIONS OFF)
    if(BUILD_TESTING)
        huhucapture_test(qt_qr_service tests/qt/qr_service_tests.cpp)
        target_include_directories(qt_qr_service PRIVATE "${PROJECT_SOURCE_DIR}/tests")
        target_link_libraries(qt_qr_service PRIVATE huhucapture_qr_service Threads::Threads)
    endif()
    if(WIN32)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
        # The offscreen Windows plugin supplies a placeholder font engine.
        # Use the native font database without constructing visible windows.
        set(HUHUCAPTURE_TEST_QPA windows)
    else()
        set(HUHUCAPTURE_TEST_QPA offscreen)
    endif()
    add_library(huhucapture_startup STATIC src/app/SingleInstance.h src/app/SingleInstance.cpp)
    set_target_properties(huhucapture_startup PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
    target_link_libraries(huhucapture_startup PUBLIC huhucapture_core Qt6::Core Qt6::Network)
    add_library(huhucapture_launch_command STATIC
        src/services/desktop/LaunchCommand.h src/services/desktop/LaunchCommand.cpp)
    target_link_libraries(huhucapture_launch_command PUBLIC huhucapture_core Qt6::Core)
    set_target_properties(huhucapture_launch_command PROPERTIES CXX_EXTENSIONS OFF)
    if(BUILD_TESTING)
        add_executable(qt_single_instance_probe tests/qt/single_instance_probe.cpp)
        target_link_libraries(qt_single_instance_probe PRIVATE huhucapture_startup)
        set_target_properties(qt_single_instance_probe PROPERTIES CXX_EXTENSIONS OFF)
        add_test(NAME qt_single_instance COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/scripts/test-single-instance.py" $<TARGET_FILE:qt_single_instance_probe>)
        set_tests_properties(qt_single_instance PROPERTIES TIMEOUT 90)
        huhucapture_test(qt_image_contract tests/qt/image_contract_tests.cpp)
        target_link_libraries(qt_image_contract PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Svg)
        set_tests_properties(qt_image_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${HUHUCAPTURE_TEST_QPA}")
    endif()
    add_library(huhucapture_annotation_renderer STATIC src/services/rendering/QtAnnotationRenderer.cpp)
    target_link_libraries(huhucapture_annotation_renderer PUBLIC huhucapture_core Qt6::Gui)
    if(BUILD_TESTING)
        huhucapture_test(qt_annotation_render tests/qt/annotation_render_tests.cpp
            "${PROJECT_BINARY_DIR}/annotation-render/$<CONFIG>")
        target_link_libraries(qt_annotation_render PRIVATE huhucapture_annotation_renderer)
        set_tests_properties(qt_annotation_render PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${HUHUCAPTURE_TEST_QPA}")
    endif()
    add_library(huhucapture_editor STATIC
        src/ui/qt/EditorTypes.h src/ui/qt/EditorTypes.cpp
        src/ui/qt/EditorShape.h src/ui/qt/EditorShape.cpp
        src/ui/qt/EditorCanvas.h src/ui/qt/EditorCanvas.cpp
        src/ui/qt/EditorWindow.h src/ui/qt/EditorWindow.cpp
        resources/qt/huhucapture_ui.qrc)
    set_target_properties(huhucapture_editor PROPERTIES AUTOMOC ON AUTORCC ON CXX_EXTENSIONS OFF)
    target_link_libraries(huhucapture_editor PUBLIC huhucapture_annotation_renderer Qt6::Widgets)
    if(BUILD_TESTING)
        huhucapture_test(qt_editor_components tests/qt/editor_component_tests.cpp
            "${PROJECT_BINARY_DIR}/editor-render/$<CONFIG>")
        target_link_libraries(qt_editor_components PRIVATE huhucapture_editor)
        set_tests_properties(qt_editor_components PROPERTIES TIMEOUT 120
            ENVIRONMENT "QT_QPA_PLATFORM=${HUHUCAPTURE_TEST_QPA}")
    endif()
    add_library(huhucapture_desktop STATIC
        src/ui/qt/DesktopTypes.h src/ui/qt/DesktopTheme.h
        src/ui/qt/HistoryWindow.h src/ui/qt/HistoryWindow.cpp
        src/ui/qt/SettingsWindow.h src/ui/qt/SettingsWindow.cpp
        src/ui/qt/AboutPanel.h src/ui/qt/AboutPanel.cpp)
    set_target_properties(huhucapture_desktop PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
    target_link_libraries(huhucapture_desktop PUBLIC huhucapture_core Qt6::Widgets)
    if(BUILD_TESTING)
        huhucapture_test(qt_desktop_components tests/qt/desktop_component_tests.cpp
            "${PROJECT_BINARY_DIR}/desktop-render/$<CONFIG>")
        target_link_libraries(qt_desktop_components PRIVATE huhucapture_desktop)
        set_tests_properties(qt_desktop_components PROPERTIES TIMEOUT 120
            ENVIRONMENT "QT_QPA_PLATFORM=${HUHUCAPTURE_TEST_QPA}")
    endif()
    if(WIN32)
        if(BUILD_TESTING)
            add_test(NAME qt_annotation_missing_font
                COMMAND qt_annotation_render --expect-font-unavailable)
            set_tests_properties(qt_annotation_missing_font PROPERTIES TIMEOUT 60
                ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_FONTDIR=${PROJECT_BINARY_DIR}/intentionally-empty-fonts")
        endif()
    endif()
    add_library(huhucapture_json STATIC src/services/config/JsonText.cpp)
    target_link_libraries(huhucapture_json PUBLIC huhucapture_core Qt6::Core)
    add_library(huhucapture_settings STATIC src/services/config/SettingsStore.cpp)
    target_link_libraries(huhucapture_settings PUBLIC huhucapture_json
        PRIVATE huhucapture_file_committer)
    if(BUILD_TESTING)
        huhucapture_test(qt_settings_store tests/qt/settings_store_tests.cpp)
        target_link_libraries(qt_settings_store PRIVATE huhucapture_settings)
        huhucapture_test(qt_private_storage tests/qt/private_storage_tests.cpp)
        target_link_libraries(qt_private_storage PRIVATE huhucapture_file_committer Qt6::Core)
        set_tests_properties(qt_private_storage PROPERTIES TIMEOUT 30)
    endif()
    add_library(huhucapture_languages STATIC
        src/services/localization/LanguageCatalog.h
        src/services/localization/LanguageCatalog.cpp
        src/services/localization/QtTranslations.h
        src/services/localization/QtTranslations.cpp
        resources/qt/huhucapture_languages.qrc)
    set_target_properties(huhucapture_languages PROPERTIES AUTOMOC ON AUTORCC ON CXX_EXTENSIONS OFF)
    target_link_libraries(huhucapture_languages PUBLIC huhucapture_json)
    if(BUILD_TESTING)
        huhucapture_test(qt_language_catalog tests/qt/language_catalog_tests.cpp)
        target_link_libraries(qt_language_catalog PRIVATE huhucapture_languages)
    endif()
    add_library(huhucapture_media_archive STATIC src/services/media/MediaArchiveService.cpp)
    target_link_libraries(huhucapture_media_archive PUBLIC huhucapture_core Qt6::Gui
        PRIVATE huhucapture_file_committer Threads::Threads)
    set_target_properties(huhucapture_media_archive PROPERTIES CXX_EXTENSIONS OFF)
    if(BUILD_TESTING)
        huhucapture_test(qt_media_archive tests/qt/media_archive_tests.cpp)
        target_link_libraries(qt_media_archive PRIVATE huhucapture_media_archive Threads::Threads)
        set_tests_properties(qt_media_archive PROPERTIES TIMEOUT 120)
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        if(BUILD_TESTING)
            target_compile_definitions(qt_media_archive PRIVATE HUHUCAPTURE_TEST_WRAP_FSYNC)
            target_link_options(qt_media_archive PRIVATE -Wl,--wrap=fsync)
        endif()
    endif()
    if(HUHUCAPTURE_DESKTOP_VALIDATION)
        find_package(Qt6 6.12.1 EXACT REQUIRED COMPONENTS Concurrent)
        add_library(huhucapture_desktop_services STATIC
            src/services/desktop/DesktopResult.h
            src/services/desktop/DesktopCaptureAction.h src/services/desktop/DesktopCaptureAction.cpp
            src/services/desktop/CaptureExclusion.h src/services/desktop/CaptureExclusion.cpp
            src/services/desktop/SystemClipboard.h src/services/desktop/SystemClipboard.cpp
            src/services/desktop/DesktopFiles.h src/services/desktop/DesktopFiles.cpp
            src/services/desktop/FileRevealer.h src/services/desktop/FileRevealer.cpp)
        set_target_properties(huhucapture_desktop_services PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
        target_link_libraries(huhucapture_desktop_services PUBLIC huhucapture_media_archive Qt6::Gui
            PRIVATE huhucapture_launch_command Qt6::Concurrent)
        if(WIN32)
            target_sources(huhucapture_desktop_services PRIVATE
                src/platform/windows/DesktopClipboard.h src/platform/windows/DesktopClipboard.cpp
                src/platform/windows/DesktopFiles.h src/platform/windows/DesktopFiles.cpp)
            target_link_libraries(huhucapture_desktop_services PRIVATE
                user32 shell32 ole32 uuid windowscodecs)
            target_compile_definitions(huhucapture_desktop_services PRIVATE
                UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
        elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            find_package(Qt6 6.12.1 EXACT REQUIRED COMPONENTS WaylandClient DBus)
            find_package(PkgConfig REQUIRED)
            pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client>=1.22)
            pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-unix-2.0>=2.80)
            target_sources(huhucapture_desktop_services PRIVATE
                src/platform/linux/DesktopCaptureAction.h src/platform/linux/DesktopCaptureAction.cpp
                src/platform/linux/DesktopClipboard.h src/platform/linux/DesktopClipboard.cpp
                src/platform/linux/DesktopFiles.h src/platform/linux/DesktopFiles.cpp)
            target_link_libraries(huhucapture_desktop_services PRIVATE
                Qt6::WaylandClient Qt6::DBus PkgConfig::WAYLAND_CLIENT PkgConfig::GIO)
        else()
            message(FATAL_ERROR "Desktop integration targets Windows x64 and Linux amd64")
        endif()
        add_library(huhucapture_media_workspace STATIC
            src/services/media/ArchiveExport.h src/services/media/ArchiveExport.cpp
            src/services/media/ThumbnailCache.h src/services/media/ThumbnailCache.cpp
            src/app/EditorPreferences.h src/app/EditorPreferences.cpp
            src/app/MediaWorkspace.h src/app/MediaWorkspace.cpp)
        set_target_properties(huhucapture_media_workspace PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
        target_link_libraries(huhucapture_media_workspace PUBLIC huhucapture_desktop_services
            huhucapture_desktop huhucapture_editor huhucapture_settings Qt6::Concurrent)
        if(BUILD_TESTING)
            huhucapture_test(qt_media_workspace tests/qt/media_workspace_tests.cpp)
            target_link_libraries(qt_media_workspace PRIVATE huhucapture_media_workspace huhucapture_languages)
            set_tests_properties(qt_media_workspace PROPERTIES TIMEOUT 180
                ENVIRONMENT "QT_QPA_PLATFORM=${HUHUCAPTURE_TEST_QPA}")
            huhucapture_test(desktop_files_tests tests/qt/desktop_files_tests.cpp)
            target_link_libraries(desktop_files_tests PRIVATE huhucapture_desktop_services)
            huhucapture_test(file_revealer_tests tests/qt/file_revealer_tests.cpp)
            target_link_libraries(file_revealer_tests PRIVATE huhucapture_desktop_services Qt6::Widgets)
            set_tests_properties(file_revealer_tests PROPERTIES TIMEOUT 60
                ENVIRONMENT "QT_QPA_PLATFORM=${HUHUCAPTURE_TEST_QPA}")
            add_executable(desktop_clipboard_tests tests/qt/desktop_clipboard_tests.cpp)
            target_link_libraries(desktop_clipboard_tests PRIVATE huhucapture_desktop_services Qt6::Widgets)
            if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
                target_link_libraries(desktop_clipboard_tests PRIVATE Qt6::DBus)
            endif()
            set_target_properties(desktop_clipboard_tests PROPERTIES CXX_EXTENSIONS OFF)
        endif()
        if(WIN32)
            if(BUILD_TESTING)
                add_test(NAME desktop_clipboard_windows COMMAND desktop_clipboard_tests)
                set_tests_properties(desktop_clipboard_windows PROPERTIES TIMEOUT 120
                    ENVIRONMENT "QT_QPA_PLATFORM=windows")
                add_test(NAME file_revealer_windows_desktop COMMAND file_revealer_tests --real-desktop)
                set_tests_properties(file_revealer_windows_desktop PROPERTIES TIMEOUT 60
                    ENVIRONMENT "QT_QPA_PLATFORM=windows")
            endif()
        else()
            if(BUILD_TESTING)
                add_test(NAME desktop_clipboard_no_backend COMMAND desktop_clipboard_tests --reject-backend)
                set_tests_properties(desktop_clipboard_no_backend PROPERTIES TIMEOUT 60
                    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
                if(HUHUCAPTURE_X11_TESTS)
                    find_program(XVFB_RUN xvfb-run REQUIRED)
                    add_test(NAME desktop_clipboard_x11 COMMAND "${XVFB_RUN}" -a
                        "${CMAKE_COMMAND}" -E env "QT_QPA_PLATFORM=xcb" $<TARGET_FILE:desktop_clipboard_tests>)
                    set_tests_properties(desktop_clipboard_x11 PROPERTIES TIMEOUT 120)
                else()
                    message(STATUS "Real X11 tests explicitly disabled; no desktop clipboard acceptance is claimed")
                endif()
            endif()
        endif()
    endif()
    if(HUHUCAPTURE_RUST_VALIDATION)
        set(HUHUCAPTURE_RUST_METADATA "${PROJECT_SOURCE_DIR}/artifacts/native-rust/build-info.json"
            CACHE FILEPATH "Actual locked Rust static library and native link metadata")
        file(READ "${HUHUCAPTURE_RUST_METADATA}" native_metadata)
        string(JSON native_schema GET "${native_metadata}" schemaVersion)
        string(JSON native_version GET "${native_metadata}" rustVersion)
        string(JSON native_host GET "${native_metadata}" host)
        string(JSON native_library GET "${native_metadata}" library)
        string(JSON native_library_hash GET "${native_metadata}" librarySha256)
        string(JSON native_lock_hash GET "${native_metadata}" cargoLockSha256)
        if(NOT native_schema EQUAL 1 OR NOT native_version STREQUAL "1.97.1" OR NOT EXISTS "${native_library}")
            message(FATAL_ERROR "Invalid or missing pinned Rust aggregate metadata")
        endif()
        if(WIN32)
            string(JSON native_crt GET "${native_metadata}" windowsCrt)
            if(NOT native_host STREQUAL "x86_64-pc-windows-msvc" OR NOT native_crt STREQUAL "MD")
                message(FATAL_ERROR "The Rust library must use Windows x64 and the matching dynamic release CRT")
            endif()
        elseif(NOT native_host STREQUAL "x86_64-unknown-linux-gnu")
            message(FATAL_ERROR "The Rust library must use the Linux amd64 toolchain")
        endif()
        file(SHA256 "${native_library}" actual_native_hash)
        file(SHA256 "${PROJECT_SOURCE_DIR}/third_party/localsend/Cargo.lock" actual_lock_hash)
        if(NOT actual_native_hash STREQUAL native_library_hash OR NOT actual_lock_hash STREQUAL native_lock_hash)
            message(FATAL_ERROR "Native library or Cargo.lock changed after its build")
        endif()
        string(JSON native_source_count LENGTH "${native_metadata}" sourceFiles)
        if(native_source_count LESS 1)
            message(FATAL_ERROR "Missing actual Rust source fingerprints")
        endif()
        math(EXPR native_source_last "${native_source_count} - 1")
        foreach(index RANGE ${native_source_last})
            string(JSON native_source MEMBER "${native_metadata}" sourceFiles ${index})
            if(NOT native_source MATCHES "^(src/platform/rust/|third_party/localsend/|third_party/gifski/|scripts/build-native-rust\\.py$)"
                OR native_source MATCHES "(^|/)\\.\\.(/|$)" OR NOT EXISTS "${PROJECT_SOURCE_DIR}/${native_source}")
                message(FATAL_ERROR "Invalid native source fingerprint path")
            endif()
            string(JSON native_source_expected GET "${native_metadata}" sourceFiles "${native_source}")
            file(SHA256 "${PROJECT_SOURCE_DIR}/${native_source}" native_source_actual)
            if(NOT native_source_actual STREQUAL native_source_expected)
                message(FATAL_ERROR "Native sources changed after the aggregate build: ${native_source}")
            endif()
        endforeach()
        string(JSON native_count LENGTH "${native_metadata}" nativeLibraries)
        if(native_count LESS 1)
            message(FATAL_ERROR "Missing actual Rust native link dependencies")
        endif()
        math(EXPR native_last "${native_count} - 1")
        set(native_system_libraries)
        foreach(index RANGE ${native_last})
            string(JSON native_system_library GET "${native_metadata}" nativeLibraries ${index})
            list(APPEND native_system_libraries "${native_system_library}")
        endforeach()
        string(JSON native_option_count LENGTH "${native_metadata}" nativeLinkOptions)
        set(native_link_options)
        if(native_option_count GREATER 0)
            math(EXPR native_option_last "${native_option_count} - 1")
            foreach(index RANGE ${native_option_last})
                string(JSON native_option GET "${native_metadata}" nativeLinkOptions ${index})
                string(TOLOWER "${native_option}" native_option_lower)
                if(NOT WIN32 OR NOT native_option_lower STREQUAL "/defaultlib:msvcrt")
                    message(FATAL_ERROR "Unexpected Rust native linker option")
                endif()
                list(APPEND native_link_options "${native_option}")
            endforeach()
        endif()
        add_library(huhucapture_rust STATIC IMPORTED)
        string(JSON native_file_count LENGTH "${native_metadata}" nativeLibraryFiles)
        if(native_file_count GREATER 0)
            math(EXPR native_file_last "${native_file_count} - 1")
            foreach(index RANGE ${native_file_last})
                string(JSON native_file GET "${native_metadata}" nativeLibraryFiles ${index} path)
                string(JSON native_file_expected GET "${native_metadata}" nativeLibraryFiles ${index} sha256)
                if(NOT EXISTS "${native_file}")
                    message(FATAL_ERROR "A locked native dependency import library is missing")
                endif()
                file(SHA256 "${native_file}" native_file_actual)
                if(NOT native_file_actual STREQUAL native_file_expected)
                    message(FATAL_ERROR "A locked native dependency import library changed")
                endif()
            endforeach()
        endif()
        set_target_properties(huhucapture_rust PROPERTIES IMPORTED_LOCATION "${native_library}"
            INTERFACE_LINK_LIBRARIES "${native_system_libraries}"
            INTERFACE_LINK_OPTIONS "${native_link_options}"
            INTERFACE_INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/src/platform/rust/include;${PROJECT_SOURCE_DIR}/third_party/gifski")
        add_library(huhucapture_gif_inspection STATIC src/services/media/GifInspection.cpp)
        target_link_libraries(huhucapture_gif_inspection PUBLIC huhucapture_media_archive huhucapture_rust)
        add_library(huhucapture_transfer STATIC
            src/services/transfer/ReceiveSink.h
            src/services/transfer/TransferService.h
            src/services/transfer/TransferService.cpp)
        set_target_properties(huhucapture_transfer PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
        target_link_libraries(huhucapture_transfer PUBLIC huhucapture_core Qt6::Core
            PRIVATE huhucapture_rust Threads::Threads)
        add_library(huhucapture_receive_archive STATIC
            src/services/transfer/ReceiveArchiveSink.h
            src/services/transfer/ReceiveArchiveSink.cpp)
        target_link_libraries(huhucapture_receive_archive PUBLIC huhucapture_media_archive
            huhucapture_transfer PRIVATE huhucapture_rust Threads::Threads)
        set_target_properties(huhucapture_receive_archive PROPERTIES CXX_EXTENSIONS OFF)
        add_library(huhucapture_transfer_ui STATIC
            src/ui/qt/TransferPresentation.h
            src/ui/qt/TransferWindow.h src/ui/qt/TransferWindow.cpp
            src/ui/qt/ReceivePrompt.h src/ui/qt/ReceivePrompt.cpp
            src/ui/qt/TransferSettingsPanel.h src/ui/qt/TransferSettingsPanel.cpp)
        set_target_properties(huhucapture_transfer_ui PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
        target_link_libraries(huhucapture_transfer_ui PUBLIC huhucapture_transfer
            huhucapture_desktop Qt6::Widgets Qt6::Network)
        if(BUILD_TESTING)
            huhucapture_test(qt_transfer_ui tests/qt/transfer_ui_tests.cpp
                "${PROJECT_BINARY_DIR}/transfer-render/$<CONFIG>")
            target_link_libraries(qt_transfer_ui PRIVATE huhucapture_transfer_ui
                huhucapture_receive_archive huhucapture_languages)
            set_tests_properties(qt_transfer_ui PROPERTIES TIMEOUT 240
                ENVIRONMENT "QT_QPA_PLATFORM=${HUHUCAPTURE_TEST_QPA}")
        endif()
        find_package(Qt6 6.12.1 EXACT REQUIRED COMPONENTS Concurrent)
        add_library(huhucapture_lan_controller STATIC
            src/app/LanController.h src/app/LanController.cpp
            src/services/transfer/IdentityStore.h src/services/transfer/IdentityStore.cpp)
        set_target_properties(huhucapture_lan_controller PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
        target_link_libraries(huhucapture_lan_controller PUBLIC
            huhucapture_transfer_ui huhucapture_receive_archive huhucapture_settings
            huhucapture_file_committer Qt6::Concurrent Qt6::Network)
        if(BUILD_TESTING)
            huhucapture_test(qt_lan_controller tests/qt/lan_controller_tests.cpp
                "${PROJECT_BINARY_DIR}/lan-controller-render/$<CONFIG>")
            target_link_libraries(qt_lan_controller PRIVATE huhucapture_lan_controller huhucapture_languages)
            set_tests_properties(qt_lan_controller PROPERTIES TIMEOUT 300
                ENVIRONMENT "QT_QPA_PLATFORM=${HUHUCAPTURE_TEST_QPA}")
            add_test(NAME qt_identity_store COMMAND qt_lan_controller --identity-only)
            set_tests_properties(qt_identity_store PROPERTIES TIMEOUT 60
                ENVIRONMENT "QT_QPA_PLATFORM=${HUHUCAPTURE_TEST_QPA}")
            huhucapture_test(qt_rust_bridge tests/qt/rust_bridge_probe.cpp
                "${PROJECT_BINARY_DIR}/rust-bridge/$<CONFIG>")
            target_link_libraries(qt_rust_bridge PRIVATE huhucapture_gif_inspection)
            huhucapture_test(qt_transfer_service tests/qt/transfer_service_tests.cpp)
            target_link_libraries(qt_transfer_service PRIVATE huhucapture_receive_archive
                huhucapture_gif_inspection Qt6::Network)
            set_tests_properties(qt_transfer_service PROPERTIES TIMEOUT 240
                ENVIRONMENT "QT_QPA_PLATFORM=${HUHUCAPTURE_TEST_QPA}")
        endif()
        if(MSVC)
            if(BUILD_TESTING)
                target_link_options(qt_rust_bridge PRIVATE /WX)
                target_link_options(qt_transfer_service PRIVATE /WX)
                target_link_options(qt_transfer_ui PRIVATE /WX)
                target_link_options(qt_lan_controller PRIVATE /WX)
            endif()
        endif()
        if(BUILD_TESTING)
            set_tests_properties(qt_rust_bridge PROPERTIES TIMEOUT 180
                ENVIRONMENT "QT_QPA_PLATFORM=${HUHUCAPTURE_TEST_QPA}")
        endif()
    endif()
    if(WIN32)
        if(BUILD_TESTING)
            huhucapture_test(windows_audio_format tests/platform/windows_audio_format_tests.cpp)
        endif()
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
        if(BUILD_TESTING)
            target_link_libraries(qt_image_contract PRIVATE huhucapture_native_media)
        endif()
    endif()
    if(HUHUCAPTURE_PORTAL_VALIDATION)
        if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
            message(FATAL_ERROR "Portal validation is only supported on Linux")
        endif()
        find_package(PkgConfig REQUIRED)
        enable_language(C)
        find_package(Qt6 6.12.1 EXACT REQUIRED COMPONENTS WaylandClient GuiPrivate)
        pkg_check_modules(LIBPORTAL REQUIRED IMPORTED_TARGET libportal=0.11.0)
        pkg_check_modules(LIBPORTAL_QT REQUIRED IMPORTED_TARGET libportal-qt6=0.11.0)
        pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-unix-2.0>=2.80)
        pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client>=1.22)
        if(NOT TARGET Wayland::Scanner)
            find_program(WAYLAND_SCANNER_EXECUTABLE wayland-scanner REQUIRED)
            add_executable(Wayland::Scanner IMPORTED)
            set_target_properties(Wayland::Scanner PROPERTIES IMPORTED_LOCATION "${WAYLAND_SCANNER_EXECUTABLE}")
        endif()
        if(BUILD_TESTING)
            find_program(DBUS_RUN_SESSION dbus-run-session REQUIRED)
        endif()
        add_library(huhucapture_portal STATIC
            src/platform/linux/portal/PortalTypes.h
            src/platform/linux/portal/PortalSupport.h
            src/platform/linux/portal/PortalParent.h
            src/platform/linux/portal/PortalParent.cpp
            src/platform/linux/portal/PortalCapture.h
            src/platform/linux/portal/PortalCapture.cpp
            src/platform/linux/portal/PortalShortcuts.h
            src/platform/linux/portal/PortalShortcuts.cpp
            src/platform/linux/portal/WaylandRequestBarrier.h
            src/platform/linux/portal/WaylandRequestBarrier.cpp
            third_party/kwayland-parent/WaylandForeign.h
            third_party/kwayland-parent/WaylandForeign.cpp)
        set_target_properties(huhucapture_portal PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
        target_compile_definitions(huhucapture_portal PRIVATE QT_NO_KEYWORDS)
        target_link_libraries(huhucapture_portal PUBLIC huhucapture_core Qt6::Core Qt6::Gui
            PRIVATE Qt6::Widgets Qt6::GuiPrivate Qt6::WaylandClient PkgConfig::WAYLAND_CLIENT
                PkgConfig::LIBPORTAL PkgConfig::LIBPORTAL_QT PkgConfig::GIO)
        target_include_directories(huhucapture_portal PRIVATE "${PROJECT_SOURCE_DIR}/third_party/kwayland-parent")
        qt6_generate_wayland_protocol_client_sources(huhucapture_portal PRIVATE_CODE
            FILES "${PROJECT_SOURCE_DIR}/third_party/wayland-protocols/xdg-foreign-unstable-v2.xml")
        target_compile_options(huhucapture_portal PRIVATE -Wall -Wextra -Wpedantic)
        if(BUILD_TESTING)
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
            add_executable(portal_parent_probe tests/platform/portal_parent_probe.cpp)
            target_compile_definitions(portal_parent_probe PRIVATE QT_NO_KEYWORDS)
            target_link_libraries(portal_parent_probe PRIVATE huhucapture_portal Qt6::Widgets
                PkgConfig::WAYLAND_CLIENT PkgConfig::LIBPORTAL PkgConfig::GIO)
        endif()
        # This executable requires a real desktop and user authorization. It is
        # deliberately not reported as a passing offscreen CTest capture test.
    endif()
    if(HUHUCAPTURE_PIPEWIRE_VALIDATION)
        pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3=1.6.11)
        add_library(huhucapture_pipewire STATIC
            src/platform/linux/pipewire/PipeWireCapture.cpp
            src/platform/linux/pipewire/PipeWireMemFdMappings.cpp)
        target_link_libraries(huhucapture_pipewire PUBLIC huhucapture_media_frames
            PRIVATE PkgConfig::PIPEWIRE Threads::Threads)
        set_target_properties(huhucapture_pipewire PROPERTIES CXX_EXTENSIONS OFF)
        target_compile_options(huhucapture_pipewire PRIVATE -Wall -Wextra -Wpedantic)
        if(BUILD_TESTING)
            huhucapture_test(pipewire_fd_tests tests/platform/pipewire_fd_tests.cpp)
            target_link_libraries(pipewire_fd_tests PRIVATE huhucapture_pipewire PkgConfig::PIPEWIRE)
            huhucapture_test(pipewire_memfd_tests tests/platform/pipewire_memfd_tests.cpp)
            target_link_libraries(pipewire_memfd_tests PRIVATE huhucapture_pipewire PkgConfig::PIPEWIRE)
            set_tests_properties(pipewire_memfd_tests PROPERTIES TIMEOUT 20)
            add_executable(pipewire_capture_probe tests/platform/pipewire_capture_probe.cpp)
            target_compile_definitions(pipewire_capture_probe PRIVATE QT_NO_KEYWORDS)
            target_link_libraries(pipewire_capture_probe PRIVATE huhucapture_pipewire
                huhucapture_portal Qt6::Widgets PkgConfig::PIPEWIRE)
        endif()
        # Actual Portal/PipeWire frames require an owned compositor and daemon;
        # the isolated wrapper in CI produces the corresponding evidence.
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
        if(BUILD_TESTING)
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
    if(HUHUCAPTURE_VIDEO_VALIDATION)
        set(HUHUCAPTURE_VIDEO_FIXTURES "${PROJECT_SOURCE_DIR}/artifacts/video-fixtures"
            CACHE PATH "Identical real encoded MP4 fixtures generated by the pinned Linux encoder")
        add_library(huhucapture_video_inspection STATIC src/services/media/Mp4Envelope.cpp
            src/services/media/FfmpegVideoInspection.h src/services/media/FfmpegVideoInspection.cpp
            src/services/media/FfmpegVideoThumbnail.cpp)
        target_link_libraries(huhucapture_video_inspection PUBLIC huhucapture_media_archive)
        set_target_properties(huhucapture_video_inspection PROPERTIES CXX_EXTENSIONS OFF)
        if(WIN32)
            include(build/cmake/windows-video-inspection.cmake)
            target_sources(huhucapture_video_inspection PRIVATE src/platform/windows/VideoInspection.cpp)
            target_link_libraries(huhucapture_video_inspection PRIVATE
                huhucapture_media_frames huhucapture_video_ffmpeg mfplat mfreadwrite mfuuid shlwapi ole32)
            target_compile_options(huhucapture_video_inspection PRIVATE /W4 /utf-8 /permissive-)
        elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            target_sources(huhucapture_video_inspection PRIVATE src/platform/linux/VideoInspection.cpp)
            target_link_libraries(huhucapture_video_inspection PRIVATE PkgConfig::FFMPEG)
            target_compile_options(huhucapture_video_inspection PRIVATE -Wall -Wextra -Wpedantic)
            if(BUILD_TESTING)
                add_executable(video_inspection_fixtures tests/qt/video_inspection_fixtures.cpp)
                target_link_libraries(video_inspection_fixtures PRIVATE huhucapture_core Qt6::Core
                    PkgConfig::FFMPEG ffmpeg_mux_example m)
                set_target_properties(video_inspection_fixtures PROPERTIES CXX_EXTENSIONS OFF)
            endif()
        else()
            message(FATAL_ERROR "Video inspection validation targets Windows x64 and Linux amd64")
        endif()
        if(BUILD_TESTING)
            huhucapture_test(qt_video_inspection tests/qt/video_inspection_tests.cpp
                "${HUHUCAPTURE_VIDEO_FIXTURES}" "${PROJECT_BINARY_DIR}/video-inspection/$<CONFIG>")
            target_link_libraries(qt_video_inspection PRIVATE huhucapture_video_inspection Threads::Threads)
            add_test(NAME qt_hevc_inspection COMMAND qt_video_inspection
                "${HUHUCAPTURE_VIDEO_FIXTURES}" "${PROJECT_BINARY_DIR}/hevc-inspection/$<CONFIG>" --hevc)
            set_tests_properties(qt_video_inspection qt_hevc_inspection PROPERTIES TIMEOUT 240)
        endif()
        if(WIN32)
            if(BUILD_TESTING)
                set_tests_properties(qt_video_inspection qt_hevc_inspection PROPERTIES ENVIRONMENT_MODIFICATION
                    "PATH=path_list_prepend:$<TARGET_FILE_DIR:huhucapture_ffmpeg_avformat>")
            endif()
        endif()
    endif()
endif()

if(HUHUCAPTURE_QT_VALIDATION)
    include(build/cmake/qt-application.cmake)
    include(build/cmake/tests.cmake)
endif()
