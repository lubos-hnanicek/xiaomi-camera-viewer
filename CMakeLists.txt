cmake_minimum_required(VERSION 3.25)

project(XiaomiViewer
    VERSION 0.4.3
    DESCRIPTION "Native Windows viewer and controller for Xiaomi CW400/CW500 cameras"
    LANGUAGES CXX RC)

if(NOT WIN32)
    message(FATAL_ERROR "XiaomiViewer targets Windows only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static CRT, because the app ships as a zip that is unpacked and run: a missing
# or too-old Visual C++ redistributable would otherwise stop it starting, and
# the toolset is new enough that "some redistributable is installed" is not
# enough. Safe because no CRT object crosses a module boundary -- the bridge ABI
# passes only caller-owned buffers, and FFmpeg memory is always released through
# FFmpeg's own allocators -- so the private heap is never shared.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# --- Version and publisher metadata -----------------------------------------
# Keep the project version above as the only hand-written version in the tree,
# and feed it to the executable, manifest and Go bridge so Explorer and crash
# reports identify every binary in a release consistently.
set(XV_PRODUCT_NAME "Xiaomi Camera Viewer")
set(XV_PUBLISHER "Xiaomi Camera Viewer contributors")
set(XV_COPYRIGHT "Copyright (c) 2026 ${XV_PUBLISHER}")
set(XV_VERSION_FULL "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}.0")

set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(VERSION_HEADER "${GENERATED_DIR}/app/Version.h")
set(APP_MANIFEST "${GENERATED_DIR}/app.manifest")

set(BRIDGE_VERSION_RC "${GENERATED_DIR}/xmbridge.rc")

configure_file(cmake/Version.h.in "${VERSION_HEADER}" @ONLY)
configure_file(src/app/app.manifest.in "${APP_MANIFEST}" @ONLY)
configure_file(bridge/versioninfo.rc.in "${BRIDGE_VERSION_RC}" @ONLY)

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
    "${BRIDGE_VERSION_RC}"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/xmbridge_types.h")

add_custom_command(
    OUTPUT "${BRIDGE_DLL}"
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/scripts/build-bridge.ps1"
            -OutputDir "${BRIDGE_OUT}"
            -Version "${PROJECT_VERSION}"
            -VersionInfoRc "${BRIDGE_VERSION_RC}"
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
    src/media/AudioDecoder.cpp
    src/media/AudioFormat.cpp
    src/media/AudioPlayer.cpp
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
    "${APP_MANIFEST}"
    src/app/app.rc)

add_dependencies(XiaomiViewer bridge)

# rc.exe cannot report what a script pulled in, so the icon and the generated
# version header have to be named as dependencies by hand, or a regenerated icon
# and a bumped version never reach the executable.
set_source_files_properties(src/app/app.rc PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/app/XiaomiViewer.ico;${VERSION_HEADER}")

target_include_directories(XiaomiViewer PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${GENERATED_DIR}")

target_link_libraries(XiaomiViewer PRIVATE
    imgui
    ffmpeg
    nlohmann_json
    crypt32        # DPAPI, for the account token at rest
    windowscodecs  # WIC, to decode the sign-in captcha
    ole32          # WASAPI, to play the camera's audio
    shlwapi
    shell32)

target_compile_definitions(XiaomiViewer PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX)

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
