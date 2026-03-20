cmake_minimum_required(VERSION 3.20)
project(StreaMonitor VERSION 2.1.39 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ──────────────────────────────────────────────
# Platform detection
# ──────────────────────────────────────────────
if(WIN32)
    set(SM_PLATFORM "windows")
elseif(APPLE)
    set(SM_PLATFORM "macos")
else()
    set(SM_PLATFORM "linux")
endif()
message(STATUS "Building for platform: ${SM_PLATFORM}")

# ──────────────────────────────────────────────
# Options
# ──────────────────────────────────────────────
option(SM_BUILD_TESTS "Build unit tests" OFF)
option(SM_USE_SYSTEM_FFMPEG "Use system FFmpeg instead of vcpkg" OFF)
option(SM_USE_SYSTEM_LIBS "Use system libraries (for Linux package builds)" OFF)
option(SM_STATIC_LINK "Static link all dependencies (Windows only)" OFF)

# ──────────────────────────────────────────────
# Static linking setup (Windows)
# ──────────────────────────────────────────────
if(SM_STATIC_LINK AND WIN32)
    message(STATUS "Static linking enabled - using static CRT")
    # Use static vcpkg triplet
    if(DEFINED VCPKG_TARGET_TRIPLET)
        if(NOT VCPKG_TARGET_TRIPLET MATCHES "-static")
            message(WARNING "SM_STATIC_LINK is ON but VCPKG_TARGET_TRIPLET doesn't contain 'static'. "
                           "Consider using x64-windows-static or x64-windows-static-md")
        endif()
    endif()
endif()

# ──────────────────────────────────────────────
# vcpkg integration (auto-detect or manual)
# ──────────────────────────────────────────────
if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "Vcpkg toolchain file")
endif()

# ──────────────────────────────────────────────
# Dependencies
# ──────────────────────────────────────────────
find_package(PkgConfig QUIET)

# FFmpeg - Use vcpkg's CMake integration when available
find_package(FFMPEG CONFIG QUIET)
if(FFMPEG_FOUND)
    # vcpkg provides FFMPEG::avformat, etc.
    set(FFMPEG_LIBS ${FFMPEG_LIBRARIES})
    set(FFMPEG_INCLUDE_DIRS ${FFMPEG_INCLUDE_DIRS})
elseif(SM_USE_SYSTEM_FFMPEG OR SM_USE_SYSTEM_LIBS)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavformat libavcodec libavutil libswscale libswresample)
    set(FFMPEG_LIBS PkgConfig::FFMPEG)
else()
    find_path(AVFORMAT_INCLUDE_DIR libavformat/avformat.h)
    find_library(AVFORMAT_LIBRARY avformat)
    find_library(AVCODEC_LIBRARY avcodec)
    find_library(AVUTIL_LIBRARY avutil)
    find_library(SWSCALE_LIBRARY swscale)
    find_library(SWRESAMPLE_LIBRARY swresample)
    set(FFMPEG_LIBS ${AVFORMAT_LIBRARY} ${AVCODEC_LIBRARY} ${AVUTIL_LIBRARY}
        ${SWSCALE_LIBRARY} ${SWRESAMPLE_LIBRARY})
    # x264/x265 codec libs required by avcodec for static builds.
    # On MSVC with vcpkg, x265 is C++ and embeds CRT markers —
    # MUST link the correct debug/release version to avoid LNK2038.
    if(VCPKG_TARGET_TRIPLET AND MSVC)
        set(_VCPKG_LIB "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}")
        if(EXISTS "${_VCPKG_LIB}/lib/libx264.lib")
            list(APPEND FFMPEG_LIBS
                "$<IF:$<CONFIG:Debug>,${_VCPKG_LIB}/debug/lib/libx264.lib,${_VCPKG_LIB}/lib/libx264.lib>")
        endif()
        if(EXISTS "${_VCPKG_LIB}/lib/x265-static.lib")
            list(APPEND FFMPEG_LIBS
                "$<IF:$<CONFIG:Debug>,${_VCPKG_LIB}/debug/lib/x265-static.lib,${_VCPKG_LIB}/lib/x265-static.lib>")
        endif()
    else()
        find_library(X264_LIBRARY libx264)
        find_library(X265_LIBRARY x265-static)
        if(X264_LIBRARY)
            list(APPEND FFMPEG_LIBS ${X264_LIBRARY})
        endif()
        if(X265_LIBRARY)
            list(APPEND FFMPEG_LIBS ${X265_LIBRARY})
        endif()
    endif()
    set(FFMPEG_INCLUDE_DIRS ${AVFORMAT_INCLUDE_DIR})
endif()

# CURL for HTTP
find_package(CURL REQUIRED)

# OpenSSL
find_package(OpenSSL REQUIRED)

# Threads (required on Linux/macOS)
find_package(Threads REQUIRED)

