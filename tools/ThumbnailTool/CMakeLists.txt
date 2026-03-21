cmake_minimum_required(VERSION 3.20)
project(ThumbnailTool VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ──────────────────────────────────────────────
# Dependencies — reuse parent project's targets:
#   imgui_lib  (ImGui + GLFW + OpenGL3)
#   glfw       (from parent's FetchContent)
#   OpenGL::GL (from parent)
# FFmpeg is found via parent's vcpkg
# ──────────────────────────────────────────────

# FFmpeg
find_package(FFMPEG CONFIG QUIET)
if(FFMPEG_FOUND)
    set(TT_FFMPEG_LIBS ${FFMPEG_LIBRARIES})
    set(TT_FFMPEG_INCLUDES ${FFMPEG_INCLUDE_DIRS})
else()
    find_path(AVFORMAT_INCLUDE_DIR libavformat/avformat.h)
    find_library(AVFORMAT_LIBRARY avformat)
    find_library(AVCODEC_LIBRARY avcodec)
    find_library(AVUTIL_LIBRARY avutil)
    find_library(SWSCALE_LIBRARY swscale)
    find_library(SWRESAMPLE_LIBRARY swresample)
    set(TT_FFMPEG_LIBS ${AVFORMAT_LIBRARY} ${AVCODEC_LIBRARY} ${AVUTIL_LIBRARY}
        ${SWSCALE_LIBRARY} ${SWRESAMPLE_LIBRARY})
    set(TT_FFMPEG_INCLUDES ${AVFORMAT_INCLUDE_DIR})
endif()

# x264/x265 for static builds
if(VCPKG_TARGET_TRIPLET AND MSVC)
    set(_VCPKG_LIB "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}")
    if(EXISTS "${_VCPKG_LIB}/lib/libx264.lib")
        list(APPEND TT_FFMPEG_LIBS
            "$<IF:$<CONFIG:Debug>,${_VCPKG_LIB}/debug/lib/libx264.lib,${_VCPKG_LIB}/lib/libx264.lib>")
    endif()
    if(EXISTS "${_VCPKG_LIB}/lib/x265-static.lib")
        list(APPEND TT_FFMPEG_LIBS
            "$<IF:$<CONFIG:Debug>,${_VCPKG_LIB}/debug/lib/x265-static.lib,${_VCPKG_LIB}/lib/x265-static.lib>")
    endif()
endif()

# ──────────────────────────────────────────────
# ThumbnailTool executable
# ──────────────────────────────────────────────
add_executable(ThumbnailTool WIN32
    src/main.cpp
    src/app.cpp
    # Shared thumbnail generator from parent
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/utils/thumbnail_generator.cpp
)

target_include_directories(ThumbnailTool PRIVATE
    src
    ${TT_FFMPEG_INCLUDES}
    # Access parent's thumbnail_generator.h via "utils/" prefix
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
)

target_link_libraries(ThumbnailTool PRIVATE
    imgui_lib        # Parent's ImGui (GLFW + OpenGL3 backend)
    ${TT_FFMPEG_LIBS}
    # FFmpeg static dependencies (network, crypto, Media Foundation)
    ws2_32
    bcrypt
    ncrypt
    crypt32
    secur32
    mfuuid
    mfplat
    strmiids
    dwmapi          # DwmSetWindowAttribute for dark title bar
    shlwapi
)

target_compile_definitions(ThumbnailTool PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

# Copy to parent build output
add_custom_command(TARGET ThumbnailTool POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:ThumbnailTool>
        ${CMAKE_BINARY_DIR}/ThumbnailTool.exe
    COMMENT "Copying ThumbnailTool.exe to build root"
)
