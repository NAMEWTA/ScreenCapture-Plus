# One installed Qt product tree. Packaging consumes this exact tree; it does not
# collect binaries from a developer PATH or retain the old single-EXE layout.
if(NOT TARGET huhucapture)
    message(FATAL_ERROR "Qt installation requires the unified huhucapture application target")
endif()
include(GNUInstallDirs)
find_package(Git REQUIRED)
if(WIN32)
    set(_huhu_platform windows)
    set(_huhu_architecture x64)
    set(_huhu_bin .)
    set(_huhu_default_kind portable-zip)
    set(_huhu_licenses licenses)
    set(_huhu_required_plugins "qwindows qschannelbackend")
else()
    set(_huhu_platform linux)
    set(_huhu_architecture amd64)
    set(_huhu_bin bin)
    set(_huhu_default_kind appimage)
    set(_huhu_licenses share/huhucapture/licenses)
    set(_huhu_required_plugins "qxcb qwayland qopensslbackend")
endif()
set(HUHUCAPTURE_PACKAGE_KIND "${_huhu_default_kind}" CACHE STRING "Actual installation form recorded beside the executable")
set_property(CACHE HUHUCAPTURE_PACKAGE_KIND PROPERTY STRINGS installer portable-zip deb appimage)
if((WIN32 AND NOT HUHUCAPTURE_PACKAGE_KIND MATCHES "^(installer|portable-zip)$") OR
   (NOT WIN32 AND NOT HUHUCAPTURE_PACKAGE_KIND MATCHES "^(deb|appimage)$"))
    message(FATAL_ERROR "The installation marker must match the actual platform and package form")
endif()
set(HUHUCAPTURE_RUNTIME_LICENSES "" CACHE PATH "Verified license directory from the restored runtime dependencies")
execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    OUTPUT_VARIABLE HUHU_PACKAGE_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=normal WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    OUTPUT_VARIABLE _huhu_source_status OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
if(_huhu_source_status STREQUAL "")
    set(HUHU_PACKAGE_DIRTY false)
else()
    set(HUHU_PACKAGE_DIRTY true)
endif()
file(SHA256 "${PROJECT_SOURCE_DIR}/build/dependencies.lock.json" HUHU_DEPENDENCY_LOCK_SHA256)
file(SHA256 "${PROJECT_SOURCE_DIR}/third_party/localsend/Cargo.lock" HUHU_CARGO_LOCK_SHA256)
set(HUHU_PACKAGE_PLATFORM "${_huhu_platform}")
set(HUHU_PACKAGE_ARCHITECTURE "${_huhu_architecture}")
configure_file("${PROJECT_SOURCE_DIR}/build/package/build-identity.json.in"
    "${PROJECT_BINARY_DIR}/package/build-identity.json.in" @ONLY)
file(GENERATE OUTPUT "${PROJECT_BINARY_DIR}/package/$<CONFIG>/build-identity.json"
    INPUT "${PROJECT_BINARY_DIR}/package/build-identity.json.in")
file(READ "${PROJECT_BINARY_DIR}/package/build-identity.json.in" HUHU_BINARY_BUILD_IDENTITY)
configure_file("${PROJECT_SOURCE_DIR}/build/package/build-identity.cpp.in"
    "${PROJECT_BINARY_DIR}/package/build-identity.cpp.in" @ONLY)
file(GENERATE OUTPUT "${PROJECT_BINARY_DIR}/package/$<CONFIG>/build-identity.cpp"
    INPUT "${PROJECT_BINARY_DIR}/package/build-identity.cpp.in")
target_sources(huhucapture PRIVATE "${PROJECT_BINARY_DIR}/package/$<CONFIG>/build-identity.cpp")
if(MSVC)
    target_link_options(huhucapture PRIVATE /INCLUDE:huhuPackageIdentity)
    # The product already compiles resources/qt/application.rc, which includes
    # the same version-info.rc as the retained Windows product. Do not embed a
    # second VERSIONINFO resource or introduce another version representation.
