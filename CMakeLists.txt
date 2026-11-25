cmake_minimum_required(VERSION 3.25)
include(build/cmake/version.cmake)
project(huhucapture VERSION "${HUHUCAPTURE_VERSION}" LANGUAGES C CXX)

configure_file(src/app/Version.h.in generated/app/Version.h @ONLY)
add_library(huhucapture_version INTERFACE)
target_include_directories(huhucapture_version INTERFACE "${PROJECT_BINARY_DIR}/generated")
target_compile_features(huhucapture_version INTERFACE cxx_std_20)

# Shared contracts can be validated without Qt. The complete application host
# is selected explicitly below; its native/package acceptance remains separate.
# The shipping Windows application stays on its verified build until P12.
include(CTest)
option(HUHUCAPTURE_QT_VALIDATION "Build the pinned Qt migration validation targets" OFF)
option(HUHUCAPTURE_PORTAL_VALIDATION "Build the real Linux Portal control-plane validation" OFF)
option(HUHUCAPTURE_FFMPEG_VALIDATION "Build the pinned software-codec API validation" OFF)
option(HUHUCAPTURE_VIDEO_VALIDATION "Inspect the same real complete and malformed MP4 files on both platforms" OFF)
option(HUHUCAPTURE_DESKTOP_VALIDATION "Build real system clipboard, trash and file-manager service validation" OFF)
option(HUHUCAPTURE_PIPEWIRE_VALIDATION "Build the fixed Linux PipeWire data-plane validation" OFF)
option(HUHUCAPTURE_RUST_VALIDATION "Link one Rust aggregate into the real Qt ABI probe" OFF)
option(HUHUCAPTURE_NATIVE_INPUT_VALIDATION "Build the real OS shortcut/input validation (requires XTEST development files on Linux)" ON)
option(HUHUCAPTURE_BUILD_APP "Build the unified Qt desktop application" OFF)
if(HUHUCAPTURE_BUILD_APP)
    foreach(required IN ITEMS QT DESKTOP RUST VIDEO)
        set(HUHUCAPTURE_${required}_VALIDATION ON)
    endforeach()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        foreach(required IN ITEMS PORTAL PIPEWIRE FFMPEG)
            set(HUHUCAPTURE_${required}_VALIDATION ON)
        endforeach()
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

