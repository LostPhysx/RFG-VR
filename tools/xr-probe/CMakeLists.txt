cmake_minimum_required(VERSION 3.20)
project(xr_probe CXX C)

# Match the planned mod: static CRT, x86 (configure with -A Win32).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
set(BUILD_WITH_STATIC_CRT ON CACHE BOOL "" FORCE)
FetchContent_Declare(openxr
    GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
    GIT_TAG release-1.1.63
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(openxr)
# The static loader hard-codes /MD; force it to the static CRT we use everywhere.
set_target_properties(openxr_loader PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(xr-probe main.cpp)
target_compile_features(xr-probe PRIVATE cxx_std_20)
target_link_libraries(xr-probe PRIVATE openxr_loader d3d11 dxgi d3dcompiler)
# rfg.exe is LARGE_ADDRESS_AWARE; mirror that so address-space numbers are comparable.
target_link_options(xr-probe PRIVATE /LARGEADDRESSAWARE)
