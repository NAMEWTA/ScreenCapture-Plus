cmake_minimum_required(VERSION 3.25)
project(huhucapture LANGUAGES CXX)

# During P00/P03 this builds the shared contracts and migration validation only.
# The shipping Windows application remains on its verified build until P12.
# Do not mistake a passing core-only configuration for a Qt product build.
include(CTest)
option(HUHUCAPTURE_QT_VALIDATION "Build the pinned Qt migration validation targets" OFF)
option(HUHUCAPTURE_PORTAL_VALIDATION "Build the real Linux Portal control-plane validation" OFF)
option(HUHUCAPTURE_FFMPEG_VALIDATION "Build the pinned software-codec API validation" OFF)
option(HUHUCAPTURE_PIPEWIRE_VALIDATION "Build the fixed Linux PipeWire data-plane validation" OFF)
option(HUHUCAPTURE_RUST_VALIDATION "Link one Rust aggregate into the real Qt ABI probe" OFF)
if(HUHUCAPTURE_PORTAL_VALIDATION AND NOT HUHUCAPTURE_QT_VALIDATION)
    message(FATAL_ERROR "Portal validation requires HUHUCAPTURE_QT_VALIDATION=ON")
endif()
if(HUHUCAPTURE_PIPEWIRE_VALIDATION AND NOT HUHUCAPTURE_PORTAL_VALIDATION)
    message(FATAL_ERROR "PipeWire validation requires HUHUCAPTURE_PORTAL_VALIDATION=ON")
endif()
if(HUHUCAPTURE_FFMPEG_VALIDATION AND NOT HUHUCAPTURE_QT_VALIDATION)
    message(FATAL_ERROR "Software-codec validation requires HUHUCAPTURE_QT_VALIDATION=ON")
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
target_include_directories(huhucapture_core INTERFACE "${PROJECT_SOURCE_DIR}/src")
target_compile_features(huhucapture_core INTERFACE cxx_std_20)
if(MSVC)
    target_compile_options(huhucapture_core INTERFACE /utf-8 /permissive-)
endif()

find_package(Threads REQUIRED)
add_library(huhucapture_media_frames STATIC src/core/MediaFrame.cpp)
target_link_libraries(huhucapture_media_frames PUBLIC huhucapture_core Threads::Threads)
set_target_properties(huhucapture_media_frames PROPERTIES CXX_EXTENSIONS OFF)

