# Ombra: two executables.
# Build with: powershell -ExecutionPolicy Bypass -File build.ps1

cmake_minimum_required(VERSION 3.24)
project(ombra VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Ombra is Windows-only: WGC + NVENC + WASAPI.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# AddressSanitizer over everything, x264 included (`build.ps1 Asan` -> build\Asan). Run
# ombra-tests from there with the MSVC bin dir on PATH (clang_rt.asan_dynamic-x86_64.dll).
option(OMBRA_ASAN "Build with /fsanitize=address" OFF)
if(OMBRA_ASAN)
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/fsanitize=address$<SEMICOLON>/Zi>)
    add_link_options(/DEBUG /INCREMENTAL:NO)
endif()

# The CRT is linked statically, so a fresh Windows install runs the exes without the VC++
# redistributable (the installer and the portable zip ship no runtime DLLs). Asan keeps the DLL CRT
# its runtime expects.
if(NOT OMBRA_ASAN)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Release keeps symbols (a PDB beside each exe) so a minidump (crash.h) reads with function names.
# /Z7 puts debug info in the objects, so parallel Ninja compiles never contend for one PDB; /OPT:REF
# and /OPT:ICF keep the exe the size it is without /DEBUG. Asan sets its own /Zi.
if(NOT OMBRA_ASAN)
    add_compile_options($<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C,CXX>>:/Z7>)
    add_link_options($<$<CONFIG:Release>:/DEBUG> $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
endif()

# Settings both targets share. NOMINMAX and WIN32_LEAN_AND_MEAN are not optional — <windows.h>
# min/max macros collide with std::min/std::max throughout.
add_library(ombra_common INTERFACE)
target_compile_definitions(ombra_common INTERFACE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(ombra_common INTERFACE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /utf-8 /EHsc>)
target_include_directories(ombra_common INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

# Version and publisher: project VERSION above is the one place the version
# lives; the publisher is used for VERSIONINFO, the installer and the package
# manifests. version_defs.h is configure-time; build_info.h (the git commit) is rewritten at every
# build but only touched when it changes, so Ninja's restat recompiles version.cpp alone.
set(OMBRA_PUBLISHER "Zyrecx" CACHE STRING "Publisher name in VERSIONINFO and the installer")
set(OMBRA_SENTRY_DSN "" CACHE STRING "Sentry DSN for crash reports; release builds only")
configure_file(cmake/version_defs.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/ombra/version_defs.h" @ONLY)
set(OMBRA_BUILD_INFO "${CMAKE_CURRENT_BINARY_DIR}/generated/ombra/build_info.h")
add_custom_target(ombra_build_info
    COMMAND "${CMAKE_COMMAND}" -DSRC=${CMAKE_CURRENT_SOURCE_DIR} -DOUT=${OMBRA_BUILD_INFO}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_info.cmake"
    BYPRODUCTS "${OMBRA_BUILD_INFO}"
    COMMENT "Reading the git commit"
    VERBATIM)
# Shaders are compiled to headers at build time, so the resident engine never loads d3dcompiler.
# fxc ships with the Windows SDK; scripts/devenv.ps1 puts its x64 bin on PATH.
find_program(OMBRA_FXC fxc REQUIRED)
set(OMBRA_SHADER_OUT "${CMAKE_CURRENT_BINARY_DIR}/generated/ombra/shaders")
file(MAKE_DIRECTORY "${OMBRA_SHADER_OUT}")
set(OMBRA_SHADER_HEADERS)
foreach(stage IN ITEMS vs ps)
    string(TOUPPER "${stage}" STAGE)
    set(out "${OMBRA_SHADER_OUT}/scale_${stage}.h")
    add_custom_command(
        OUTPUT "${out}"
        COMMAND "${OMBRA_FXC}" /nologo /O3 /T ${stage}_4_0 /E ${STAGE}Main /Vn kScale${STAGE}
                /Fh "${out}" "${CMAKE_CURRENT_SOURCE_DIR}/src/ombra/shaders/scale.hlsl"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/ombra/shaders/scale.hlsl"
        COMMENT "fxc scale.hlsl (${stage})"
        VERBATIM)
    list(APPEND OMBRA_SHADER_HEADERS "${out}")
endforeach()
foreach(entry IN ITEMS Luma Chroma)
    set(out "${OMBRA_SHADER_OUT}/nv12_${entry}.h")
    add_custom_command(
        OUTPUT "${out}"
        COMMAND "${OMBRA_FXC}" /nologo /O3 /T ps_4_0 /E PS${entry} /Vn kNv12${entry}PS
                /Fh "${out}" "${CMAKE_CURRENT_SOURCE_DIR}/src/ombra/shaders/nv12.hlsl"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/ombra/shaders/nv12.hlsl"
        COMMENT "fxc nv12.hlsl (${entry})"
        VERBATIM)
    list(APPEND OMBRA_SHADER_HEADERS "${out}")
endforeach()

# --- engine: always resident, < 50 MB + ring ---------------------------------------------------
# WIN32 (not console-subsystem): the engine autostarts via a Run-key registry entry
# (reconfig.cpp's SetWindowsAutostart) and is meant to sit resident forever with just a tray icon —
# a console-subsystem exe holds a console window open the whole time it's resident (2026-09-05).
# Entry point is wWinMain (main.cpp); test modes (timedtest, jsontest, etc.) still print to an
# interactive terminal via AttachConsole(ATTACH_PARENT_PROCESS), same technique as ombra-ui.
# Everything but the engine wiring (engine.cpp) and the legacy test modes, so ombra-tests links the same code the engine runs.
add_library(ombra_core STATIC
    src/ombra/paths.cpp
    src/ombra/log.cpp
    src/ombra/crash.cpp
    src/ombra/crash_upload.cpp
    src/ombra/diagnostics.cpp
    src/ombra/sent_records.cpp
    src/ombra/zip_writer.cpp
    src/ombra/version.cpp
    src/ombra/util.cpp
    src/ombra/telemetry.cpp
    src/ombra/json.cpp
    src/ombra/pipe.cpp
    src/ombra/tray.cpp
    src/ombra/config.cpp
    src/ombra/setup_plan.cpp
    src/ombra/capture.cpp
    src/ombra/window_target.cpp
    src/ombra/games.cpp
    src/ombra/game_library.cpp
    src/ombra/encoder.cpp
    src/ombra/encode_target.cpp
    src/ombra/amf_encoder.cpp
    src/ombra/x264_encoder.cpp
    src/ombra/amf_probe.cpp
    src/ombra/gputopo.cpp
    src/ombra/gpuwatch.cpp
    src/ombra/capturebench.cpp
    src/ombra/pacer.cpp
    src/ombra/flight_recorder.cpp
    src/ombra/frame_scaler.cpp
    ${OMBRA_SHADER_HEADERS}
    src/ombra/supervisor.cpp
    src/ombra/controller.cpp
    src/ombra/hotkeys.cpp
    src/ombra/hotkey_spec.cpp
    src/ombra/screenshot.cpp
    src/ombra/ring.cpp
    src/ombra/av1.cpp
    src/ombra/clip_file.cpp
    src/ombra/clip_ops.cpp
    src/ombra/clip_reencode.cpp
    src/ombra/mux.cpp
    src/ombra/audio.cpp
    src/ombra/audio_tracks.cpp
    src/ombra/aac_encoder.cpp
    src/ombra/reconfig.cpp
    src/ombra/save.cpp
    src/ombra/update.cpp
    src/ombra/engine_pipe.cpp)
target_include_directories(ombra_core PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_dependencies(ombra_core ombra_build_info)
# The CPU encoder (x264_encoder.cpp). Static: its code pages are file-backed and only paged in
# when CPU mode runs, so it adds disk size, not resident memory.
add_subdirectory(third_party/x264)
target_link_libraries(ombra_core PUBLIC
    ombra_common
    x264
    d3d11 dxgi windowsapp user32 ole32 mfplat mfuuid mfreadwrite shell32 pdh winmm version windowscodecs
    advapi32 gdi32 dwmapi mmdevapi shcore winhttp bcrypt)

# The executables' icon, rendered at build time from the brand mark (src/ombra/brand_mark.h) so it
# can't drift from the tray and overlay. Both .rc files embed it through OBJECT_DEPENDS, which also
# makes Ninja rebuild the resources when the mark changes.
add_executable(ombra-icongen src/tools/icongen.cpp)
target_link_libraries(ombra-icongen PRIVATE ombra_common ole32 windowscodecs)
set(OMBRA_ICON "${CMAKE_CURRENT_BINARY_DIR}/generated/ombra/ombra.ico")
add_custom_command(
    OUTPUT "${OMBRA_ICON}"
    COMMAND ombra-icongen "${OMBRA_ICON}"
    DEPENDS ombra-icongen
    COMMENT "Rendering ombra.ico from the brand mark")
set_source_files_properties(src/ombra/ombra.rc src/ombra-ui/ombra-ui.rc PROPERTIES
    OBJECT_DEPENDS "${OMBRA_ICON};${CMAKE_CURRENT_SOURCE_DIR}/src/ombra/versioninfo.rc2")

# Dev tool: the engine's encoders on raw frames from stdin, for calibrating the quality presets
# (scripts/encbench.py). Not shipped.
add_executable(ombra-encbench src/tools/encbench.cpp)
target_link_libraries(ombra-encbench PRIVATE ombra_core)

add_executable(ombra WIN32
    src/ombra/main.cpp
    src/ombra/engine.cpp
    src/ombra/ombra.rc)
target_link_libraries(ombra PRIVATE ombra_core delayimp)
# Only the `update-check` child calls WinHTTP (update.h); delay-loaded so the resident engine never maps it.
target_link_options(ombra PRIVATE /DELAYLOAD:winhttp.dll)

# --- ui core: screens/widgets/pipe client, shared by ombra-ui and ombra-tests's headless UI tests -
# Split out so tests/ui_tests.cpp can link screens.cpp + widget.cpp + a fake PipeClient without
# pulling in main.cpp's wWinMain or overlay_window.cpp's concrete ScreenHost. paths/log/json/config
# are NOT re-listed here (ombra-ui gets them below, ombra-tests gets them from ombra_core already)
# to avoid duplicate symbols when ombra-tests links both ombra_core and ombra_ui_core.
add_library(ombra_ui_core STATIC
    src/ombra-ui/pipe_client.cpp
    src/ombra-ui/screens.cpp
    src/ombra-ui/audio_levels.cpp
    src/ombra-ui/clips_screen.cpp
    src/ombra-ui/setup_screen.cpp
    src/ombra-ui/clip_player.cpp
    src/ombra-ui/overlay_window.cpp
    src/ombra-ui/toast_window.cpp
    src/ombra-ui/ui/theme.cpp
    src/ombra-ui/ui/fonts.cpp
    src/ombra-ui/ui/color.cpp
    src/ombra-ui/ui/controls.cpp
    src/ombra-ui/ui/containers.cpp
    src/ombra-ui/ui/navigation.cpp
    src/ombra-ui/ui/motion.cpp
    src/ombra-ui/ui/draw_probe.cpp
    src/ombra-ui/ui/attached_panel.cpp
    src/ombra-ui/ui/flex.cpp
    src/ombra-ui/ui/disk_location.cpp
    src/ombra-ui/ui/file_icon.cpp
    src/ombra-ui/ui/widget.cpp)
target_link_libraries(ombra_ui_core PUBLIC
    ombra_common
    user32 shell32 d2d1 dxguid dwrite dcomp dxgi d3d11 windowscodecs ole32 shlwapi mfplat mfuuid)
# d2d1/dwrite/dcomp/dxgi/d3d11/windowscodecs: overlay_window.cpp + ui/*.cpp --
# the D3D11/DComp/Direct2D overlay and its offscreen-PNG test/snapshot path.
# dxguid: CLSID_D2D1Shadow, the menu popup's shadow (ui/controls.cpp).
# shlwapi: SHCreateMemStream, wrapping an in-memory SVG icon's XML bytes as an IStream for
# ID2D1DeviceContext5::CreateSvgDocument (widget.cpp's DrawIcon).
# settings_window.{h,cpp} is deleted -- gdi32/comctl32 dropped with it.

# --- behavior tests: console exe, doctest, never touches the real install -------------------------
# Headless tests run by default. GPU tests are opt-in (--gpu or OMBRA_TEST_GPU).
# Real audio recording is separately opt-in (--audio or OMBRA_TEST_AUDIO).
# Needs ffmpeg/ffprobe on PATH (or %OMBRA_FFMPEG_DIR%) to check saved files; missing them fails.
add_executable(ombra-tests
    tests/main.cpp
    tests/support/test_env.cpp
    tests/support/process.cpp
    tests/support/amf_caps.cpp
    tests/support/av1_fixture.cpp
    tests/support/h264_fixture.cpp
    tests/support/audio_analysis.cpp
    tests/support/media_probe.cpp
    tests/support/synthetic_capture.cpp
    tests/instance_tests.cpp
    tests/log_tests.cpp
    tests/crash_tests.cpp
    tests/flight_recorder_tests.cpp
    tests/crash_upload_tests.cpp
    tests/diagnostics_tests.cpp
    tests/gpu_encode_tests.cpp
    tests/pipeline_gpu_tests.cpp
    tests/av1_mux_tests.cpp
    tests/frame_scaler_tests.cpp
    tests/memory_soak_gpu_tests.cpp
    tests/clip_player_tests.cpp
    tests/save_tests.cpp
    tests/mux_tests.cpp
    tests/clip_file_tests.cpp
    tests/json_tests.cpp
    tests/update_tests.cpp
    tests/schema_tests.cpp
    tests/ring_tests.cpp
    tests/config_tests.cpp
    tests/setup_tests.cpp
    tests/hotkey_tests.cpp
    tests/aac_bench_tests.cpp
    tests/audio_tracks_tests.cpp
    tests/audio_tests.cpp
    tests/audio_format_tests.cpp
    tests/gputopo_tests.cpp
    tests/amf_encoder_tests.cpp
    tests/pacer_tests.cpp
    tests/encode_static_bench.cpp
    tests/reconfig_tests.cpp
    tests/engine_pipe_tests.cpp
    tests/audio_hw_tests.cpp
    tests/capture_gpu_tests.cpp
    tests/window_target_tests.cpp
    tests/games_tests.cpp
    tests/ui_tests.cpp
    tests/color_tests.cpp
    tests/controls_tests.cpp
    tests/motion_tests.cpp
    src/ombra-ui/fonts.rc)  # the UI tests render with the embedded fonts, as ombra-ui does
# The UI fonts (third_party/fonts): rc.exe finds RCDATA files on the include path, so the repo root
# goes on fonts.rc's, and the .ttf files are its dependencies.
set_source_files_properties(src/ombra-ui/fonts.rc PROPERTIES
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}"
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/inter/Inter-Regular.ttf;${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/inter/Inter-SemiBold.ttf;${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/geist/Geist-Regular.ttf;${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/geist/Geist-SemiBold.ttf")
target_include_directories(ombra-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
target_compile_definitions(ombra-tests PRIVATE
    OMBRA_TEST_FIXTURE_DIR=L"${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
target_link_libraries(ombra-tests PRIVATE ombra_core ombra_ui_core dbghelp)  # dbghelp: crash_tests reads the dumps
# shell32 supplies SHGetKnownFolderPath (log.cpp: %LOCALAPPDATA%\Ombra\logs).
# WASAPI (mmdeviceapi.h/audioclient.h) is header-only COM; ole32 supplies CoCreateInstance /
# CoInitializeEx / CoTaskMemFree, no separate import lib for the audio interfaces themselves.
# mmdevapi supplies ActivateAudioInterfaceAsync (audio.cpp's process-loopback path,
# unlike the rest of WASAPI this one is a plain
# exported DLL function, not a COM interface reached via CoCreateInstance, so it does need an
# explicit import lib.
# mfplat supplies MFStartup/MFCreateMediaType/MFCreateSample/MFCreateMemoryBuffer; mfuuid supplies
# the MF_MT_*/MFMediaType_*/MFAudioFormat_* GUID definitions those calls reference.
# nvEncodeAPI64.dll is loaded with LoadLibrary at runtime — no import library on purpose.
# winmm supplies timeBeginPeriod/timeEndPeriod, the pacer's fallback path when the high-resolution
# waitable timer flag is not supported (pre-Windows 10 1803) -- pacer.cpp.
# version supplies GetFileVersionInfoSizeW/GetFileVersionInfoW/VerQueryValueW — controller.cpp's
# per-game-folder name resolution.
# windowscodecs supplies WIC (screenshot.cpp's PNG encode/decode) -- an OS API,
# not a vendored PNG library, same posture as WASAPI/Media Foundation elsewhere in this list.
# advapi32 supplies ConvertStringSecurityDescriptorToSecurityDescriptorW (pipe.cpp's owner-only
# DACL).
# gdi32 supplies CreateDIBSection/CreateIconIndirect: tray.cpp renders the brand mark in the
# accent colour into the tray HICON.

# --- ui: spawned on demand, exits on close -----------------------------------------------------
# WIN32 (not console-subsystem): ombra-ui has a real top-level window, and a
# console-subsystem exe flashes a console momentarily on every launch (Explorer double-click, the
# tray's "Open Settings"). Entry point is wWinMain (main.cpp); --selftest still works from an
# interactive terminal (main.cpp's RunUi() comment explains why).
add_executable(ombra-ui WIN32
    src/ombra-ui/main.cpp
    src/ombra-ui/driver.cpp
    src/ombra/paths.cpp
    src/ombra/log.cpp
    src/ombra/crash.cpp
    src/ombra/flight_recorder.cpp
    src/ombra/diagnostics.cpp
    src/ombra/sent_records.cpp
    src/ombra/zip_writer.cpp
    src/ombra/util.cpp
    src/ombra/version.cpp
    src/ombra/json.cpp
    src/ombra/config.cpp
    src/ombra/setup_plan.cpp
    src/ombra/hotkey_spec.cpp
    src/ombra/window_target.cpp
    src/ombra/games.cpp
    src/ombra/clip_ops.cpp
    src/ombra/clip_file.cpp
    src/ombra/mux.cpp
    src/ombra/av1.cpp
    src/ombra-ui/ombra-ui.rc
    src/ombra-ui/fonts.rc)
target_link_libraries(ombra-ui PRIVATE ombra_ui_core dwmapi)
add_dependencies(ombra-ui ombra_build_info)
# The generated icon (ombra/ombra.ico) for ombra-ui.rc; ombra gets this dir from ombra_core.
target_include_directories(ombra-ui PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
# dwmapi: window_target.cpp's snapshot skips cloaked windows (DwmGetWindowAttribute).
# log.cpp: ombra-ui/main.cpp and pipe_client.cpp both call into Log:: directly.
# json.cpp is shared with ombra: the pipe protocol speaks the same JSON DOM
# config.json already uses, so ombra-ui's pipe client links the same module rather than a second
# hand-rolled parser.
# config.cpp: step 6's first-run flow needs ConfigDirW() (the first-run marker lives alongside
# config.json) and AllSettings() (--selftest's get_schema/get_config ground truth). Its only
# external dependency is SHGetKnownFolderPath, already covered by shell32 above.
# clip_file.cpp + mux.cpp + av1.cpp: the Clips screen's keyframe trim writes through the same muxer
# a save does.
# paths.cpp: the instance's pipe/mutex names and folders (OMBRA_HOME). Deliberately NOT linking
# src/ombra/pipe.cpp: ombra-ui is a pipe client, never a PipeServer.
