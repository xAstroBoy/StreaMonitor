cmake_minimum_required(VERSION 3.20)
project(StripHelper VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ──────────────────────────────────────────────
# Dependencies (shared with parent StreaMonitor project via vcpkg)
# ──────────────────────────────────────────────

# nlohmann/json
find_package(nlohmann_json CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    FetchContent_Declare(json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
    FetchContent_MakeAvailable(json)
endif()

# ImGui — fetch imgui + DX11 backend (Windows only)
include(FetchContent)
FetchContent_Declare(imgui_sh
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.8-docking)
FetchContent_MakeAvailable(imgui_sh)

# Build ImGui as a static library with Win32+DX11 backend
add_library(imgui_sh_lib STATIC
    ${imgui_sh_SOURCE_DIR}/imgui.cpp
    ${imgui_sh_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_sh_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_sh_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_sh_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_sh_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_sh_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui_sh_lib PUBLIC
    ${imgui_sh_SOURCE_DIR}
    ${imgui_sh_SOURCE_DIR}/backends
)

# FFmpeg — shared with parent vcpkg
find_package(FFMPEG CONFIG QUIET)
if(FFMPEG_FOUND)
    set(SH_FFMPEG_LIBS ${FFMPEG_LIBRARIES})
    set(SH_FFMPEG_INCLUDES ${FFMPEG_INCLUDE_DIRS})
else()
    find_path(AVFORMAT_INCLUDE_DIR libavformat/avformat.h)
    find_library(AVFORMAT_LIBRARY avformat)
    find_library(AVCODEC_LIBRARY avcodec)
    find_library(AVUTIL_LIBRARY avutil)
    find_library(SWSCALE_LIBRARY swscale)
    find_library(SWRESAMPLE_LIBRARY swresample)
    set(SH_FFMPEG_LIBS ${AVFORMAT_LIBRARY} ${AVCODEC_LIBRARY} ${AVUTIL_LIBRARY}
        ${SWSCALE_LIBRARY} ${SWRESAMPLE_LIBRARY})

    # x264/x265 for static builds
    if(VCPKG_TARGET_TRIPLET AND MSVC)
        set(_VCPKG_LIB "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}")
        if(EXISTS "${_VCPKG_LIB}/lib/libx264.lib")
            list(APPEND SH_FFMPEG_LIBS
                "$<IF:$<CONFIG:Debug>,${_VCPKG_LIB}/debug/lib/libx264.lib,${_VCPKG_LIB}/lib/libx264.lib>")
        endif()
        if(EXISTS "${_VCPKG_LIB}/lib/x265-static.lib")
            list(APPEND SH_FFMPEG_LIBS
                "$<IF:$<CONFIG:Debug>,${_VCPKG_LIB}/debug/lib/x265-static.lib,${_VCPKG_LIB}/lib/x265-static.lib>")
        endif()
    else()
        find_library(X264_LIBRARY libx264)
        find_library(X265_LIBRARY x265-static)
        if(X264_LIBRARY)
            list(APPEND SH_FFMPEG_LIBS ${X264_LIBRARY})
        endif()
        if(X265_LIBRARY)
            list(APPEND SH_FFMPEG_LIBS ${X265_LIBRARY})
        endif()
    endif()
    set(SH_FFMPEG_INCLUDES ${AVFORMAT_INCLUDE_DIR})
endif()

# ──────────────────────────────────────────────
# StripHelper executable
# ──────────────────────────────────────────────
add_executable(StripHelper WIN32
    src/main.cpp
    src/app.cpp
    src/pipeline.cpp
    src/subprocess.cpp
    src/native_ffmpeg.cpp
    src/shell_integration.cpp
    # Shared thumbnail generator from parent
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/utils/thumbnail_generator.cpp
    # Windows application icon
    res/striphelper.rc
)

target_include_directories(StripHelper PRIVATE
    src
    res
    ${SH_FFMPEG_INCLUDES}
    # Access parent's thumbnail_generator.h via "utils/" prefix
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
)

target_link_libraries(StripHelper PRIVATE
    imgui_sh_lib
    nlohmann_json::nlohmann_json
    ${SH_FFMPEG_LIBS}
    d3d11
    dxgi
    ole32
    shlwapi
    advapi32
    # FFmpeg static dependencies (network, crypto, Media Foundation)
    ws2_32
    bcrypt
    ncrypt
    crypt32
    secur32
    mfuuid
    mfplat
    strmiids
)

target_compile_definitions(StripHelper PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
)

# Copy to parent build output
add_custom_command(TARGET StripHelper POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:StripHelper>
        ${CMAKE_BINARY_DIR}/StripHelper.exe
    COMMENT "Copying StripHelper.exe to build root"
)
