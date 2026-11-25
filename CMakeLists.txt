cmake_minimum_required(VERSION 3.25)
project(huhucapture LANGUAGES CXX)

# During P00/P03 this builds the shared contracts and migration validation only.
# The shipping Windows application remains on its verified build until P12.
# Do not mistake a passing core-only configuration for a Qt product build.
include(CTest)
option(HUHUCAPTURE_QT_VALIDATION "Build the pinned Qt migration validation targets" OFF)

add_library(huhucapture_core INTERFACE)
target_include_directories(huhucapture_core INTERFACE "${PROJECT_SOURCE_DIR}/src")
target_compile_features(huhucapture_core INTERFACE cxx_std_20)

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
    endif()
    huhucapture_test(qt_image_contract tests/qt/image_contract_tests.cpp)
    target_link_libraries(qt_image_contract PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Svg)
    set_tests_properties(qt_image_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_library(huhucapture_annotation_renderer STATIC src/services/rendering/QtAnnotationRenderer.cpp)
    target_link_libraries(huhucapture_annotation_renderer PUBLIC huhucapture_core Qt6::Gui)
    huhucapture_test(qt_annotation_render tests/qt/annotation_render_tests.cpp
        "${PROJECT_BINARY_DIR}/annotation-render/$<CONFIG>")
    target_link_libraries(qt_annotation_render PRIVATE huhucapture_annotation_renderer)
    set_tests_properties(qt_annotation_render PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_library(huhucapture_settings STATIC
        src/services/config/JsonText.cpp
        src/services/config/SettingsStore.cpp)
    target_link_libraries(huhucapture_settings PUBLIC huhucapture_core Qt6::Core
        PRIVATE huhucapture_file_committer)
    huhucapture_test(qt_settings_store tests/qt/settings_store_tests.cpp)
    target_link_libraries(qt_settings_store PRIVATE huhucapture_settings)
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
endif()
