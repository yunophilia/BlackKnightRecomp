cmake_minimum_required(VERSION 3.21)

# Build with the presets in CMakePresets.json: clang-cl + Ninja on Windows (MSVC also
# works), clang + Ninja on Linux. The runtime is Windows-only (D3D11, XAudio2, XInput),
# so other platforms only build the recompiled game code for now.
project(BlackKnightRecomp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

if(NOT EXISTS ${CMAKE_SOURCE_DIR}/generated/sources.cmake)
    message(FATAL_ERROR "Run tools/build_generated.py first to generate the recompiled sources")
endif()
include(${CMAKE_SOURCE_DIR}/generated/sources.cmake)

# MSVC_FRONTEND: cl or clang-cl (MSVC-style flags); CLANG: clang-cl or clang
set(MSVC_FRONTEND ${MSVC})
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set(CLANG ON)
endif()

# Visual Studio 2026 is recommended; 2022 (MSVC 19.30) is the oldest supported. clang-cl
# reports the version of the Visual Studio libraries it found.
if(MSVC_FRONTEND)
    if(CLANG)
        set(VS_LIB_VERSION ${CMAKE_CXX_SIMULATE_VERSION})
    else()
        set(VS_LIB_VERSION ${CMAKE_CXX_COMPILER_VERSION})
    endif()
    if(VS_LIB_VERSION VERSION_LESS 19.30)
        message(FATAL_ERROR "Visual Studio 2022 or newer is required (found MSVC ${VS_LIB_VERSION}); "
                            "install Visual Studio 2026 (Build Tools are enough) with the C++ workload")
    endif()
endif()

# ---------------------------------------------------------------- recompiled game code
add_library(ppcrecomp OBJECT ${PPC_RECOMP_SOURCES})
target_include_directories(ppcrecomp PUBLIC runtime/cpu generated)
if(MSVC_FRONTEND)
    # always optimized, also in Debug: unoptimized recompiled code is far too slow
    target_compile_options(ppcrecomp PRIVATE /O2 /Ob2 /GS- /EHs-c- /std:c++latest)
    if(CLANG)
        target_compile_options(ppcrecomp PRIVATE -Wno-unused-label -Wno-unused-variable)
    else()
        target_compile_options(ppcrecomp PRIVATE /bigobj /wd4102 /wd4244 /wd4146 /wd4018)
    endif()
else()
    target_compile_options(ppcrecomp PRIVATE -O2 -fno-exceptions -fno-strict-aliasing -w)
endif()

if(NOT WIN32)
    message(STATUS "The runtime is Windows-only: building the recompiled code (ppcrecomp) only")
    return()
endif()

# ---------------------------------------------------------------- runtime
file(GLOB_RECURSE RUNTIME_SOURCES CONFIGURE_DEPENDS runtime/*.cpp)
add_executable(BlackKnightRecomp ${RUNTIME_SOURCES})
target_include_directories(BlackKnightRecomp PRIVATE runtime runtime/cpu generated)
target_sources(BlackKnightRecomp PRIVATE $<TARGET_OBJECTS:ppcrecomp>)
target_link_libraries(BlackKnightRecomp PRIVATE d3d11 dxgi d3dcompiler xinput winmm ole32 xaudio2 dbghelp)
# the scripts in tools/ and the shader cache expect the game in out/
set_target_properties(BlackKnightRecomp PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/out
                                                   RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_SOURCE_DIR}/out
                                                   RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/out)
target_compile_definitions(BlackKnightRecomp PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
if(MSVC_FRONTEND)
    target_compile_options(BlackKnightRecomp PRIVATE /EHsc /std:c++latest /utf-8 $<$<CONFIG:RelWithDebInfo>:/Ob2>)
    if(CLANG)
        target_compile_options(BlackKnightRecomp PRIVATE -Wno-unused-function -Wno-unused-variable)
    else()
        target_compile_options(BlackKnightRecomp PRIVATE /wd4611)
    endif()
    target_link_options(BlackKnightRecomp PRIVATE /STACK:268435456 /SUBSYSTEM:CONSOLE)
endif()
