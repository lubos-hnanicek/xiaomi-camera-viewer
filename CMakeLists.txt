cmake_minimum_required(VERSION 3.25)

project(XiaomiViewer
    VERSION 0.1.0
    DESCRIPTION "Native Windows viewer and controller for Xiaomi CW400/CW500 cameras"
    LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "XiaomiViewer targets Windows only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(THIRD_PARTY "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

if(NOT EXISTS "${THIRD_PARTY}/imgui/imgui.h")
    message(FATAL_ERROR "Dependencies are missing. Run scripts/fetch-deps.ps1 first.")
endif()
if(NOT EXISTS "${THIRD_PARTY}/ffmpeg/include/libavcodec/avcodec.h")
    message(FATAL_ERROR "FFmpeg is missing. Run scripts/fetch-deps.ps1 first.")
endif()

# --- Dear ImGui -------------------------------------------------------------
add_library(imgui STATIC
    "${THIRD_PARTY}/imgui/imgui.cpp"
    "${THIRD_PARTY}/imgui/imgui_draw.cpp"
    "${THIRD_PARTY}/imgui/imgui_tables.cpp"
    "${THIRD_PARTY}/imgui/imgui_widgets.cpp"
    "${THIRD_PARTY}/imgui/backends/imgui_impl_win32.cpp"
    "${THIRD_PARTY}/imgui/backends/imgui_impl_dx11.cpp")

target_include_directories(imgui SYSTEM PUBLIC
    "${THIRD_PARTY}/imgui"
    "${THIRD_PARTY}/imgui/backends")

target_compile_definitions(imgui PUBLIC
    IMGUI_DISABLE_OBSOLETE_FUNCTIONS
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX)

target_link_libraries(imgui PUBLIC d3d11 dxgi d3dcompiler dwmapi)

# --- FFmpeg (prebuilt LGPL shared) ------------------------------------------
add_library(ffmpeg INTERFACE)
target_include_directories(ffmpeg SYSTEM INTERFACE "${THIRD_PARTY}/ffmpeg/include")
target_link_libraries(ffmpeg INTERFACE
    "${THIRD_PARTY}/ffmpeg/lib/avcodec.lib"
    "${THIRD_PARTY}/ffmpeg/lib/avformat.lib"  # the Matroska muxer, for recording
    "${THIRD_PARTY}/ffmpeg/lib/avutil.lib"
    "${THIRD_PARTY}/ffmpeg/lib/swresample.lib")

# --- nlohmann/json ----------------------------------------------------------
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json SYSTEM INTERFACE "${THIRD_PARTY}")

# --- Go bridge DLL ----------------------------------------------------------
# cgo cannot use MSVC, so the bridge is built by its own script with mingw-w64
# and consumed here as a prebuilt import library.
set(BRIDGE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/bridge")
set(BRIDGE_OUT "${CMAKE_CURRENT_SOURCE_DIR}/build/bridge")
set(BRIDGE_DLL "${BRIDGE_OUT}/xmbridge.dll")

# Every Go source is a dependency, not just the cgo entry point: listing only
# bridge.go silently skips the rebuild when the protocol code underneath changes.
file(GLOB_RECURSE BRIDGE_SOURCES CONFIGURE_DEPENDS "${BRIDGE_DIR}/*.go")
list(APPEND BRIDGE_SOURCES
    "${BRIDGE_DIR}/go.mod"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/xmbridge_types.h")

add_custom_command(
    OUTPUT "${BRIDGE_DLL}"
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/scripts/build-bridge.ps1"
            -OutputDir "${BRIDGE_OUT}"
    WORKING_DIRECTORY "${BRIDGE_DIR}"
    DEPENDS ${BRIDGE_SOURCES}
    COMMENT "Building Go bridge DLL (xmbridge.dll)"
    VERBATIM)

add_custom_target(bridge DEPENDS "${BRIDGE_DLL}")

# --- Application ------------------------------------------------------------
add_executable(XiaomiViewer WIN32
    src/main.cpp
    src/app/App.cpp
    src/app/Frameless.cpp
    src/app/Log.cpp
    src/app/SingleInstance.cpp
    src/app/Theme.cpp
    src/bridge/Bridge.cpp
    src/cloud/Miot.cpp
    src/config/Config.cpp
    src/media/Recorder.cpp
    src/media/StreamWorker.cpp
    src/media/VideoDecoder.cpp
    src/render/D3D11Context.cpp
    src/render/VideoFrameTexture.cpp
    src/ui/CamerasView.cpp
    src/ui/GridView.cpp
    src/ui/LoginView.cpp
    src/ui/PtzOverlay.cpp
    src/ui/SettingsView.cpp
    src/util/Encoding.cpp
    src/app/app.manifest)

add_dependencies(XiaomiViewer bridge)

target_include_directories(XiaomiViewer PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(XiaomiViewer PRIVATE
    imgui
    ffmpeg
    nlohmann_json
    crypt32        # DPAPI, for the account token at rest
    windowscodecs  # WIC, to decode the sign-in captcha
    shlwapi
    shell32)

target_compile_definitions(XiaomiViewer PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    XV_VERSION="${PROJECT_VERSION}")

if(MSVC)
    # No /MP here: Ninja already runs one cl.exe per source, and adding /MP on
    # top multiplies the process count.
    # /external:W0 keeps /W4 meaningful: FFmpeg's headers alone emit dozens of
    # conversion warnings that would otherwise bury our own.
    target_compile_options(XiaomiViewer PRIVATE /W4 /permissive- /utf-8 /external:W0)
    target_compile_options(imgui PRIVATE /utf-8 /external:W0)
    # The app is a windowed process but keeps a console attach path for -console.
    target_link_options(XiaomiViewer PRIVATE /SUBSYSTEM:WINDOWS)
endif()

# --- Runtime layout ---------------------------------------------------------
# Everything the exe needs at runtime lands next to it so the build directory is
# directly runnable and can be zipped as-is.
#
# This is a target of its own rather than a POST_BUILD step, because POST_BUILD
# only fires when the exe relinks: a rebuilt xmbridge.dll would otherwise never
# reach the output directory and the app would keep loading a stale bridge.
set(STAGE_COMMANDS
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${BRIDGE_DLL}" "$<TARGET_FILE_DIR:XiaomiViewer>")

foreach(dll avcodec-62 avformat-62 avutil-60 swresample-6)
    list(APPEND STAGE_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${THIRD_PARTY}/ffmpeg/bin/${dll}.dll" "$<TARGET_FILE_DIR:XiaomiViewer>")
endforeach()

add_custom_target(stage_runtime ALL
    ${STAGE_COMMANDS}
    DEPENDS "${BRIDGE_DLL}"
    COMMENT "Staging runtime dependencies")

add_dependencies(stage_runtime XiaomiViewer)