endif()
file(WRITE "${PROJECT_BINARY_DIR}/package/installation.json"
    "{\n  \"schemaVersion\": 1,\n  \"product\": \"huhucapture\",\n  \"version\": \"${HUHUCAPTURE_VERSION}\",\n  \"platform\": \"${_huhu_platform}\",\n  \"architecture\": \"${_huhu_architecture}\",\n  \"packageKind\": \"${HUHUCAPTURE_PACKAGE_KIND}\"\n}\n")
install(TARGETS huhucapture RUNTIME DESTINATION "${_huhu_bin}" COMPONENT Runtime)
install(FILES "${PROJECT_BINARY_DIR}/package/installation.json"
    "${PROJECT_BINARY_DIR}/package/$<CONFIG>/build-identity.json" DESTINATION "${_huhu_bin}" COMPONENT Runtime)
install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" "${PROJECT_SOURCE_DIR}/third_party/NOTICE.md"
    DESTINATION "${_huhu_licenses}" COMPONENT Runtime)
include("${PROJECT_SOURCE_DIR}/build/cmake/runtime-notices.cmake")
huhucapture_install_runtime_notices("${_huhu_licenses}")
install(FILES "${PROJECT_SOURCE_DIR}/build/dependencies.lock.json"
    DESTINATION "${_huhu_licenses}" COMPONENT Runtime)
get_target_property(_huhu_qt_core_file Qt6::Core IMPORTED_LOCATION_RELEASE)
get_filename_component(_huhu_qt_library_directory "${_huhu_qt_core_file}" DIRECTORY)
get_filename_component(_huhu_qt_prefix "${_huhu_qt_library_directory}" DIRECTORY)
if(EXISTS "${_huhu_qt_prefix}/sbom")
    install(DIRECTORY "${_huhu_qt_prefix}/sbom/" DESTINATION "${_huhu_licenses}/qt-sbom" COMPONENT Runtime)
endif()
# The official qttranslations archive already supplies compiled catalogs.
# Qt's generic deployment otherwise requires qttools/lconvert and silently
# skips all translations when that unrelated SDK tool was not installed.
install(DIRECTORY "${_huhu_qt_prefix}/translations/" DESTINATION translations
    COMPONENT Runtime FILES_MATCHING PATTERN "*.qm")
if(HUHUCAPTURE_RUNTIME_LICENSES)
    install(DIRECTORY "${HUHUCAPTURE_RUNTIME_LICENSES}/" DESTINATION "${_huhu_licenses}/runtime" COMPONENT Runtime)
endif()