if(HUHUCAPTURE_QT_VALIDATION)
    # No fallback to a distribution Qt with a different private ABI.
    find_package(Qt6 6.12.1 EXACT REQUIRED COMPONENTS Core Gui Widgets Network Svg Concurrent)
    add_library(huhucapture_qr_service STATIC src/services/recognition/QrService.cpp)
    target_link_libraries(huhucapture_qr_service PUBLIC huhucapture_qr_core Qt6::Core Qt6::Gui)
    set_target_properties(huhucapture_qr_service PROPERTIES CXX_EXTENSIONS OFF)
    if(WIN32)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif()
    add_library(huhucapture_startup STATIC src/app/SingleInstance.h src/app/SingleInstance.cpp)
    set_target_properties(huhucapture_startup PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
    target_link_libraries(huhucapture_startup PUBLIC huhucapture_core Qt6::Core Qt6::Network)
    add_library(huhucapture_capture STATIC src/capture/CaptureTypes.h
        src/capture/CaptureSession.h src/capture/CaptureSession.cpp)
    set_target_properties(huhucapture_capture PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
    target_link_libraries(huhucapture_capture PUBLIC huhucapture_core Qt6::Core Qt6::Gui Qt6::Concurrent)
    add_library(huhucapture_annotation_renderer STATIC src/services/rendering/QtAnnotationRenderer.cpp)
    target_link_libraries(huhucapture_annotation_renderer PUBLIC huhucapture_core Qt6::Gui)
    add_library(huhucapture_editor STATIC
        src/ui/qt/EditorTypes.h src/ui/qt/EditorTypes.cpp
        src/ui/qt/EditorShape.h src/ui/qt/EditorShape.cpp
        src/ui/qt/EditorCanvas.h src/ui/qt/EditorCanvas.cpp
        src/ui/qt/EditorWindow.h src/ui/qt/EditorWindow.cpp
        resources/qt/huhucapture_ui.qrc)
    set_target_properties(huhucapture_editor PROPERTIES AUTOMOC ON AUTORCC ON CXX_EXTENSIONS OFF)
    target_link_libraries(huhucapture_editor PUBLIC huhucapture_annotation_renderer Qt6::Widgets)
    add_library(huhucapture_desktop STATIC
        src/ui/qt/DesktopTypes.h src/ui/qt/DesktopTheme.h
        src/ui/qt/HistoryWindow.h src/ui/qt/HistoryWindow.cpp
        src/ui/qt/SettingsWindow.h src/ui/qt/SettingsWindow.cpp
        src/ui/qt/AboutPanel.h src/ui/qt/AboutPanel.cpp)
    set_target_properties(huhucapture_desktop PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
    target_link_libraries(huhucapture_desktop PUBLIC huhucapture_core Qt6::Widgets)
    add_library(huhucapture_json STATIC src/services/config/JsonText.cpp)
    target_link_libraries(huhucapture_json PUBLIC huhucapture_core Qt6::Core)
    add_library(huhucapture_settings STATIC src/services/config/SettingsStore.cpp)
    target_link_libraries(huhucapture_settings PUBLIC huhucapture_json
        PRIVATE huhucapture_file_committer)
    add_library(huhucapture_languages STATIC
        src/services/localization/LanguageCatalog.h
        src/services/localization/LanguageCatalog.cpp
        src/services/localization/QtTranslations.h
        src/services/localization/QtTranslations.cpp
        resources/qt/huhucapture_languages.qrc)
    set_target_properties(huhucapture_languages PROPERTIES AUTOMOC ON AUTORCC ON CXX_EXTENSIONS OFF)
    target_link_libraries(huhucapture_languages PUBLIC huhucapture_json)
    add_library(huhucapture_updates STATIC
        src/services/update/UpdateService.h src/services/update/UpdateService.cpp
        src/services/update/ReleaseManifest.h src/services/update/ReleaseManifest.cpp
        src/services/update/Installation.h src/services/update/Installation.cpp)
    set_target_properties(huhucapture_updates PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
    target_link_libraries(huhucapture_updates PUBLIC huhucapture_core Qt6::Core Qt6::Network
        PRIVATE huhucapture_file_committer)
    add_library(huhucapture_media_archive STATIC src/services/media/MediaArchiveService.cpp)
    target_link_libraries(huhucapture_media_archive PUBLIC huhucapture_core Qt6::Gui
        PRIVATE huhucapture_file_committer Threads::Threads)
    set_target_properties(huhucapture_media_archive PROPERTIES CXX_EXTENSIONS OFF)
    add_library(huhucapture_recording_session STATIC
        src/recording/RecordingSession.h src/recording/RecordingSession.cpp)
    set_target_properties(huhucapture_recording_session PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
    target_link_libraries(huhucapture_recording_session PUBLIC huhucapture_media_archive Qt6::Core
        PRIVATE Qt6::Concurrent)
    add_library(huhucapture_recording_ui STATIC
        src/ui/qt/RecordingWindow.h src/ui/qt/RecordingWindow.cpp)
    set_target_properties(huhucapture_recording_ui PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
    target_link_libraries(huhucapture_recording_ui PUBLIC
        huhucapture_recording_session huhucapture_capture huhucapture_editor Qt6::Widgets)
    if(HUHUCAPTURE_DESKTOP_VALIDATION)
        find_package(Qt6 6.12.1 EXACT REQUIRED COMPONENTS Concurrent)
        add_library(huhucapture_desktop_services STATIC
            src/services/desktop/DesktopResult.h
            src/services/desktop/CaptureExclusion.h src/services/desktop/CaptureExclusion.cpp
            src/services/desktop/SystemClipboard.h src/services/desktop/SystemClipboard.cpp
            src/services/desktop/DesktopFiles.h src/services/desktop/DesktopFiles.cpp
            src/services/desktop/FileRevealer.h src/services/desktop/FileRevealer.cpp)
        set_target_properties(huhucapture_desktop_services PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
        target_link_libraries(huhucapture_desktop_services PUBLIC huhucapture_media_archive Qt6::Gui
            PRIVATE Qt6::Concurrent)
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
            src/app/MediaWorkspace.h src/app/MediaWorkspace.cpp)
        set_target_properties(huhucapture_media_workspace PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
        target_link_libraries(huhucapture_media_workspace PUBLIC huhucapture_desktop_services
            huhucapture_desktop huhucapture_editor Qt6::Concurrent)
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
        add_library(huhucapture_gif_encoder STATIC src/services/media/GifEncoder.cpp)
        target_link_libraries(huhucapture_gif_encoder PUBLIC
            huhucapture_media_frames huhucapture_file_committer PRIVATE huhucapture_rust)
        set_target_properties(huhucapture_gif_encoder PROPERTIES CXX_EXTENSIONS OFF)
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
        find_package(Qt6 6.12.1 EXACT REQUIRED COMPONENTS Concurrent)
        add_library(huhucapture_lan_controller STATIC
            src/app/LanController.h src/app/LanController.cpp
            src/services/transfer/IdentityStore.h src/services/transfer/IdentityStore.cpp)
        set_target_properties(huhucapture_lan_controller PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
        target_link_libraries(huhucapture_lan_controller PUBLIC
            huhucapture_transfer_ui huhucapture_receive_archive huhucapture_settings
            huhucapture_file_committer Qt6::Concurrent Qt6::Network)
    endif()
    if(WIN32)
        add_library(huhucapture_windows_capture STATIC
            src/platform/windows/GdiCapture.cpp src/platform/windows/CaptureGeometry.cpp)
        target_link_libraries(huhucapture_windows_capture PUBLIC huhucapture_core gdi32 user32 dwmapi)
        target_compile_definitions(huhucapture_windows_capture PUBLIC
            UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
        target_compile_options(huhucapture_windows_capture PRIVATE /W4 /utf-8 /permissive-)
        set_target_properties(huhucapture_windows_capture PROPERTIES CXX_EXTENSIONS OFF)
        # Compile the retained native backends without Ling, its PCH, or /MT.
        # This verifies the Qt/native link boundary, not hardware capture.
        add_library(huhucapture_native_media STATIC
            src/platform/windows/FileHandleStream.cpp
            src/platform/windows/NativeMp4Recording.cpp
            src/recording/VideoMp4.cpp)
        target_link_libraries(huhucapture_native_media PUBLIC huhucapture_core huhucapture_file_committer
            huhucapture_windows_capture ole32 oleaut32 uuid shell32 shlwapi propsys
            d3d11 dxgi windowscodecs mf mfplat mfreadwrite mfuuid
            wininet comctl32)
        target_compile_definitions(huhucapture_native_media PUBLIC
            UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
        target_compile_options(huhucapture_native_media PRIVATE /W4 /utf-8 /permissive-)
        if(HUHUCAPTURE_RUST_VALIDATION)
            add_library(huhucapture_native_gif STATIC src/platform/windows/NativeGifRecording.cpp)
            target_link_libraries(huhucapture_native_gif PUBLIC
                huhucapture_gif_encoder huhucapture_native_media)
            target_compile_options(huhucapture_native_gif PRIVATE /W4 /utf-8 /permissive-)
            set_target_properties(huhucapture_native_gif PROPERTIES CXX_EXTENSIONS OFF)
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
        add_library(huhucapture_pipewire_audio STATIC src/platform/linux/pipewire/PipeWireAudio.cpp)
        target_link_libraries(huhucapture_pipewire_audio PUBLIC huhucapture_core
            PRIVATE PkgConfig::PIPEWIRE Threads::Threads)
        set_target_properties(huhucapture_pipewire_audio PROPERTIES CXX_EXTENSIONS OFF)
        target_compile_options(huhucapture_pipewire_audio PRIVATE -Wall -Wextra -Wpedantic)
        # Actual Portal/PipeWire frames require an owned compositor and daemon;
        # the isolated wrapper in CI produces the corresponding evidence.
    endif()
    if(WIN32 OR HUHUCAPTURE_PORTAL_VALIDATION)
        add_library(huhucapture_window_activation STATIC
            src/services/desktop/WindowActivation.h src/services/desktop/WindowActivation.cpp)
        set_target_properties(huhucapture_window_activation PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
        target_link_libraries(huhucapture_window_activation PUBLIC huhucapture_core Qt6::Core Qt6::Gui)
        if(WIN32)
            target_sources(huhucapture_window_activation PRIVATE src/platform/windows/WindowFocus.h)
            target_link_libraries(huhucapture_window_activation PRIVATE user32)
        endif()
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            target_sources(huhucapture_window_activation PRIVATE
                src/platform/linux/WindowActivation.h src/platform/linux/WindowActivation.cpp
                third_party/kwayland-activation/WaylandActivation.h
                third_party/kwayland-activation/WaylandActivation.cpp)
            target_include_directories(huhucapture_window_activation PRIVATE "${PROJECT_SOURCE_DIR}/third_party/kwayland-activation")
            target_link_libraries(huhucapture_window_activation PRIVATE
                Qt6::WaylandClient Qt6::GuiPrivate PkgConfig::WAYLAND_CLIENT)
            qt6_generate_wayland_protocol_client_sources(huhucapture_window_activation PRIVATE_CODE
                FILES "${PROJECT_SOURCE_DIR}/third_party/wayland-protocols/xdg-activation-v1.xml")
        endif()
        add_library(huhucapture_capture_ui STATIC
            src/ui/qt/CaptureCanvas.h src/ui/qt/CaptureCanvas.cpp
            src/ui/qt/CaptureWindow.h src/ui/qt/CaptureWindow.cpp)
        set_target_properties(huhucapture_capture_ui PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
        target_link_libraries(huhucapture_capture_ui PUBLIC huhucapture_capture
            huhucapture_editor huhucapture_window_activation Qt6::Widgets)
        add_library(huhucapture_screen_capture STATIC src/services/capture/ScreenCaptureService.h
            src/services/capture/ScreenCaptureService.cpp src/platform/ScreenCaptureBackend.h)
        set_target_properties(huhucapture_screen_capture PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
        target_compile_definitions(huhucapture_screen_capture PRIVATE QT_NO_KEYWORDS)
        target_link_libraries(huhucapture_screen_capture PUBLIC huhucapture_capture
            huhucapture_media_archive Qt6::Core Qt6::Gui Qt6::Concurrent)
        if(WIN32)
            target_sources(huhucapture_screen_capture PRIVATE src/platform/windows/ScreenCaptureBackend.cpp)
            target_link_libraries(huhucapture_screen_capture PRIVATE huhucapture_windows_capture)
        else()
            pkg_check_modules(XCB_CAPTURE REQUIRED IMPORTED_TARGET xcb xcb-randr xcb-shm xcb-xfixes xcb-xtest)
            add_library(huhucapture_x11_capture STATIC src/platform/linux/X11Capture.cpp)
            set_target_properties(huhucapture_x11_capture PROPERTIES CXX_EXTENSIONS OFF)
            target_link_libraries(huhucapture_x11_capture PUBLIC huhucapture_core PRIVATE PkgConfig::XCB_CAPTURE)
            target_sources(huhucapture_screen_capture PRIVATE src/platform/linux/ScreenCaptureBackend.cpp)
            target_link_libraries(huhucapture_screen_capture PRIVATE huhucapture_x11_capture huhucapture_portal)
        endif()
        if(WIN32 OR TARGET huhucapture_pipewire)
            add_library(huhucapture_live_capture STATIC
                src/services/capture/LiveCaptureSource.h src/services/capture/LiveCaptureSource.cpp)
            set_target_properties(huhucapture_live_capture PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
            target_compile_definitions(huhucapture_live_capture PRIVATE QT_NO_KEYWORDS)
            target_link_libraries(huhucapture_live_capture PUBLIC huhucapture_capture
                huhucapture_frame_normalizer Qt6::Concurrent)
            if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
                target_link_libraries(huhucapture_live_capture PRIVATE huhucapture_portal huhucapture_pipewire)
            endif()
        endif()
        if(HUHUCAPTURE_DESKTOP_VALIDATION AND TARGET huhucapture_live_capture)
            add_library(huhucapture_capture_workspace STATIC src/app/CaptureWorkspace.h src/app/CaptureWorkspace.cpp)
            set_target_properties(huhucapture_capture_workspace PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
            target_link_libraries(huhucapture_capture_workspace PUBLIC huhucapture_screen_capture huhucapture_live_capture
                huhucapture_capture_ui huhucapture_media_workspace huhucapture_qr_service)
            add_library(huhucapture_recording_workspace STATIC src/app/RecordingWorkspace.h src/app/RecordingWorkspace.cpp)
            set_target_properties(huhucapture_recording_workspace PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
            target_link_libraries(huhucapture_recording_workspace PUBLIC huhucapture_capture_workspace
                huhucapture_recording_ui huhucapture_recording_session)
            add_library(huhucapture_native_scroll STATIC src/platform/NativeScrollSource.h)
            target_link_libraries(huhucapture_native_scroll PUBLIC huhucapture_core)
            set_target_properties(huhucapture_native_scroll PROPERTIES CXX_EXTENSIONS OFF)
            if(WIN32)
                target_sources(huhucapture_native_scroll PRIVATE src/platform/windows/NativeScrollSource.cpp)
                target_link_libraries(huhucapture_native_scroll PRIVATE huhucapture_windows_capture)
            else()
                target_sources(huhucapture_native_scroll PRIVATE src/platform/linux/NativeScrollSource.cpp)
                target_link_libraries(huhucapture_native_scroll PRIVATE huhucapture_x11_capture)
            endif()
            add_library(huhucapture_scroll_session STATIC
                src/capture/ScrollCaptureSession.h src/capture/ScrollCaptureSession.cpp)
            set_target_properties(huhucapture_scroll_session PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
            target_link_libraries(huhucapture_scroll_session PUBLIC huhucapture_capture
                huhucapture_scroll_model huhucapture_live_capture huhucapture_screen_capture
                PRIVATE huhucapture_native_scroll huhucapture_frame_normalizer Qt6::Concurrent)
            add_library(huhucapture_scroll_ui STATIC src/ui/qt/ScrollWindow.h src/ui/qt/ScrollWindow.cpp)
            set_target_properties(huhucapture_scroll_ui PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
            target_link_libraries(huhucapture_scroll_ui PUBLIC huhucapture_scroll_session
                huhucapture_editor huhucapture_desktop_services
                PRIVATE huhucapture_window_activation)
            add_library(huhucapture_scroll_workspace STATIC src/app/ScrollWorkspace.h src/app/ScrollWorkspace.cpp)
            set_target_properties(huhucapture_scroll_workspace PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
            target_link_libraries(huhucapture_scroll_workspace PUBLIC huhucapture_capture_workspace
                huhucapture_media_workspace huhucapture_scroll_ui)
        endif()
        add_library(huhucapture_settings_controller STATIC
            src/app/SettingsController.h src/app/SettingsController.cpp
            src/services/desktop/GlobalShortcut.h src/services/desktop/GlobalShortcut.cpp
            src/services/desktop/LaunchCommand.h src/services/desktop/LaunchCommand.cpp
            src/services/desktop/AutoStart.h src/platform/GlobalShortcutBackend.h)
        set_target_properties(huhucapture_settings_controller PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
        target_compile_definitions(huhucapture_settings_controller PRIVATE QT_NO_KEYWORDS)
        target_link_libraries(huhucapture_settings_controller PUBLIC
            huhucapture_desktop huhucapture_settings huhucapture_languages Qt6::Widgets)
        if(WIN32)
            target_sources(huhucapture_settings_controller PRIVATE
                src/platform/windows/GlobalShortcut.cpp src/platform/windows/AutoStart.cpp)
            target_link_libraries(huhucapture_settings_controller PRIVATE user32 advapi32 shell32)
        else()
            pkg_check_modules(XCB_INPUT REQUIRED IMPORTED_TARGET xcb xcb-keysyms xkbcommon)
            target_sources(huhucapture_settings_controller PRIVATE
                src/platform/linux/GlobalShortcut.cpp src/platform/linux/AutoStart.cpp)
            target_link_libraries(huhucapture_settings_controller PRIVATE
                huhucapture_portal PkgConfig::GIO PkgConfig::XCB_INPUT)
        endif()
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
        add_library(huhucapture_mp4_encoder STATIC src/services/media/Mp4Encoder.cpp)
        target_link_libraries(huhucapture_mp4_encoder PUBLIC
            huhucapture_media_frames huhucapture_file_committer PRIVATE PkgConfig::FFMPEG)
        set_target_properties(huhucapture_mp4_encoder PROPERTIES CXX_EXTENSIONS OFF)
    endif()
    if(HUHUCAPTURE_VIDEO_VALIDATION)
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
        else()
            message(FATAL_ERROR "Video inspection validation targets Windows x64 and Linux amd64")
        endif()
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TARGET huhucapture_mp4_encoder
        AND TARGET huhucapture_gif_encoder AND TARGET huhucapture_pipewire_audio)
        add_library(huhucapture_live_recording STATIC
            src/platform/linux/LiveRecording.h src/platform/linux/LiveRecording.cpp)
        target_link_libraries(huhucapture_live_recording PUBLIC huhucapture_frame_normalizer
            huhucapture_recording_session PRIVATE huhucapture_mp4_encoder huhucapture_gif_encoder
            huhucapture_pipewire_audio)
        set_target_properties(huhucapture_live_recording PROPERTIES CXX_EXTENSIONS OFF)
        add_library(huhucapture_x11_recording STATIC
            src/platform/linux/X11Recording.h src/platform/linux/X11Recording.cpp)
        target_link_libraries(huhucapture_x11_recording PUBLIC huhucapture_live_recording
            PRIVATE huhucapture_x11_capture)
        set_target_properties(huhucapture_x11_recording PROPERTIES CXX_EXTENSIONS OFF)
    endif()
endif()


if(HUHUCAPTURE_BUILD_APP)
    add_library(huhucapture_application STATIC src/app/QtApplication.h src/app/QtApplication.cpp)
    set_target_properties(huhucapture_application PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
    target_link_libraries(huhucapture_application PUBLIC
        huhucapture_startup huhucapture_recording_workspace huhucapture_settings_controller
        huhucapture_lan_controller huhucapture_gif_inspection huhucapture_video_inspection
        huhucapture_updates huhucapture_scroll_workspace)
    if(WIN32)
        target_sources(huhucapture_application PRIVATE src/platform/windows/DesktopSession.cpp)
        target_link_libraries(huhucapture_application PRIVATE user32)
    endif()
    qt_add_executable(huhucapture WIN32 src/app/qt_main.cpp)
    target_link_libraries(huhucapture PRIVATE huhucapture_application)
    set_target_properties(huhucapture PROPERTIES CXX_EXTENSIONS OFF)
    if(WIN32)
        target_link_libraries(huhucapture PRIVATE huhucapture_native_media huhucapture_native_gif)
        target_sources(huhucapture PRIVATE resources/qt/application.rc)
        target_include_directories(huhucapture PRIVATE "${PROJECT_SOURCE_DIR}/resources")
        target_compile_definitions(huhucapture PRIVATE
            HUHU_VERSION_MAJOR=${PROJECT_VERSION_MAJOR} HUHU_VERSION_MINOR=${PROJECT_VERSION_MINOR}
            HUHU_VERSION_PATCH=${PROJECT_VERSION_PATCH} HUHU_VERSION_REVISION=0)
        # The same per-monitor awareness used by the retained native product.
        # Qt's actual screen bindings still own per-screen coordinate conversion.
        set_target_properties(huhucapture PROPERTIES VS_DPI_AWARE "PerMonitor")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_libraries(huhucapture PRIVATE huhucapture_x11_recording huhucapture_live_recording)
    endif()
    include(build/cmake/qt-install.cmake)
endif()

if(BUILD_TESTING)
    include(build/cmake/tests.cmake)
endif()
