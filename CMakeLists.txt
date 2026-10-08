cmake_minimum_required(VERSION 3.21)
project(CGPlay VERSION 1.0.7.12 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC OFF)

# ─── Output directories ───────────────────────────────────────────────────────
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# ─── Options ──────────────────────────────────────────────────────────────────
option(CGPLAY_USE_OCIO     "Enable OpenColorIO support"    ON)
option(CGPLAY_USE_EXR      "Enable OpenEXR support"        ON)
option(CGPLAY_USE_TLRENDER "Enable tlRender/full playback" OFF)

# Dependency locations are local configuration, not paths from the release machine.
# Command-line -D values take precedence over these environment/default values.
function(cgplay_dependency_path name fallback description)
    if(NOT "$ENV{${name}}" STREQUAL "")
        file(TO_CMAKE_PATH "$ENV{${name}}" _default)
    else()
        set(_default "${fallback}")
    endif()
    set(${name} "${_default}" CACHE PATH "${description}")
endfunction()

cgplay_dependency_path(TLRENDER_SOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../CGV/tlRender-main" "Path to tlRender source tree")
cgplay_dependency_path(TLRENDER_INSTALL_DIR
    "${TLRENDER_SOURCE_DIR}/install-Release" "tlRender dependency installation prefix")
cgplay_dependency_path(TLRENDER_BUILD_DIR
    "${TLRENDER_SOURCE_DIR}/build_tlr" "Existing tlRender build tree for legacy stub mode")
cgplay_dependency_path(FEATHER_TK_SOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../CGV/feather-tk-main" "Path to feather-tk source tree")
cgplay_dependency_path(FEATHER_TK_INSTALL_DIR "" "Optional feather-tk installation prefix")
cgplay_dependency_path(CGV_INSTALL_DIR "" "Optional additional dependency installation prefix")
cgplay_dependency_path(CGPLAY_VCPKG_LIBRARY_DIR ""
    "Optional vcpkg library directory, including the target triplet and lib directory")

# ─── Dependencies ─────────────────────────────────────────────────────────────
# OpenGL (system)
find_package(OpenGL REQUIRED)

# Qt6
find_package(Qt6 REQUIRED COMPONENTS
    Core Gui Widgets OpenGL OpenGLWidgets Concurrent Network)
find_package(Qt6 REQUIRED COMPONENTS WebEngineWidgets)

# ─── tlRender / feather-tk (optional, for full playback) ──────────────────────
if(CGPLAY_USE_TLRENDER)
    set(TLRENDER_SDL2 ON CACHE BOOL "" FORCE)
    set(TLRENDER_SDL3 OFF CACHE BOOL "" FORCE)
    set(TLRENDER_FFMPEG ON CACHE BOOL "" FORCE)
    set(TLRENDER_FFMPEG_PLUGIN ON CACHE BOOL "" FORCE)

    # tlRender reads CMAKE_INSTALL_PREFIX for FFmpeg .lib paths (no find_package)
    set(_saved_CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")
    set(CMAKE_INSTALL_PREFIX "${TLRENDER_INSTALL_DIR}")
    include_directories("${CMAKE_INSTALL_PREFIX}/include")

    find_package(Freetype CONFIG REQUIRED)
    find_package(minizip-ng CONFIG REQUIRED)

    # SDL2 alias for install-Release ftk cmake compatibility
    find_package(SDL2 CONFIG QUIET)
    if(NOT TARGET SDL2::SDL2-static AND TARGET SDL2::SDL2)
        add_library(SDL2::SDL2-static ALIAS SDL2::SDL2)
    elseif(NOT TARGET SDL2::SDL2-static)
        add_library(SDL2::SDL2-static INTERFACE IMPORTED)
    endif()

    find_package(ftk QUIET PATHS "${FEATHER_TK_INSTALL_DIR}" "${FEATHER_TK_SOURCE_DIR}/build_install")
    if(NOT ftk_FOUND)
        if(EXISTS "${FEATHER_TK_SOURCE_DIR}/CMakeLists.txt")
            message(STATUS "Adding feather-tk as subdirectory: ${FEATHER_TK_SOURCE_DIR}")
            add_subdirectory("${FEATHER_TK_SOURCE_DIR}" feather-tk EXCLUDE_FROM_ALL)
            set(ftk_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cmake" CACHE PATH "ftk config dir" FORCE)
        else()
            message(WARNING "feather-tk not found; set FEATHER_TK_SOURCE_DIR or install ftk.")
        endif()
    endif()

    # tlRender
    find_package(tlRender QUIET)
    if(NOT tlRender_FOUND)
        if(EXISTS "${TLRENDER_SOURCE_DIR}/CMakeLists.txt")
            message(STATUS "Adding tlRender as subdirectory: ${TLRENDER_SOURCE_DIR}")
            add_subdirectory("${TLRENDER_SOURCE_DIR}" tlRender EXCLUDE_FROM_ALL)
        else()
            message(WARNING "tlRender not found; set TLRENDER_SOURCE_DIR.")
        endif()
    endif()

    # Fix FFmpeg imported targets: tlRender doesn't set INTERFACE_INCLUDE_DIRECTORIES
    set(_ff_inc "${TLRENDER_INSTALL_DIR}/include")
    foreach(_ff_target FFmpeg::avutil FFmpeg::avcodec FFmpeg::avformat
                      FFmpeg::avdevice FFmpeg::swresample FFmpeg::swscale FFmpeg::FFmpeg)
        if(TARGET ${_ff_target})
            set_target_properties(${_ff_target} PROPERTIES
                INTERFACE_INCLUDE_DIRECTORIES "${_ff_inc}")
        endif()
    endforeach()

    set(CMAKE_INSTALL_PREFIX "${_saved_CMAKE_INSTALL_PREFIX}")
else()
    message(STATUS "tlRender disabled (stub mode). Set CGPLAY_USE_TLRENDER=ON for full playback.")
endif()

# OpenColorIO
if(CGPLAY_USE_OCIO)
    find_package(OpenColorIO QUIET)
    if(NOT OpenColorIO_FOUND)
        message(WARNING "OpenColorIO not found; OCIO features disabled.")
        set(CGPLAY_USE_OCIO OFF)
    endif()
endif()

# OpenEXR
if(CGPLAY_USE_EXR)
    find_package(OpenEXR QUIET)
    if(NOT OpenEXR_FOUND)
        message(WARNING "OpenEXR not found; will rely on tlRender's bundled EXR.")
    endif()
endif()

# ─── OpenTimelineIO (OTIO import/export) ──────────────────────────────────────
set(CGPLAY_USE_OTIO ON CACHE BOOL "Enable OpenTimelineIO support (import/export)")
if(CGPLAY_USE_OTIO)
    find_package(OpenTimelineIO QUIET PATHS "${CGV_INSTALL_DIR}" "${TLRENDER_INSTALL_DIR}")
    if(NOT OpenTimelineIO_FOUND)
        # Try to find directly
        find_path(OTIO_INCLUDE_DIR opentimelineio/timeline.h
            PATHS "${CGV_INSTALL_DIR}/include" "${TLRENDER_INSTALL_DIR}/include")
        find_library(OTIO_LIBRARY opentimelineio
            PATHS "${CGV_INSTALL_DIR}/lib" "${TLRENDER_INSTALL_DIR}/lib")
        if(OTIO_INCLUDE_DIR AND OTIO_LIBRARY)
            set(OpenTimelineIO_FOUND TRUE)
            set(OpenTimelineIO_INCLUDE_DIRS "${OTIO_INCLUDE_DIR}")
            set(OpenTimelineIO_LIBRARIES    "${OTIO_LIBRARY}")
            message(STATUS "OpenTimelineIO found: ${OTIO_LIBRARY}")
        else()
            message(WARNING "OpenTimelineIO not found; OTIO import/export disabled.")
            set(CGPLAY_USE_OTIO OFF)
        endif()
    endif()
endif()

# ─── Compile definitions ──────────────────────────────────────────────────────
add_compile_definitions(
    $<$<BOOL:${CGPLAY_USE_OCIO}>:CGPLAY_OCIO_ENABLED>
    $<$<BOOL:${CGPLAY_USE_EXR}>:CGPLAY_EXR_ENABLED>
    NOMINMAX   # Windows: suppress min/max macros
    FTK_API_GL_4_1  # Required by CGV_install ftk headers
)

# ─── Subdirectory ─────────────────────────────────────────────────────────────
add_subdirectory(src)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/player_function_smoke.py")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/player_function_smoke.py"
        "${CMAKE_BINARY_DIR}/player_function_smoke.py"
        COPYONLY)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/run_player_smoke_one_click.ps1")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/run_player_smoke_one_click.ps1"
        "${CMAKE_BINARY_DIR}/run_player_smoke_one_click.ps1"
        COPYONLY)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/run_player_smoke_one_click.bat")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/run_player_smoke_one_click.bat"
        "${CMAKE_BINARY_DIR}/run_player_smoke_one_click.bat"
        COPYONLY)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/一键测试播放器功能和快捷键.bat")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/一键测试播放器功能和快捷键.bat"
        "${CMAKE_BINARY_DIR}/一键测试播放器功能和快捷键.bat"
        COPYONLY)
endif()