# Public Qt deployment API: fixed SDK tools select its own libraries, TLS,
# platform plugins, image formats and translations. Do not hide missing tools
# with NO_UNSUPPORTED_PLATFORM_ERROR. Compiler CRT is handled separately below.
# Qt 6.12's NEW policy forwards CONTENT without re-evaluating it as a macro.
# Bracketed text and [.] patterns need no compensating backslash layers.
qt_policy(SET QTP0008 NEW)
set(_huhu_deploy_content [=[
set(QT_DEPLOY_BIN_DIR "@_huhu_bin@")
set(QT_DEPLOY_LIB_DIR "lib")
set(QT_DEPLOY_PLUGINS_DIR "plugins")
set(QT_DEPLOY_TRANSLATIONS_DIR "translations")
qt_deploy_runtime_dependencies(
    EXECUTABLE "$<TARGET_FILE:huhucapture>"
    BIN_DIR "@_huhu_bin@"
    LIB_DIR "lib"
    PLUGINS_DIR "plugins"
    GENERATE_QT_CONF
    NO_COMPILER_RUNTIME
    NO_TRANSLATIONS
    INCLUDE_PLUGIN_TYPES platforms imageformats tls
    INCLUDE_PLUGINS @_huhu_required_plugins@
    PRE_EXCLUDE_REGEXES "^ld-linux.*" "^libc[.]so.*" "^libm[.]so.*" "^libpthread[.]so.*" "^libdl[.]so.*" "^librt[.]so.*" "^libGL.*" "^libEGL.*" "^libvulkan.*" "^libdrm.*"
)
]=])
string(CONFIGURE "${_huhu_deploy_content}" _huhu_deploy_content @ONLY)
qt_generate_deploy_script(TARGET huhucapture OUTPUT_SCRIPT _huhu_deploy_script CONTENT "${_huhu_deploy_content}")
install(SCRIPT "${_huhu_deploy_script}" COMPONENT Runtime)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # GRD can resolve FFmpeg's indirect x264 dependency through the host
    # ldconfig cache, then omit that system path. Use the same private media
    # prefix as the linked codec and retain its complete shared-library chain.
    pkg_get_variable(_huhu_x264_libdir x264 libdir)
    pkg_get_variable(_huhu_avcodec_libdir libavcodec libdir)
    if(NOT IS_DIRECTORY "${_huhu_x264_libdir}" OR NOT IS_DIRECTORY "${_huhu_avcodec_libdir}")
        message(FATAL_ERROR "The locked software media prefix must provide x264 and libavcodec pkg-config metadata")
    endif()
    file(REAL_PATH "${_huhu_x264_libdir}" _huhu_x264_libdir)
    file(REAL_PATH "${_huhu_avcodec_libdir}" _huhu_avcodec_libdir)
    if(NOT _huhu_x264_libdir STREQUAL _huhu_avcodec_libdir)
        message(FATAL_ERROR "x264 and libavcodec must come from the same restored software media prefix")
    endif()
    find_file(_huhu_x264_runtime NAMES libx264.so PATHS "${_huhu_x264_libdir}"
        NO_DEFAULT_PATH NO_CACHE REQUIRED)
    file(REAL_PATH "${_huhu_x264_runtime}" _huhu_x264_real_file)
    get_filename_component(_huhu_x264_real_directory "${_huhu_x264_real_file}" DIRECTORY)
    if(NOT _huhu_x264_real_directory STREQUAL _huhu_x264_libdir)
        message(FATAL_ERROR "The x264 shared-library chain escaped its restored media directory")
    endif()
    # Keep this outside the public Qt deployment script: the Qt materials
    # collector replays only that script's exact reviewed deployment call.
    configure_file("${PROJECT_SOURCE_DIR}/build/package/linux-x264-runtime.cmake.in"
        "${PROJECT_BINARY_DIR}/package/linux-x264-runtime.cmake" @ONLY)
    install(SCRIPT "${PROJECT_BINARY_DIR}/package/linux-x264-runtime.cmake" COMPONENT Runtime)
