cmake_minimum_required(VERSION 3.24)
project(rfg_vr C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "rfg.exe is 32-bit: configure with -A Win32")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(safetyhook
    GIT_REPOSITORY https://github.com/cursey/safetyhook.git
    GIT_TAG v0.7.0
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(safetyhook)

# OpenXR static loader; it finds the 32-bit runtime via HKLM\SOFTWARE\WOW6432Node\Khronos\OpenXR.
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
set(BUILD_WITH_STATIC_CRT ON CACHE BOOL "" FORCE)
FetchContent_Declare(openxr
    GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
    GIT_TAG release-1.1.63
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(openxr)
# The static loader hard-codes /MD; force it onto the static CRT used by everything else.
set_target_properties(openxr_loader PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(rfg_vr SHARED
    src/dllmain.cpp
    src/game.cpp
    src/config.cpp
    src/controllers.cpp
    src/log.cpp
    src/d3d11_hook.cpp
    src/xr.cpp
    src/camera_hook.cpp
    src/hud.cpp
    src/mouse.cpp
    src/gamestate.cpp
    src/input.cpp
    src/dinput8.def)
set_target_properties(rfg_vr PROPERTIES OUTPUT_NAME dinput8)
target_compile_features(rfg_vr PRIVATE cxx_std_23)
target_compile_definitions(rfg_vr PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(rfg_vr PRIVATE /W4 /permissive- /utf-8 /Zi)
target_link_options(rfg_vr PRIVATE /DEBUG /OPT:REF /OPT:ICF)
target_link_libraries(rfg_vr PRIVATE safetyhook::safetyhook openxr_loader d3d11 dxgi)

# Developer tools, not part of releases: autostart into the newest save, intro video skip.
option(RFGVR_DEV "Build the developer tools (autostart)" OFF)
if(RFGVR_DEV)
    target_sources(rfg_vr PRIVATE src/autostart.cpp src/video_hook.cpp src/iat.cpp)
    target_compile_definitions(rfg_vr PRIVATE RFGVR_DEV)
endif()

# Opt-in: copy the built proxy into the game folder (game/ is a disposable copy).
set(RFGVR_GAME_DIR "${CMAKE_SOURCE_DIR}/../game" CACHE PATH "Game install to deploy into")
option(RFGVR_DEPLOY "Copy dinput8.dll/.pdb into RFGVR_GAME_DIR after build" OFF)
if(RFGVR_DEPLOY)
    add_custom_command(TARGET rfg_vr POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:rfg_vr> $<TARGET_PDB_FILE:rfg_vr> "${RFGVR_GAME_DIR}")
endif()
