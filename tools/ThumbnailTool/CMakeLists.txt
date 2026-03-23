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
elseif(SM_USE_SYSTEM_FFMPEG OR SM_USE_SYSTEM_LIBS)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(TT_FF REQUIRED IMPORTED_TARGET
        libavformat libavcodec libavutil libswscale libswresample)
    set(TT_FFMPEG_LIBS PkgConfig::TT_FF)
    set(TT_FFMPEG_INCLUDES "")
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
set(TT_SOURCES
    src/main.cpp
    src/app.cpp
    # Shared utilities from parent
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/utils/thumbnail_generator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/utils/mkv_edit.cpp
)
if(WIN32)
    list(APPEND TT_SOURCES res/thumbnailtool.rc)
endif()

add_executable(ThumbnailTool ${TT_SOURCES})
if(WIN32)
    set_target_properties(ThumbnailTool PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

target_include_directories(ThumbnailTool PRIVATE
    src
    res
    ${TT_FFMPEG_INCLUDES}
    # Access parent's thumbnail_generator.h via "utils/" prefix
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
)

target_link_libraries(ThumbnailTool PRIVATE
    imgui_lib        # Parent's ImGui (GLFW + OpenGL3 backend)
    nlohmann_json::nlohmann_json  # Config persistence
    ${TT_FFMPEG_LIBS}
)

if(WIN32)
    # FFmpeg static dependencies (network, crypto, Media Foundation)
    target_link_libraries(ThumbnailTool PRIVATE
        ws2_32 bcrypt ncrypt crypt32 secur32
        mfuuid mfplat strmiids
        dwmapi shlwapi
    )
    target_compile_definitions(ThumbnailTool PRIVATE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
elseif(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(ThumbnailTool PRIVATE Threads::Threads)
endif()

# Copy to parent build output
add_custom_command(TARGET ThumbnailTool POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:ThumbnailTool>
        ${CMAKE_BINARY_DIR}/$<TARGET_FILE_NAME:ThumbnailTool>
    COMMENT "Copying ThumbnailTool to build root"
)