# nlohmann/json
find_package(nlohmann_json CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    FetchContent_Declare(json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
    FetchContent_MakeAvailable(json)
endif()

# spdlog
find_package(spdlog CONFIG QUIET)
if(NOT spdlog_FOUND)
    include(FetchContent)
    FetchContent_Declare(spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG v1.13.0)
    FetchContent_MakeAvailable(spdlog)
endif()

# nghttp2 (HTTP/2 protocol library — replaces cpp-httplib for native h2 support)
find_package(unofficial-nghttp2 CONFIG QUIET)
if(NOT unofficial-nghttp2_FOUND)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(NGHTTP2 IMPORTED_TARGET libnghttp2)
    endif()
endif()

# ImGui + GLFW + OpenGL backend
include(FetchContent)

FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.8-docking)
FetchContent_MakeAvailable(imgui)

# Build ImGui as a library
add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui_lib PUBLIC glfw)
find_package(OpenGL REQUIRED)
target_link_libraries(imgui_lib PUBLIC OpenGL::GL)

# ──────────────────────────────────────────────
# Core engine library
# ──────────────────────────────────────────────
file(GLOB_RECURSE ENGINE_SOURCES
    src/core/*.cpp
    src/core/*.h
    src/sites/*.cpp
    src/sites/*.h
    src/downloaders/*.cpp
    src/downloaders/*.h
    src/net/*.cpp
    src/net/*.h
    src/config/*.cpp
    src/config/*.h
    src/web/*.cpp
    src/web/*.h
)

add_library(sm_engine STATIC ${ENGINE_SOURCES})
target_include_directories(sm_engine PUBLIC
    src
    ${FFMPEG_INCLUDE_DIRS}
)
target_link_libraries(sm_engine PUBLIC
    ${FFMPEG_LIBS}
    CURL::libcurl
    OpenSSL::SSL OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    Threads::Threads
)

# Link nghttp2
if(unofficial-nghttp2_FOUND)
    target_link_libraries(sm_engine PUBLIC unofficial::nghttp2::nghttp2)
elseif(NGHTTP2_FOUND)
    target_link_libraries(sm_engine PUBLIC PkgConfig::NGHTTP2)
else()
    find_library(NGHTTP2_LIB nghttp2)
    find_path(NGHTTP2_INCLUDE nghttp2/nghttp2.h)
    if(NGHTTP2_LIB AND NGHTTP2_INCLUDE)
        target_link_libraries(sm_engine PUBLIC ${NGHTTP2_LIB})
        target_include_directories(sm_engine PUBLIC ${NGHTTP2_INCLUDE})
    else()
        message(FATAL_ERROR "nghttp2 not found. Install via vcpkg: vcpkg install nghttp2")
    endif()
endif()

if(WIN32)
    target_link_libraries(sm_engine PUBLIC ws2_32 bcrypt iphlpapi shell32 ncrypt crypt32 mfuuid mfplat strmiids dbghelp psapi)
    target_compile_definitions(sm_engine PUBLIC
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
elseif(APPLE)
    # macOS-specific settings
    find_library(COCOA_FRAMEWORK Cocoa)
    find_library(IOKIT_FRAMEWORK IOKit)
    find_library(CORE_VIDEO_FRAMEWORK CoreVideo)
    target_link_libraries(sm_engine PUBLIC 
        ${COCOA_FRAMEWORK} ${IOKIT_FRAMEWORK} ${CORE_VIDEO_FRAMEWORK}
    )
else()
    # Linux-specific settings
    target_link_libraries(sm_engine PUBLIC dl)
endif()

# ──────────────────────────────────────────────
# Unified executable (GUI default, --cli for headless)
# ──────────────────────────────────────────────
file(GLOB_RECURSE GUI_SOURCES
    src/gui/*.cpp
    src/gui/*.h
)

# Single executable: src/main.cpp dispatches to guiMain / cliMain
if(WIN32)
    add_executable(StreaMonitor WIN32
        src/main.cpp
        src/main_gui.cpp
        src/main_cli.cpp
        ${GUI_SOURCES}
        src/resources/app.rc
    )
elseif(APPLE)
    add_executable(StreaMonitor MACOSX_BUNDLE
        src/main.cpp
        src/main_gui.cpp
        src/main_cli.cpp
        ${GUI_SOURCES}
    )
    set_target_properties(StreaMonitor PROPERTIES
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.streamonitor.app"
        MACOSX_BUNDLE_BUNDLE_NAME "StreaMonitor"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    )
else()
    add_executable(StreaMonitor
        src/main.cpp
        src/main_gui.cpp
        src/main_cli.cpp
        ${GUI_SOURCES}
    )
endif()

# Force whole-archive linking to include static site registrations
if(MSVC)
    target_link_options(StreaMonitor PRIVATE /WHOLEARCHIVE:sm_engine /ENTRY:mainCRTStartup)
    target_link_libraries(StreaMonitor PRIVATE sm_engine imgui_lib)
elseif(APPLE)
    target_link_libraries(StreaMonitor PRIVATE 
        -Wl,-force_load,$<TARGET_FILE:sm_engine>
        imgui_lib
    )
else()
    target_link_libraries(StreaMonitor PRIVATE 
        -Wl,--whole-archive sm_engine -Wl,--no-whole-archive
        imgui_lib
    )
endif()

# Include paths for GUI-only dependencies (miniaudio)
target_include_directories(StreaMonitor PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/miniaudio
)

# Platform-specific libraries
if(WIN32)
    target_link_libraries(StreaMonitor PRIVATE winhttp dwmapi)
endif()

# ──────────────────────────────────────────────
# Install
# ──────────────────────────────────────────────
install(TARGETS StreaMonitor
    RUNTIME DESTINATION bin)
