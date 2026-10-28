cmake_minimum_required(VERSION 3.21)

# Build with the presets in CMakePresets.json: clang-cl + Ninja on Windows (MSVC also
# works), clang + Ninja on Linux. The runtime keeps its platform code in runtime/platform:
# Win32, Direct3D 11, XInput and XAudio2 on Windows; SDL3, dxvk-native (Direct3D 11 on
# Vulkan) and vkd3d-shader on Linux (docs/linux.md).
project(BlackKnightRecomp LANGUAGES CXX)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(LINUX ON)
    enable_language(C)   # the libraries built from source (SDL3, vkd3d, libarchive, ...)
endif()

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
include(FetchContent)
set(NOD_VERSION v2.0.0-alpha.12)
if(WIN32)
    FetchContent_Declare(nod_prebuilt
        URL https://github.com/encounter/nod/releases/download/${NOD_VERSION}/libnod-windows-x86_64.tar.gz
        URL_HASH SHA256=c9dfecf8e52066a5222f3cb1cad18d0740c958ce65bf60cb84e780a3856ed881
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
elseif(LINUX)
    FetchContent_Declare(nod_prebuilt
        URL https://github.com/encounter/nod/releases/download/${NOD_VERSION}/libnod-linux-x86_64.tar.gz
        URL_HASH SHA256=920adb1d2386c8728e0521f8d52809057c04bd46913433a0000ee0931d08d69f
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
endif()
if(WIN32 OR LINUX)
    FetchContent_MakeAvailable(nod_prebuilt)
    find_package(nod REQUIRED CONFIG PATHS ${nod_prebuilt_SOURCE_DIR} NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)

    add_library(bkr_installer STATIC runtime/install/install.cpp runtime/install/verify.cpp runtime/install/releases.cpp)
    target_include_directories(bkr_installer PUBLIC runtime)
    target_link_libraries(bkr_installer PUBLIC nod::nod_static)
    if(WIN32)
        target_compile_definitions(bkr_installer PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
        target_sources(bkr_installer PRIVATE runtime/platform/win32/sha1.cpp)
    else()
        target_compile_options(bkr_installer PRIVATE -include ${CMAKE_SOURCE_DIR}/runtime/platform/compat.h)
        target_sources(bkr_installer PRIVATE runtime/platform/linux/sha1.cpp)
        target_link_libraries(bkr_installer PUBLIC pthread dl m)
    endif()
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

    include(cmake/mod_deps.cmake)

    # bkr_mods_test: checks the mod overlay of the disc layer on a synthetic disc (tools/mods_test)
    if(WIN32)
        set(BKR_PLATFORM_DIR runtime/platform/win32)
    else()
        set(BKR_PLATFORM_DIR runtime/platform/linux)
    endif()
    add_executable(bkr_mods_test tools/mods_test/main.cpp runtime/ios/disc.cpp runtime/mods/mods.cpp
                                 runtime/mods/mod_install.cpp runtime/mods/gamebanana.cpp runtime/net/http.cpp
                                 ${BKR_PLATFORM_DIR}/http.cpp)
    target_link_libraries(bkr_mods_test PRIVATE bkr_archive bkr_json)
    target_include_directories(bkr_mods_test PRIVATE runtime)
    target_compile_definitions(bkr_mods_test PRIVATE BKR_MODS_TEST_DATA="${CMAKE_SOURCE_DIR}/tools/mods_test/data")
    if(WIN32)
        target_link_libraries(bkr_mods_test PRIVATE winhttp bcrypt shell32)
        target_compile_definitions(bkr_mods_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    else()
        target_link_libraries(bkr_mods_test PRIVATE dl)
        target_compile_options(bkr_mods_test PRIVATE -include ${CMAKE_SOURCE_DIR}/runtime/platform/compat.h)
    endif()
    if(MSVC_FRONTEND)
        target_compile_options(bkr_mods_test PRIVATE /EHsc /std:c++latest /utf-8)
    endif()
    set_target_properties(bkr_mods_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/out
                                                   RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_SOURCE_DIR}/out
                                                   RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/out)
endif()

# ---------------------------------------------------------------- Linux platform libraries
# Downloaded at configure time, pinned like nod and ImGui; their shared libraries are copied
# next to the executable (out/) and into the AppImage (tools/package_linux.sh).
#   SDL3 (https://libsdl.org, zlib): window, input, audio, dialogs
#   dxvk-native (https://github.com/doitsujin/dxvk, zlib): Direct3D 11 on Vulkan, prebuilt
#     for the Steam Runtime; its MinGW Windows/DirectX headers are LGPL-2.1+ (data
#     structure definitions only, see its include/dxvk/README.md)
#   vkd3d-shader (https://gitlab.winehq.org/wine/vkd3d, LGPL-2.1+): compiles the generated
#     HLSL to DXBC (the D3DCompile of Wine and Proton), linked statically
#   stb_image (https://github.com/nothings/stb, MIT or public domain): decodes the mod
#     browser's web images (WIC on Windows); a single header
if(LINUX)
    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
        URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz
        URL_HASH SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
    FetchContent_MakeAvailable(SDL3)

    FetchContent_Declare(dxvk_native
        URL https://github.com/doitsujin/dxvk/releases/download/v3.1.1/dxvk-native-3.1.1-steamrt-sniper.tar.gz
        URL_HASH SHA256=506fa1ea5bb6ce9a89e11436f7ab5191bc0196e8a577e0cf0b16a45bbc9991b7
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
    FetchContent_MakeAvailable(dxvk_native)   # the archive's usr/ becomes the source dir
    set(DXVK_LIBS ${dxvk_native_SOURCE_DIR}/lib/libdxvk_d3d11.so.0 ${dxvk_native_SOURCE_DIR}/lib/libdxvk_dxgi.so.0)
    add_library(dxvk INTERFACE)
    target_include_directories(dxvk SYSTEM INTERFACE ${dxvk_native_SOURCE_DIR}/include/dxvk)
    target_link_libraries(dxvk INTERFACE ${DXVK_LIBS})

    # autotools build; needs bison, flex and Perl's JSON module. It wants newer Vulkan and
    # SPIR-V headers than older distributions have (Khronos, Apache-2.0/MIT, headers only).
    FetchContent_Declare(vulkan_headers
        URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.341.0.tar.gz
        URL_HASH SHA256=d73bc5036b6556b741f6985ff600ca720308c5f2850e4a43ceb498bd3de069e7
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE
        SOURCE_SUBDIR none)   # headers only: no add_subdirectory
    FetchContent_Declare(spirv_headers
        URL https://github.com/KhronosGroup/SPIRV-Headers/archive/refs/tags/vulkan-sdk-1.4.341.0.tar.gz
        URL_HASH SHA256=cab0a654c4917e16367483296b44cdb1d614e3120c721beafcd37e3a8580486c
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE
        SOURCE_SUBDIR none)
    FetchContent_MakeAvailable(vulkan_headers spirv_headers)
    include(ExternalProject)
    ExternalProject_Add(vkd3d_build
        URL https://dl.winehq.org/vkd3d/source/vkd3d-1.19.tar.xz
        URL_HASH SHA256=034613605baab8ba84674f8d272cf22b5e86bc6bc03fc5728ef9bce07308baa6
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE   # the archive's times: else make reruns autotools
        CONFIGURE_COMMAND <SOURCE_DIR>/configure --disable-tests --disable-demos --disable-doxygen-doc --enable-static
                          --without-ncurses --without-opengl --without-xcb --with-spirv-tools=no
                          CC=${CMAKE_C_COMPILER} "CFLAGS=-O2 -fPIC -fno-lto"
                          "CPPFLAGS=-I${vulkan_headers_SOURCE_DIR}/include -I${spirv_headers_SOURCE_DIR}/include"
        BUILD_COMMAND make -j8
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS <BINARY_DIR>/.libs/libvkd3d-shader.a)
    ExternalProject_Get_Property(vkd3d_build SOURCE_DIR BINARY_DIR)
    file(MAKE_DIRECTORY ${SOURCE_DIR}/include)
    add_library(vkd3d_shader INTERFACE)
    add_dependencies(vkd3d_shader vkd3d_build)
    target_include_directories(vkd3d_shader SYSTEM INTERFACE ${SOURCE_DIR}/include)
    target_link_libraries(vkd3d_shader INTERFACE ${BINARY_DIR}/.libs/libvkd3d-shader.a m pthread)

    FetchContent_Declare(stb
        URL https://raw.githubusercontent.com/nothings/stb/013ac3beddff3dbffafd5177e7972067cd2b5083/stb_image.h
        URL_HASH SHA256=594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3
        DOWNLOAD_NO_EXTRACT TRUE)
    FetchContent_MakeAvailable(stb)
    add_library(stb_image INTERFACE)
    target_include_directories(stb_image SYSTEM INTERFACE ${stb_SOURCE_DIR})
endif()

# ---------------------------------------------------------------- Dear ImGui (launcher, settings overlay)
# https://github.com/ocornut/imgui (MIT), downloaded at configure time
if(WIN32 OR LINUX)
    FetchContent_Declare(imgui
        URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.tar.gz
        URL_HASH SHA256=21d8a0a565e85dce943e375db00812c2f3f0ab21f3f0f7964e364a63422d7f99
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
    FetchContent_MakeAvailable(imgui)
    add_library(imgui STATIC ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp
                             ${imgui_SOURCE_DIR}/imgui_tables.cpp ${imgui_SOURCE_DIR}/imgui_widgets.cpp
                             ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
    target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
    target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
    if(WIN32)
        target_sources(imgui PRIVATE ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp)
    else()
        target_sources(imgui PRIVATE ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp)
        target_link_libraries(imgui PUBLIC SDL3::SDL3 dxvk)
    endif()
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

if(NOT WIN32 AND NOT LINUX)
    message(STATUS "The runtime supports Windows and Linux: building the recompiled code (ppcrecomp) only")
    return()
endif()

# ---------------------------------------------------------------- runtime
# shared code, plus one platform's implementation of runtime/platform/*.h
file(GLOB_RECURSE RUNTIME_SOURCES CONFIGURE_DEPENDS runtime/*.cpp)
list(FILTER RUNTIME_SOURCES EXCLUDE REGEX "/runtime/(install|platform)/")
if(WIN32)
    file(GLOB PLATFORM_SOURCES CONFIGURE_DEPENDS runtime/platform/win32/*.cpp)
else()
    file(GLOB PLATFORM_SOURCES CONFIGURE_DEPENDS runtime/platform/linux/*.cpp)
endif()
list(FILTER PLATFORM_SOURCES EXCLUDE REGEX "/sha1[.]cpp$")   # in bkr_installer
list(APPEND RUNTIME_SOURCES ${PLATFORM_SOURCES})
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
if(WIN32)
    target_link_libraries(BlackKnightRecomp PRIVATE bkr_installer imgui bkr_archive bkr_json winhttp windowscodecs bcrypt d3d11 dxgi d3dcompiler xinput winmm ole32 uuid shell32 xaudio2 dbghelp)
else()
    target_link_libraries(BlackKnightRecomp PRIVATE bkr_installer imgui bkr_archive bkr_json SDL3::SDL3 dxvk vkd3d_shader stb_image pthread dl)
endif()
# the scripts in tools/ and the shader cache expect the game in out/
set_target_properties(BlackKnightRecomp PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/out
                                                   RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_SOURCE_DIR}/out
                                                   RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/out)
if(BKR_CALL_COUNTS)
    set_target_properties(BlackKnightRecomp PROPERTIES OUTPUT_NAME BlackKnightRecomp_counts)
endif()
# licences of the libraries in the exe, shipped with it (tools/package.ps1 copies out/licenses)
set(BKR_LICENSES ${CMAKE_BINARY_DIR}/licenses)
file(DOWNLOAD https://raw.githubusercontent.com/encounter/nod/v2.0.0-alpha.12/LICENSE-MIT ${BKR_LICENSES}/nod-LICENSE-MIT.txt
     EXPECTED_HASH SHA256=544ffe9befaefa6b59ab1fdde58789069ba7c1b65432bdea273a16b2117909b7)
file(DOWNLOAD https://raw.githubusercontent.com/nlohmann/json/v3.12.0/LICENSE.MIT ${BKR_LICENSES}/nlohmann-json-LICENSE.MIT.txt
     EXPECTED_HASH SHA256=46a65cffd1ea955132d95a8dd921640714a8d6b537d2e4e482d31145ae95b603)
add_custom_command(TARGET BlackKnightRecomp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_SOURCE_DIR}/out/licenses
    COMMAND ${CMAKE_COMMAND} -E copy ${BKR_LICENSES}/nod-LICENSE-MIT.txt ${BKR_LICENSES}/nlohmann-json-LICENSE.MIT.txt
            ${CMAKE_SOURCE_DIR}/out/licenses/
    COMMAND ${CMAKE_COMMAND} -E copy ${imgui_SOURCE_DIR}/LICENSE.txt ${CMAKE_SOURCE_DIR}/out/licenses/imgui-LICENSE.txt
    COMMAND ${CMAKE_COMMAND} -E copy ${BKR_DEPS_PREFIX}/libarchive/src/bkr_libarchive/COPYING ${CMAKE_SOURCE_DIR}/out/licenses/libarchive-COPYING.txt
    COMMAND ${CMAKE_COMMAND} -E copy ${BKR_DEPS_PREFIX}/zlib/src/bkr_zlib/LICENSE ${CMAKE_SOURCE_DIR}/out/licenses/zlib-LICENSE.txt
    COMMAND ${CMAKE_COMMAND} -E copy ${BKR_DEPS_PREFIX}/xz/src/bkr_lzma/COPYING.0BSD ${CMAKE_SOURCE_DIR}/out/licenses/liblzma-COPYING.0BSD.txt
    VERBATIM)
if(WIN32)
    target_compile_definitions(BlackKnightRecomp PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()
if(MSVC_FRONTEND)
    target_compile_options(BlackKnightRecomp PRIVATE /EHsc /std:c++latest /utf-8 $<$<CONFIG:RelWithDebInfo>:/Ob2>)
    if(CLANG)
        target_compile_options(BlackKnightRecomp PRIVATE -Wno-unused-function -Wno-unused-variable)
    else()
        target_compile_options(BlackKnightRecomp PRIVATE /wd4611)
    endif()
    target_link_options(BlackKnightRecomp PRIVATE /STACK:268435456 /SUBSYSTEM:CONSOLE)
endif()
if(LINUX)
    # MSVC runtime names (compat.h) everywhere; type punning as with MSVC
    target_compile_options(BlackKnightRecomp PRIVATE -include ${CMAKE_SOURCE_DIR}/runtime/platform/compat.h
                           -fno-strict-aliasing -Wno-unused-function -Wno-unused-variable)
    # the C++ runtime inside the exe: it runs on distributions with an older libstdc++
    target_link_options(BlackKnightRecomp PRIVATE -static-libstdc++ -static-libgcc)
    # finds libSDL3 and dxvk next to itself (out/ and the AppImage)
    set_target_properties(BlackKnightRecomp PROPERTIES BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")
    add_custom_command(TARGET BlackKnightRecomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_SONAME_FILE:SDL3-shared> ${DXVK_LIBS} $<TARGET_FILE_DIR:BlackKnightRecomp>
        VERBATIM)
endif()
