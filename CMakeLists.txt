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

# BKR_CALL_COUNTS: instrumented build counting calls to every game function, for
# frame-rate analysis with TAS scripts (docs/tas.md); the exe is BlackKnightRecomp_counts.
option(BKR_CALL_COUNTS "Count calls to every recompiled function" OFF)
if(BKR_CALL_COUNTS)
    add_compile_definitions(BKR_CALL_COUNTS)
endif()

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

# ---------------------------------------------------------------- disc image installer
# nod (https://github.com/encounter/nod, MIT OR Apache-2.0, used under MIT) reads the
# user's disc image; the prebuilt package is downloaded at configure time.
if(WIN32)
    include(FetchContent)
    set(NOD_VERSION v2.0.0-alpha.12)
    FetchContent_Declare(nod_prebuilt
        URL https://github.com/encounter/nod/releases/download/${NOD_VERSION}/libnod-windows-x86_64.tar.gz
        URL_HASH SHA256=c9dfecf8e52066a5222f3cb1cad18d0740c958ce65bf60cb84e780a3856ed881
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
    FetchContent_MakeAvailable(nod_prebuilt)
    find_package(nod REQUIRED CONFIG PATHS ${nod_prebuilt_SOURCE_DIR} NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)

    add_library(bkr_installer STATIC runtime/install/install.cpp runtime/install/verify.cpp runtime/install/releases.cpp)
    target_include_directories(bkr_installer PUBLIC runtime)
    target_compile_definitions(bkr_installer PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(bkr_installer PUBLIC nod::nod_static)
    if(MSVC_FRONTEND)
        target_compile_options(bkr_installer PRIVATE /EHsc /utf-8)
        # nod's C dependencies are built against the DLL runtime; we link the static one
        target_link_options(bkr_installer INTERFACE /NODEFAULTLIB:msvcrt /IGNORE:4217)
    endif()

    # bkr_install <image>: installs game/extracted/DATA before the first build_generated.py
    add_executable(bkr_install tools/bkr_install/main.cpp)
    target_link_libraries(bkr_install PRIVATE bkr_installer)
    if(MSVC_FRONTEND)
        target_compile_options(bkr_install PRIVATE /EHsc /utf-8)
    endif()
    set_target_properties(bkr_install PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/out
                                                 RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_SOURCE_DIR}/out
                                                 RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/out)
endif()

# ---------------------------------------------------------------- Dear ImGui (launcher, settings overlay)
# https://github.com/ocornut/imgui (MIT), downloaded at configure time
if(WIN32)
    FetchContent_Declare(imgui
        URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.tar.gz
        URL_HASH SHA256=21d8a0a565e85dce943e375db00812c2f3f0ab21f3f0f7964e364a63422d7f99
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
    FetchContent_MakeAvailable(imgui)
    add_library(imgui STATIC ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp
                             ${imgui_SOURCE_DIR}/imgui_tables.cpp ${imgui_SOURCE_DIR}/imgui_widgets.cpp
                             ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
    target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
    target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
endif()

if(NOT EXISTS ${CMAKE_SOURCE_DIR}/generated/sources.cmake)
    # Reconfigure by itself once tools/build_generated.py has filled generated/
    file(MAKE_DIRECTORY ${CMAKE_SOURCE_DIR}/generated)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/generated)
    message(WARNING "generated/ is empty: only bkr_install is built. Install the game data "
                    "(out/bkr_install <disc image>), run tools/build_generated.py, then build again.")
    return()
endif()
include(${CMAKE_SOURCE_DIR}/generated/sources.cmake)

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
list(FILTER RUNTIME_SOURCES EXCLUDE REGEX "/runtime/install/")
add_executable(BlackKnightRecomp ${RUNTIME_SOURCES})
target_include_directories(BlackKnightRecomp PRIVATE runtime runtime/cpu generated)
target_sources(BlackKnightRecomp PRIVATE $<TARGET_OBJECTS:ppcrecomp>)
# version shown in the launcher and the overlay, refreshed on every build
set(BKR_VERSION_CPP ${CMAKE_BINARY_DIR}/bkr_version.cpp)
add_custom_target(bkr_version
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -DVERSION_OUT=${BKR_VERSION_CPP}
            -P ${CMAKE_SOURCE_DIR}/tools/version.cmake
    BYPRODUCTS ${BKR_VERSION_CPP} VERBATIM)
target_sources(BlackKnightRecomp PRIVATE ${BKR_VERSION_CPP})
add_dependencies(BlackKnightRecomp bkr_version)
target_link_libraries(BlackKnightRecomp PRIVATE bkr_installer imgui d3d11 dxgi d3dcompiler xinput winmm ole32 uuid shell32 xaudio2 dbghelp)
# the scripts in tools/ and the shader cache expect the game in out/
set_target_properties(BlackKnightRecomp PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/out
                                                   RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_SOURCE_DIR}/out
                                                   RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/out)
if(BKR_CALL_COUNTS)
    set_target_properties(BlackKnightRecomp PROPERTIES OUTPUT_NAME BlackKnightRecomp_counts)
endif()
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
