# Fixed-source inspection DLLs, using the actual matching-CRT build metadata.
# This does not replace the retained Windows capture / MF recorder.
set(HUHUCAPTURE_WINDOWS_VIDEO_METADATA
    "${PROJECT_SOURCE_DIR}/.deps/video-inspection-windows/build-info.json"
    CACHE FILEPATH "Actual FFmpeg Windows inspection build and integrity metadata")
file(READ "${HUHUCAPTURE_WINDOWS_VIDEO_METADATA}" video_metadata)
file(READ "${PROJECT_SOURCE_DIR}/build/dependencies.lock.json" dependency_lock)
string(JSON video_source GET "${video_metadata}" source)
string(JSON expected_source GET "${dependency_lock}" qtMigration softwareMedia ffmpeg commit)
string(JSON video_patches GET "${video_metadata}" patches)
string(JSON expected_patches GET "${dependency_lock}" qtMigration softwareMedia ffmpeg patches)
if(NOT video_source STREQUAL expected_source OR NOT video_patches STREQUAL expected_patches)
    message(FATAL_ERROR "Video inspection metadata does not match the fixed source and strict patches")
endif()

foreach(configuration IN ITEMS Debug Release)
    string(JSON prefix GET "${video_metadata}" configurations ${configuration} prefix)
    string(JSON actual_crt GET "${video_metadata}" configurations ${configuration} crt)
    string(JSON expected_crt GET "${dependency_lock}" qtMigration videoInspectionWindows configurations ${configuration})
    if(NOT actual_crt STREQUAL expected_crt OR NOT EXISTS "${prefix}/include/libavcodec/avcodec.h")
        message(FATAL_ERROR "Missing ${configuration} FFmpeg headers or matching CRT metadata")
    endif()
    file(TO_CMAKE_PATH "${prefix}" video_prefix_${configuration})
    string(JSON file_count LENGTH "${video_metadata}" configurations ${configuration} files)
    if(NOT file_count EQUAL 8)
        message(FATAL_ERROR "The inspection profile must have exactly four DLLs and four import libraries")
    endif()
    set(checked_files)
    math(EXPR last_file "${file_count} - 1")
    foreach(index RANGE ${last_file})
        string(JSON relative GET "${video_metadata}" configurations ${configuration} files ${index} path)
        file(TO_CMAKE_PATH "${relative}" relative)
        string(JSON expected_hash GET "${video_metadata}" configurations ${configuration} files ${index} sha256)
        if(NOT relative MATCHES "^bin/(avcodec-63\\.dll|avformat-63\\.dll|avutil-61\\.dll|swscale-10\\.dll|avcodec\\.lib|avformat\\.lib|avutil\\.lib|swscale\\.lib)$")
            message(FATAL_ERROR "Unexpected inspection binary: ${relative}")
        endif()
        if(relative IN_LIST checked_files OR NOT EXISTS "${prefix}/${relative}")
            message(FATAL_ERROR "Duplicate or missing inspection binary: ${relative}")
        endif()
        list(APPEND checked_files "${relative}")
        file(SHA256 "${prefix}/${relative}" actual_hash)
        if(NOT actual_hash STREQUAL expected_hash)
            message(FATAL_ERROR "Inspection binary changed after its build: ${relative}")
        endif()
    endforeach()
endforeach()

add_library(huhucapture_video_ffmpeg INTERFACE)
foreach(component IN ITEMS avcodec avformat avutil swscale)
    if(component STREQUAL "avutil")
        set(major 61)
    elseif(component STREQUAL "swscale")
        set(major 10)
    else()
        set(major 63)
    endif()
    add_library(huhucapture_ffmpeg_${component} SHARED IMPORTED)
    set_target_properties(huhucapture_ffmpeg_${component} PROPERTIES
        IMPORTED_CONFIGURATIONS "Debug;Release"
        IMPORTED_IMPLIB_DEBUG "${video_prefix_Debug}/bin/${component}.lib"
        IMPORTED_LOCATION_DEBUG "${video_prefix_Debug}/bin/${component}-${major}.dll"
        IMPORTED_IMPLIB_RELEASE "${video_prefix_Release}/bin/${component}.lib"
        IMPORTED_LOCATION_RELEASE "${video_prefix_Release}/bin/${component}-${major}.dll"
        MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release
        MAP_IMPORTED_CONFIG_MINSIZEREL Release
        INTERFACE_INCLUDE_DIRECTORIES "${video_prefix_Release}/include")
    target_link_libraries(huhucapture_video_ffmpeg INTERFACE huhucapture_ffmpeg_${component})
endforeach()
