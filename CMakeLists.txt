cmake_minimum_required(VERSION 3.20)

# The VS2019 16.10 linker crashes (LNK1000) on the Windows SDK 10.0.26100 import
# libraries; prefer 10.0.19041 when it is installed (override with -DCMAKE_SYSTEM_VERSION).
if(WIN32 AND NOT CMAKE_SYSTEM_VERSION AND EXISTS "C:/Program Files (x86)/Windows Kits/10/Lib/10.0.19041.0/um/x64")
    set(CMAKE_SYSTEM_VERSION 10.0.19041.0)
endif()

project(BlackKnightRecomp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT EXISTS ${CMAKE_SOURCE_DIR}/generated/sources.cmake)
    message(FATAL_ERROR "Run tools/build_generated.py first to generate the recompiled sources")
endif()
include(${CMAKE_SOURCE_DIR}/generated/sources.cmake)

# ---------------------------------------------------------------- recompiled game code
add_library(ppcrecomp OBJECT ${PPC_RECOMP_SOURCES})
target_include_directories(ppcrecomp PUBLIC runtime/cpu generated)
if(MSVC)
    target_compile_options(ppcrecomp PRIVATE /O2 /Ob2 /GS- /bigobj /MP /EHs-c- /wd4102 /wd4244 /wd4146 /wd4018 /std:c++latest)
endif()

# ---------------------------------------------------------------- runtime
file(GLOB_RECURSE RUNTIME_SOURCES CONFIGURE_DEPENDS runtime/*.cpp)
add_executable(BlackKnightRecomp ${RUNTIME_SOURCES})
target_include_directories(BlackKnightRecomp PRIVATE runtime runtime/cpu generated)
target_sources(BlackKnightRecomp PRIVATE $<TARGET_OBJECTS:ppcrecomp>)
target_link_libraries(BlackKnightRecomp PRIVATE d3d11 dxgi d3dcompiler xinput winmm ole32 xaudio2)
if(MSVC)
    target_compile_options(BlackKnightRecomp PRIVATE /MP /EHsc /std:c++latest /wd4611 /utf-8)
    target_compile_definitions(BlackKnightRecomp PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    target_link_options(BlackKnightRecomp PRIVATE /STACK:268435456)
endif()
