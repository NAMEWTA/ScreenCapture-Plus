# Shared production services are separate from the executables that validate
# them. BUILD_TESTING=OFF builds the same code and does not need test daemons.
find_package(Qt6 6.12.1 EXACT REQUIRED COMPONENTS Concurrent)

add_library(huhucapture_updates STATIC
    src/services/update/ReleaseManifest.h src/services/update/ReleaseManifest.cpp
    src/services/update/UpdateService.h src/services/update/UpdateService.cpp)
set_target_properties(huhucapture_updates PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
target_link_libraries(huhucapture_updates PUBLIC huhucapture_core Qt6::Core Qt6::Network
    PRIVATE huhucapture_file_committer Threads::Threads)

add_library(huhucapture_capture_session STATIC
    src/capture/CaptureTypes.h src/capture/CaptureSession.h src/capture/CaptureSession.cpp
    src/ui/qt/CaptureWindow.h src/ui/qt/CaptureWindow.cpp
    src/ui/qt/CapturePlacement.h src/ui/qt/CapturePlacement.cpp)
set_target_properties(huhucapture_capture_session PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
target_link_libraries(huhucapture_capture_session PUBLIC huhucapture_editor huhucapture_media_frames)
if(WIN32)
    target_sources(huhucapture_capture_session PRIVATE src/platform/windows/DesktopPointer.cpp)
    target_link_libraries(huhucapture_capture_session PRIVATE user32)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(XCB_POINTER REQUIRED IMPORTED_TARGET xcb)
    target_sources(huhucapture_capture_session PRIVATE src/platform/linux/DesktopPointer.cpp)
    target_link_libraries(huhucapture_capture_session PRIVATE PkgConfig::XCB_POINTER)
endif()

if(BUILD_TESTING)
    huhucapture_test(qt_updates tests/qt/update_service_tests.cpp)
    target_link_libraries(qt_updates PRIVATE huhucapture_updates)
    set_tests_properties(qt_updates PROPERTIES TIMEOUT 180)
    huhucapture_test(qt_capture_session tests/qt/capture_session_tests.cpp)
    target_link_libraries(qt_capture_session PRIVATE huhucapture_capture_session)
    set_tests_properties(qt_capture_session PROPERTIES TIMEOUT 120
        ENVIRONMENT "QT_QPA_PLATFORM=${HUHUCAPTURE_TEST_QPA}")
endif()

if(BUILD_TESTING AND TARGET huhucapture_desktop_services)
    huhucapture_test(qt_desktop_shutdown tests/qt/desktop_shutdown_tests.cpp)
    target_link_libraries(qt_desktop_shutdown PRIVATE huhucapture_desktop_services Qt6::Widgets)
    set_tests_properties(qt_desktop_shutdown PROPERTIES TIMEOUT 60
        ENVIRONMENT "QT_QPA_PLATFORM=${HUHUCAPTURE_TEST_QPA}")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TARGET PkgConfig::PIPEWIRE)
    add_library(huhucapture_pipewire_audio STATIC
        src/platform/linux/pipewire/PipeWireAudio.h src/platform/linux/pipewire/PipeWireAudio.cpp)
    target_link_libraries(huhucapture_pipewire_audio PUBLIC huhucapture_core
        PRIVATE PkgConfig::PIPEWIRE Threads::Threads)
    set_target_properties(huhucapture_pipewire_audio PROPERTIES CXX_EXTENSIONS OFF)
    if(BUILD_TESTING)
        huhucapture_test(pipewire_audio tests/platform/pipewire_audio_tests.cpp)
        target_link_libraries(pipewire_audio PRIVATE huhucapture_pipewire_audio)
    endif()
endif()

if(HUHUCAPTURE_RUST_VALIDATION)
    add_library(huhucapture_recording STATIC
        src/core/FrameSource.h src/recording/RecordingBackend.h
        src/recording/FrameSource.h src/recording/FrameSource.cpp
        src/recording/FramePixels.h src/recording/GifRecording.h src/recording/GifRecording.cpp
        src/recording/RecordingSession.h src/recording/RecordingSession.cpp
        src/ui/qt/RecordingWindow.h src/ui/qt/RecordingWindow.cpp)
    set_target_properties(huhucapture_recording PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
    target_link_libraries(huhucapture_recording PUBLIC huhucapture_media_archive
        huhucapture_media_frames huhucapture_desktop Qt6::Concurrent
        PRIVATE huhucapture_rust)
    if(BUILD_TESTING)
        huhucapture_test(qt_recording_admission tests/qt/recording_admission_tests.cpp)
        target_link_libraries(qt_recording_admission PRIVATE huhucapture_recording)
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TARGET PkgConfig::FFMPEG)
        target_sources(huhucapture_recording PRIVATE
            src/recording/SoftwareMp4Recording.h src/recording/SoftwareMp4Recording.cpp)
        target_link_libraries(huhucapture_recording PRIVATE PkgConfig::FFMPEG)
        if(BUILD_TESTING AND TARGET huhucapture_video_inspection)
            huhucapture_test(qt_recording_pipeline tests/qt/recording_pipeline_tests.cpp)
            target_link_libraries(qt_recording_pipeline PRIVATE huhucapture_recording
                huhucapture_gif_inspection huhucapture_video_inspection PkgConfig::FFMPEG)
            set_tests_properties(qt_recording_pipeline PROPERTIES TIMEOUT 180
                ENVIRONMENT "QT_QPA_PLATFORM=${HUHUCAPTURE_TEST_QPA}")
        endif()
    endif()
    if(BUILD_TESTING AND TARGET huhucapture_video_inspection)
        huhucapture_test(qt_transfer_all_media tests/qt/transfer_service_tests.cpp "${HUHUCAPTURE_VIDEO_FIXTURES}")
        target_compile_definitions(qt_transfer_all_media PRIVATE HUHUCAPTURE_TRANSFER_ALL_MEDIA)
        target_link_libraries(qt_transfer_all_media PRIVATE huhucapture_receive_archive
            huhucapture_gif_inspection huhucapture_video_inspection Qt6::Network)
        set_tests_properties(qt_transfer_all_media PROPERTIES TIMEOUT 240)
    endif()
endif()

if(BUILD_TESTING AND TARGET huhucapture_video_inspection)
    add_executable(qt_legacy_mp3_recording tests/qt/legacy_mp3_recording_tests.cpp)
    set_target_properties(qt_legacy_mp3_recording PROPERTIES CXX_EXTENSIONS OFF)
    target_link_libraries(qt_legacy_mp3_recording PRIVATE huhucapture_video_inspection huhucapture_file_committer)
    if(WIN32)
        target_sources(qt_legacy_mp3_recording PRIVATE src/platform/windows/FileHandleStream.cpp)
        target_compile_definitions(qt_legacy_mp3_recording PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
        target_link_libraries(qt_legacy_mp3_recording PRIVATE huhucapture_ffmpeg_avcodec huhucapture_ffmpeg_avformat huhucapture_ffmpeg_avutil mfreadwrite mfplat mfuuid ole32)
    else()
        target_link_libraries(qt_legacy_mp3_recording PRIVATE PkgConfig::FFMPEG)
    endif()
    set(HUHUCAPTURE_LEGACY_MP3_FIXTURES "" CACHE PATH
        "Same-byte actual Windows Media Foundation MP3-in-MP4 fixtures")
    if(HUHUCAPTURE_LEGACY_MP3_FIXTURES)
        add_test(NAME qt_legacy_mp3_recording COMMAND qt_legacy_mp3_recording "${HUHUCAPTURE_LEGACY_MP3_FIXTURES}")
        set_tests_properties(qt_legacy_mp3_recording PROPERTIES TIMEOUT 90)
    else()
        message(STATUS "Legacy MP3 runtime check requires the actual Windows-generated fixture artifact (HUHUCAPTURE_LEGACY_MP3_FIXTURES)")
    endif()
endif()

if(NOT HUHUCAPTURE_BUILD_APP)
    return()
endif()

add_library(huhucapture_desktop_settings STATIC
    src/platform/GlobalShortcutBackend.h
    src/platform/WindowActivation.h
    src/services/desktop/GlobalShortcut.h src/services/desktop/GlobalShortcut.cpp
    src/services/desktop/AutoStart.h
    src/services/desktop/LaunchCommand.h src/services/desktop/LaunchCommand.cpp
    src/app/SettingsController.h src/app/SettingsController.cpp)
set_target_properties(huhucapture_desktop_settings PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
target_link_libraries(huhucapture_desktop_settings PUBLIC huhucapture_settings
    huhucapture_languages huhucapture_desktop Qt6::Gui)

add_library(huhucapture_desktop_capture STATIC
    src/platform/DesktopSnapshot.h src/platform/NativeScrollSource.h
    src/services/capture/DesktopCapture.h src/services/capture/DesktopCapture.cpp
    src/services/capture/ScreenCastSource.h src/services/capture/ScreenCastSource.cpp)
set_target_properties(huhucapture_desktop_capture PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
target_link_libraries(huhucapture_desktop_capture PUBLIC huhucapture_capture_session
    huhucapture_media_archive Qt6::Concurrent)

if(WIN32)
    target_sources(huhucapture_desktop_settings PRIVATE
        src/platform/windows/GlobalShortcut.cpp src/platform/windows/AutoStart.cpp
        src/platform/windows/WindowActivation.cpp)
    target_link_libraries(huhucapture_desktop_settings PRIVATE user32 advapi32)
    target_sources(huhucapture_desktop_capture PRIVATE
        src/platform/windows/DesktopSnapshot.cpp src/platform/windows/NativeScrollSource.cpp)
    target_link_libraries(huhucapture_desktop_capture PRIVATE huhucapture_native_media dwmapi)
    target_sources(huhucapture_recording PRIVATE
        src/platform/windows/RecordingBackend.cpp
        src/platform/windows/NativeMp4Recording.h src/platform/windows/NativeMp4Recording.cpp
        src/platform/windows/FileHandleStream.h src/platform/windows/FileHandleStream.cpp)
    target_link_libraries(huhucapture_recording PRIVATE huhucapture_native_media)
    foreach(target IN ITEMS huhucapture_desktop_settings huhucapture_desktop_capture huhucapture_recording)
        target_compile_definitions(${target} PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
    endforeach()
    if(BUILD_TESTING)
        huhucapture_test(windows_file_stream tests/platform/windows_file_stream_tests.cpp)
        target_link_libraries(windows_file_stream PRIVATE huhucapture_recording)
        huhucapture_test(qt_recording_session tests/qt/recording_session_tests.cpp)
        target_link_libraries(qt_recording_session PRIVATE huhucapture_recording huhucapture_native_media)
        huhucapture_test(qt_native_recording_probe tests/qt/native_recording_probe.cpp
            "${PROJECT_BINARY_DIR}/native-recording/$<CONFIG>")
        target_link_libraries(qt_native_recording_probe PRIVATE huhucapture_recording
            huhucapture_native_media huhucapture_video_inspection Qt6::Concurrent Qt6::Widgets)
        set_tests_properties(qt_native_recording_probe PROPERTIES TIMEOUT 180
            ENVIRONMENT "QT_QPA_PLATFORM=windows")
    endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Qt6 6.12.1 EXACT REQUIRED COMPONENTS WaylandClientPrivate GuiPrivate)
    pkg_check_modules(XCB_CAPTURE REQUIRED IMPORTED_TARGET xcb xcb-shm xcb-randr xcb-xfixes xcb-xtest)
    pkg_check_modules(XCB_SHORTCUT REQUIRED IMPORTED_TARGET xcb-keysyms xkbcommon)
    target_sources(huhucapture_desktop_settings PRIVATE
        src/platform/linux/GlobalShortcut.cpp src/platform/linux/AutoStart.cpp
        src/platform/linux/WindowActivation.cpp)
    target_link_libraries(huhucapture_desktop_settings PRIVATE
        huhucapture_portal PkgConfig::XCB_SHORTCUT PkgConfig::GIO
        Qt6::WaylandClientPrivate Qt6::GuiPrivate)
    target_sources(huhucapture_desktop_capture PRIVATE
        src/platform/linux/DesktopSnapshot.cpp src/platform/linux/NativeScrollSource.cpp
        src/platform/linux/X11Capture.h src/platform/linux/X11Capture.cpp)
    target_link_libraries(huhucapture_desktop_capture PRIVATE
        huhucapture_portal huhucapture_pipewire PkgConfig::XCB_CAPTURE)
    target_compile_definitions(huhucapture_desktop_capture PRIVATE HUHUCAPTURE_WITH_PORTAL)
    target_sources(huhucapture_recording PRIVATE
        src/platform/linux/RecordingBackend.cpp)
    target_link_libraries(huhucapture_recording PRIVATE
        huhucapture_desktop_capture huhucapture_pipewire_audio PkgConfig::FFMPEG Threads::Threads)
endif()

add_library(huhucapture_application STATIC
    src/app/DesktopApplication.h src/app/DesktopApplication.cpp
    src/app/RecordingController.h src/app/RecordingController.cpp
    src/app/ScrollController.h src/app/ScrollController.cpp)
set_target_properties(huhucapture_application PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
target_link_libraries(huhucapture_application PUBLIC
    huhucapture_startup huhucapture_media_workspace huhucapture_lan_controller
    huhucapture_desktop_settings huhucapture_desktop_capture huhucapture_recording
    huhucapture_qr_service huhucapture_gif_inspection huhucapture_video_inspection
    huhucapture_updates)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(huhucapture_application PRIVATE HUHUCAPTURE_WITH_PORTAL)
endif()
qt_add_executable(huhucapture WIN32 src/app/QtMain.cpp)
target_link_libraries(huhucapture PRIVATE huhucapture_application)
set_target_properties(huhucapture PROPERTIES CXX_EXTENSIONS OFF
    OUTPUT_NAME huhucapture)
include(build/cmake/qt-install.cmake)

if(BUILD_TESTING)
    add_executable(qt_package_runtime_probe tests/qt/package_runtime_probe.cpp)
    target_compile_features(qt_package_runtime_probe PRIVATE cxx_std_20)
    set_target_properties(qt_package_runtime_probe PROPERTIES CXX_EXTENSIONS OFF)
    target_link_libraries(qt_package_runtime_probe PRIVATE Qt6::Core Qt6::Network)
    add_executable(qt_application tests/qt/application_tests.cpp)
    target_link_libraries(qt_application PRIVATE huhucapture_application)
    set_target_properties(qt_application PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME qt_application_one_shot COMMAND qt_application)
    set_tests_properties(qt_application_one_shot PROPERTIES TIMEOUT 90 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_test(NAME qt_application_primary COMMAND qt_application --primary)
    set_tests_properties(qt_application_primary PROPERTIES TIMEOUT 90
        ENVIRONMENT "QT_QPA_PLATFORM=${HUHUCAPTURE_TEST_QPA}")
    huhucapture_test(qt_capture_placement tests/qt/capture_placement_tests.cpp)
    target_link_libraries(qt_capture_placement PRIVATE huhucapture_capture_session huhucapture_desktop_settings)
    set_tests_properties(qt_capture_placement PROPERTIES TIMEOUT 60
        ENVIRONMENT "QT_QPA_PLATFORM=${HUHUCAPTURE_TEST_QPA}")
    add_executable(qt_settings_controller tests/qt/settings_controller_tests.cpp)
    set_target_properties(qt_settings_controller PROPERTIES CXX_EXTENSIONS OFF)
    target_link_libraries(qt_settings_controller PRIVATE huhucapture_desktop_settings)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(qt_capture_lifecycle tests/qt/capture_lifecycle_tests.cpp)
        set_target_properties(qt_capture_lifecycle PROPERTIES CXX_EXTENSIONS OFF)
        target_link_libraries(qt_capture_lifecycle PRIVATE huhucapture_application)
        add_test(NAME qt_capture_lifecycle COMMAND "${DBUS_RUN_SESSION}" -- $<TARGET_FILE:qt_capture_lifecycle>)
        set_tests_properties(qt_capture_lifecycle PROPERTIES TIMEOUT 90 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        add_executable(x11_capture_tests tests/platform/x11_capture_tests.cpp)
        set_target_properties(x11_capture_tests PROPERTIES CXX_EXTENSIONS OFF)
        target_link_libraries(x11_capture_tests PRIVATE huhucapture_application)
        add_executable(x11_capture_cancellation_tests tests/platform/x11_capture_cancellation_tests.cpp)
        set_target_properties(x11_capture_cancellation_tests PROPERTIES CXX_EXTENSIONS OFF)
        target_link_libraries(x11_capture_cancellation_tests PRIVATE huhucapture_desktop_capture PkgConfig::XCB_CAPTURE)
        add_test(NAME x11_capture_pre_cancel COMMAND x11_capture_cancellation_tests --pre-cancel)
        set_tests_properties(x11_capture_pre_cancel PROPERTIES TIMEOUT 15)
        if(HUHUCAPTURE_X11_TESTS)
            add_test(NAME x11_capture_tests COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/scripts/test-x11-capture.py" $<TARGET_FILE:x11_capture_tests>
                --artifacts "${PROJECT_BINARY_DIR}/x11-capture")
            set_tests_properties(x11_capture_tests PROPERTIES TIMEOUT 150)
            add_test(NAME x11_capture_cancellation_tests COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/scripts/test-x11-capture.py" $<TARGET_FILE:x11_capture_cancellation_tests>
                --paused-server --artifacts "${PROJECT_BINARY_DIR}/x11-capture-cancellation")
            set_tests_properties(x11_capture_cancellation_tests PROPERTIES TIMEOUT 90)
        endif()
        target_link_libraries(qt_settings_controller PRIVATE PkgConfig::XCB_CAPTURE PkgConfig::XCB_SHORTCUT PkgConfig::GIO)
        add_test(NAME qt_settings_controller_unavailable COMMAND qt_settings_controller
            "${PROJECT_BINARY_DIR}/settings-unavailable-render/$<CONFIG>" --unavailable)
        set_tests_properties(qt_settings_controller_unavailable PROPERTIES TIMEOUT 90 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        if(HUHUCAPTURE_X11_TESTS)
            add_test(NAME qt_settings_controller_x11 COMMAND "${XVFB_RUN}" -a
                "${CMAKE_COMMAND}" -E env "QT_QPA_PLATFORM=xcb" $<TARGET_FILE:qt_settings_controller>
                "${PROJECT_BINARY_DIR}/settings-controller-render/$<CONFIG>")
            set_tests_properties(qt_settings_controller_x11 PROPERTIES TIMEOUT 180)
        endif()
    else()
        add_test(NAME qt_settings_controller COMMAND qt_settings_controller
            "${PROJECT_BINARY_DIR}/settings-controller-render/$<CONFIG>")
        set_tests_properties(qt_settings_controller PROPERTIES TIMEOUT 180
            ENVIRONMENT "QT_QPA_PLATFORM=${HUHUCAPTURE_TEST_QPA}")
    endif()
endif()