# Filesystem publication is an OS seam; the archive naming/ownership rules stay
# shared. This target never depends on Qt or the presentation layer.
if(WIN32)
    add_library(huhucapture_file_committer STATIC
        src/platform/windows/FileCommitter.cpp src/platform/windows/FileIdentity.cpp)
    target_link_libraries(huhucapture_file_committer PRIVATE ole32)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(huhucapture_file_committer STATIC
        src/platform/linux/FileCommitter.cpp src/platform/linux/FileIdentity.cpp)
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
    huhucapture_test(media_frame_tests tests/media_frame_tests.cpp)
    target_link_libraries(media_frame_tests PRIVATE huhucapture_media_frames)
    huhucapture_test(pixel_buffer tests/PixelBufferTests.cpp)
    huhucapture_test(annotations tests/annotation_tests.cpp)
    huhucapture_test(annotation_regions tests/annotation_region_tests.cpp)
    huhucapture_test(annotation_panel tests/annotation_panel_layout_tests.cpp)
    huhucapture_test(history tests/history_tests.cpp)
    huhucapture_test(scroll_stitch tests/scroll_stitch_tests.cpp)
    huhucapture_test(toolbar_layout tests/toolbar_layout_tests.cpp)
    huhucapture_test(media_file_names tests/media_file_name_tests.cpp)
    huhucapture_test(release_versions tests/release_version_tests.cpp)
    huhucapture_test(shortcut_keys tests/shortcut_keys_tests.cpp)
    huhucapture_test(annotation_model tests/annotation_model_tests.cpp)
    huhucapture_test(box_model tests/box_model_tests.cpp)
    huhucapture_test(stroke_model tests/stroke_model_tests.cpp)
    huhucapture_test(arrow_model tests/arrow_model_tests.cpp)
    huhucapture_test(number_model tests/number_model_tests.cpp)
    huhucapture_test(text_model tests/text_model_tests.cpp)
    huhucapture_test(editor_document tests/editor_document_tests.cpp)
    huhucapture_test(mosaic_pixels tests/mosaic_pixels_tests.cpp)
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
    add_library(huhucapture_editor STATIC
        src/ui/qt/EditorTypes.h src/ui/qt/EditorTypes.cpp
        src/ui/qt/EditorShape.h src/ui/qt/EditorShape.cpp
        src/ui/qt/EditorCanvas.h src/ui/qt/EditorCanvas.cpp
        src/ui/qt/EditorWindow.h src/ui/qt/EditorWindow.cpp
        resources/qt/huhucapture_ui.qrc)
    set_target_properties(huhucapture_editor PROPERTIES AUTOMOC ON AUTORCC ON CXX_EXTENSIONS OFF)
    target_link_libraries(huhucapture_editor PUBLIC huhucapture_annotation_renderer Qt6::Widgets)
    huhucapture_test(qt_editor_components tests/qt/editor_component_tests.cpp
        "${PROJECT_BINARY_DIR}/editor-render/$<CONFIG>")
    target_link_libraries(qt_editor_components PRIVATE huhucapture_editor)
    set_tests_properties(qt_editor_components PROPERTIES TIMEOUT 120
        ENVIRONMENT "QT_QPA_PLATFORM=${HUHUCAPTURE_TEST_QPA}")
    add_library(huhucapture_desktop STATIC
        src/ui/qt/DesktopTypes.h src/ui/qt/DesktopTheme.h
        src/ui/qt/HistoryWindow.h src/ui/qt/HistoryWindow.cpp
        src/ui/qt/SettingsWindow.h src/ui/qt/SettingsWindow.cpp
        src/ui/qt/AboutPanel.h src/ui/qt/AboutPanel.cpp)
    set_target_properties(huhucapture_desktop PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
    target_link_libraries(huhucapture_desktop PUBLIC huhucapture_core Qt6::Widgets)
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
    add_library(huhucapture_media_archive STATIC src/services/media/MediaArchiveService.cpp)
    target_link_libraries(huhucapture_media_archive PUBLIC huhucapture_core Qt6::Gui
        PRIVATE huhucapture_file_committer Threads::Threads)
    set_target_properties(huhucapture_media_archive PROPERTIES CXX_EXTENSIONS OFF)
    huhucapture_test(qt_media_archive tests/qt/media_archive_tests.cpp)
    target_link_libraries(qt_media_archive PRIVATE huhucapture_media_archive Threads::Threads)
    set_tests_properties(qt_media_archive PROPERTIES TIMEOUT 120)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_compile_definitions(qt_media_archive PRIVATE HUHUCAPTURE_TEST_WRAP_FSYNC)
        target_link_options(qt_media_archive PRIVATE -Wl,--wrap=fsync)
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
        endif()
        set_tests_properties(qt_rust_bridge PROPERTIES TIMEOUT 180
            ENVIRONMENT "QT_QPA_PLATFORM=${HUHUCAPTURE_TEST_QPA}")
    endif()
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
        find_program(DBUS_RUN_SESSION dbus-run-session REQUIRED)
        add_library(huhucapture_portal STATIC
            src/platform/linux/portal/PortalTypes.h
            src/platform/linux/portal/PortalSupport.h
            src/platform/linux/portal/PortalParent.h
            src/platform/linux/portal/PortalParent.cpp
            src/platform/linux/portal/PortalCapture.h
            src/platform/linux/portal/PortalCapture.cpp
            src/platform/linux/portal/PortalShortcuts.h
            src/platform/linux/portal/PortalShortcuts.cpp
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
        # This executable requires a real desktop and user authorization. It is
        # deliberately not reported as a passing offscreen CTest capture test.
    endif()
    if(HUHUCAPTURE_PIPEWIRE_VALIDATION)
        pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3=1.6.11)
        add_library(huhucapture_pipewire STATIC src/platform/linux/pipewire/PipeWireCapture.cpp)
        target_link_libraries(huhucapture_pipewire PUBLIC huhucapture_media_frames
            PRIVATE PkgConfig::PIPEWIRE Threads::Threads)
        set_target_properties(huhucapture_pipewire PROPERTIES CXX_EXTENSIONS OFF)
        target_compile_options(huhucapture_pipewire PRIVATE -Wall -Wextra -Wpedantic)
        huhucapture_test(pipewire_fd_tests tests/platform/pipewire_fd_tests.cpp)
        target_link_libraries(pipewire_fd_tests PRIVATE huhucapture_pipewire PkgConfig::PIPEWIRE)
        add_executable(pipewire_capture_probe tests/platform/pipewire_capture_probe.cpp)
        target_compile_definitions(pipewire_capture_probe PRIVATE QT_NO_KEYWORDS)
        target_link_libraries(pipewire_capture_probe PRIVATE huhucapture_pipewire
            huhucapture_portal Qt6::Widgets PkgConfig::PIPEWIRE)
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