endif()
if(WIN32)
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/Lang" DESTINATION . COMPONENT Runtime)
    foreach(_huhu_component IN ITEMS avcodec avformat avutil swscale)
        install(FILES "$<TARGET_FILE:huhucapture_ffmpeg_${_huhu_component}>" DESTINATION . COMPONENT Runtime)
    endforeach()
    # CMake's official redistributable discovery uses the selected MSVC toolset
    # REDIST directories, not arbitrary matching DLL names from PATH. Debug CRT
    # is not redistributable and must never enter these complete release trees.
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
    set(CMAKE_INSTALL_DEBUG_LIBRARIES FALSE)
    set(CMAKE_INSTALL_DEBUG_LIBRARIES_ONLY FALSE)
    set(CMAKE_INSTALL_UCRT_LIBRARIES FALSE)
    include(InstallRequiredSystemLibraries)
    if(NOT CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
        message(FATAL_ERROR "No release app-local MSVC CRT was found in this toolset's redistributable set")
    endif()
    foreach(_huhu_runtime IN LISTS CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
        file(TO_CMAKE_PATH "${_huhu_runtime}" _huhu_runtime_path)
        if(NOT _huhu_runtime_path MATCHES "/[Rr]edist/" OR _huhu_runtime_path MATCHES "[Dd]ebug")
            message(FATAL_ERROR "Runtime must come from the selected release REDIST set: ${_huhu_runtime}")
        endif()
    endforeach()
    install(PROGRAMS ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS} DESTINATION . COMPONENT Runtime
        CONFIGURATIONS Release RelWithDebInfo)
    if(MSVC)
        install(FILES "$<TARGET_PDB_FILE:huhucapture>" DESTINATION symbols COMPONENT Symbols
            CONFIGURATIONS Release RelWithDebInfo EXCLUDE_FROM_ALL)
        configure_file("${PROJECT_SOURCE_DIR}/build/package/windows-symbols.cmake.in"
            "${PROJECT_BINARY_DIR}/package/windows-symbols.cmake.in" @ONLY)
        file(GENERATE OUTPUT "${PROJECT_BINARY_DIR}/package/$<CONFIG>/windows-symbols.cmake"
            INPUT "${PROJECT_BINARY_DIR}/package/windows-symbols.cmake.in")
        install(SCRIPT "${PROJECT_BINARY_DIR}/package/$<CONFIG>/windows-symbols.cmake"
            COMPONENT Symbols EXCLUDE_FROM_ALL)
    endif()
else()
    set_target_properties(huhucapture PROPERTIES INSTALL_RPATH "$ORIGIN/../lib" INSTALL_RPATH_USE_LINK_PATH FALSE)
    # qopensslbackend resolves OpenSSL with dlopen, so dependency traversal
    # cannot see these SONAMEs. Consume the explicit restored runtime set; do
    # not silently substitute arbitrary host libraries from a search path.
    set(HUHUCAPTURE_OPENSSL_RUNTIME_DIR "" CACHE PATH "Verified OpenSSL 3 runtime directory for the Linux package")
    if(HUHUCAPTURE_OPENSSL_RUNTIME_DIR)
        foreach(_huhu_openssl_name IN ITEMS libssl.so.3 libcrypto.so.3)
            if(NOT EXISTS "${HUHUCAPTURE_OPENSSL_RUNTIME_DIR}/${_huhu_openssl_name}")
                message(FATAL_ERROR "Missing explicit Qt HTTPS runtime: ${_huhu_openssl_name}")
            endif()
            file(REAL_PATH "${HUHUCAPTURE_OPENSSL_RUNTIME_DIR}/${_huhu_openssl_name}" _huhu_openssl_file)
            install(FILES "${_huhu_openssl_file}" DESTINATION lib RENAME "${_huhu_openssl_name}" COMPONENT Runtime)
        endforeach()
    else()
        install(CODE "message(FATAL_ERROR \"Linux installation requires HUHUCAPTURE_OPENSSL_RUNTIME_DIR from the restored OpenSSL 3 dependency set\")"
            COMPONENT Runtime)
    endif()
    install(FILES "${PROJECT_SOURCE_DIR}/build/package/io.github.NAMEWTA.huhucapture.desktop"
        DESTINATION share/applications COMPONENT Runtime)
    install(FILES "${PROJECT_SOURCE_DIR}/assets/brand/huhucapture-icon.svg"
        DESTINATION share/icons/hicolor/scalable/apps RENAME io.github.NAMEWTA.huhucapture.svg COMPONENT Runtime)
    # PipeWire loads these at runtime, so ELF DT_NEEDED traversal alone cannot
    # discover them. Only the pinned client tree is included, never its daemon.
    pkg_get_variable(_huhu_pipewire_prefix libpipewire-0.3 prefix)
    foreach(_huhu_client_file IN ITEMS
        lib/pipewire-0.3/libpipewire-module-protocol-native.so
        lib/pipewire-0.3/libpipewire-module-client-node.so
        lib/pipewire-0.3/libpipewire-module-adapter.so
        lib/spa-0.2/support/libspa-support.so
        lib/spa-0.2/audioconvert/libspa-audioconvert.so
        lib/spa-0.2/videoconvert/libspa-videoconvert.so
        share/pipewire/client.conf)
        if(NOT EXISTS "${_huhu_pipewire_prefix}/${_huhu_client_file}")
            message(FATAL_ERROR "Missing required pinned PipeWire client runtime: ${_huhu_client_file}")
        endif()
        get_filename_component(_huhu_client_directory "${_huhu_client_file}" DIRECTORY)
        install(FILES "${_huhu_pipewire_prefix}/${_huhu_client_file}"
            DESTINATION "${_huhu_client_directory}" COMPONENT Runtime)
    endforeach()
endif()
