cmake_minimum_required(VERSION 3.20)
project(Sorter VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ──────────────────────────────────────────────
# Dependencies — reuse parent project's targets:
#   imgui_lib  (ImGui + GLFW + OpenGL3)
#   glfw       (from parent's FetchContent)
#   OpenGL::GL (from parent)
#   nlohmann_json (from parent's vcpkg)
# ──────────────────────────────────────────────

# ──────────────────────────────────────────────
# Sorter executable
# ──────────────────────────────────────────────
set(SORTER_SOURCES
    src/main.cpp
    src/app.cpp
    src/sorter.cpp
)
if(WIN32)
    list(APPEND SORTER_SOURCES res/sorter.rc)
endif()

add_executable(Sorter ${SORTER_SOURCES})
if(WIN32)
    set_target_properties(Sorter PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

target_include_directories(Sorter PRIVATE
    src
    res
)

target_link_libraries(Sorter PRIVATE
    imgui_lib                     # Parent's ImGui (GLFW + OpenGL3 backend)
    nlohmann_json::nlohmann_json  # Config loading
)

if(WIN32)
    target_link_libraries(Sorter PRIVATE dwmapi shlwapi)
    target_compile_definitions(Sorter PRIVATE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
elseif(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(Sorter PRIVATE Threads::Threads)
endif()

# Copy to parent build output
add_custom_command(TARGET Sorter POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:Sorter>
        ${CMAKE_BINARY_DIR}/$<TARGET_FILE_NAME:Sorter>
    COMMENT "Copying Sorter to build root"
)
