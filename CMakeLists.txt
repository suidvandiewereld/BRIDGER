cmake_minimum_required(VERSION 3.28)

project(Bridger VERSION 0.1.0 LANGUAGES C CXX ASM_MASM)

if(NOT MSVC)
    message(FATAL_ERROR "Bridger targets the MSVC toolchain on Windows x64.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(BRIDGER_GAME_DIR "" CACHE PATH "Death Stranding install directory used by the deploy target")

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG v1.3.3
    GIT_SHALLOW ON)

FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW ON)

FetchContent_Declare(lua
    GIT_REPOSITORY https://github.com/lua/lua.git
    GIT_TAG v5.4.7
    GIT_SHALLOW ON)

set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(minhook json)

FetchContent_GetProperties(lua)
if(NOT lua_POPULATED)
    FetchContent_Populate(lua)
endif()
set(LUA_SOURCES
    lapi.c lauxlib.c lbaselib.c lcode.c lcorolib.c lctype.c ldblib.c ldebug.c ldo.c ldump.c
    lfunc.c lgc.c linit.c liolib.c llex.c lmathlib.c lmem.c loadlib.c lobject.c lopcodes.c
    loslib.c lparser.c lstate.c lstring.c lstrlib.c ltable.c ltablib.c ltm.c lundump.c
    lutf8lib.c lvm.c lzio.c)
list(TRANSFORM LUA_SOURCES PREPEND "${lua_SOURCE_DIR}/")
set_source_files_properties(${LUA_SOURCES} PROPERTIES LANGUAGE CXX)
add_library(lua STATIC ${LUA_SOURCES})
target_include_directories(lua PUBLIC ${lua_SOURCE_DIR})
target_compile_options(lua PRIVATE /TP /W0 /EHsc)
set_property(TARGET lua PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

file(READ "${minhook_SOURCE_DIR}/src/trampoline.c" MINHOOK_TRAMPOLINE)
file(READ "${CMAKE_SOURCE_DIR}/src/core/minhook_vmovsd.inc" MINHOOK_VMOVSD)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/core/minhook_vmovsd.inc")
string(REPLACE "if (hs.flags & F_ERROR)"
    "${MINHOOK_VMOVSD}\n        if (hs.flags & F_ERROR)"
    MINHOOK_TRAMPOLINE "${MINHOOK_TRAMPOLINE}")
string(REPLACE "if (hs.flags & F_ERROR)"
    "if ((hs.flags & F_ERROR) && oldPos < sizeof(JMP_REL))"
    MINHOOK_TRAMPOLINE "${MINHOOK_TRAMPOLINE}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/minhook_trampoline.c" "${MINHOOK_TRAMPOLINE}")

add_library(minhook STATIC
    ${minhook_SOURCE_DIR}/src/buffer.c
    ${minhook_SOURCE_DIR}/src/hook.c
    ${CMAKE_CURRENT_BINARY_DIR}/minhook_trampoline.c
    ${minhook_SOURCE_DIR}/src/hde/hde32.c
    ${minhook_SOURCE_DIR}/src/hde/hde64.c
)
target_include_directories(minhook PUBLIC ${minhook_SOURCE_DIR}/include)
target_include_directories(minhook PRIVATE ${minhook_SOURCE_DIR}/src)
set_property(TARGET minhook PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(hook_regression tests/hook_regression.cpp)
target_link_libraries(hook_regression PRIVATE minhook)

add_executable(script_ffi_test
    tests/script_ffi_test.cpp
    src/script/ffi.cpp
    src/script/ffi_x64.asm
    src/core/guard.cpp
    src/core/log.cpp)
target_include_directories(script_ffi_test PRIVATE src include ${minhook_SOURCE_DIR}/include)
target_compile_definitions(script_ffi_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(script_ffi_test PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /utf-8>)
target_link_libraries(script_ffi_test PRIVATE minhook)

set(PRELUDE_LUA "${CMAKE_SOURCE_DIR}/src/script/prelude.lua")
set(PRELUDE_BLOB "${CMAKE_BINARY_DIR}/generated/prelude_blob.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PRELUDE_LUA}")
file(READ "${PRELUDE_LUA}" PRELUDE_HEX HEX)
string(LENGTH "${PRELUDE_HEX}" PRELUDE_HEX_LENGTH)
set(PRELUDE_ARRAY "")
set(PRELUDE_OFFSET 0)
while(PRELUDE_OFFSET LESS PRELUDE_HEX_LENGTH)
    string(SUBSTRING "${PRELUDE_HEX}" ${PRELUDE_OFFSET} 64 PRELUDE_CHUNK)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," PRELUDE_CHUNK "${PRELUDE_CHUNK}")
    string(APPEND PRELUDE_ARRAY "    ${PRELUDE_CHUNK}\n")
    math(EXPR PRELUDE_OFFSET "${PRELUDE_OFFSET} + 64")
endwhile()
file(CONFIGURE OUTPUT "${PRELUDE_BLOB}" CONTENT
"// Generated from src/script/prelude.lua by CMakeLists.txt. Do not edit.
#include <cstddef>
namespace bridger::script {
extern const unsigned char kPrelude[] = {
@PRELUDE_ARRAY@};
extern const std::size_t kPreludeSize = sizeof kPrelude;
}
" @ONLY)

add_library(bridger_objects OBJECT
    src/content/content.cpp
    src/content/content_api.cpp
    src/core/bridger.cpp
    src/core/guard.cpp
    src/core/licence.cpp
    src/core/log.cpp
    src/core/memory.cpp
    src/core/process.cpp
    src/core/settings.cpp
    src/debugger/debugger.cpp
    src/debugger/globals.cpp
    src/debugger/model.cpp
    src/debugger/picker.cpp
    src/decima/dumper.cpp
    src/decima/symbols.cpp
    src/fx/depth_capture.cpp
    src/fx/dxbc.cpp
    src/fx/fx.cpp
    src/fx/fx_api.cpp
    src/fx/fx_panel.cpp
    src/fx/image.cpp
    src/fx/ngx.cpp
    src/fx/pipeline.cpp
    src/fx/bindings.cpp
    src/fx/rt.cpp
    src/loader/patches.cpp
    src/fx/prelude.cpp
    src/fx/readback.cpp
    src/loader/registry.cpp
    src/overlay/d3d.cpp
    src/overlay/inspect.cpp
    src/overlay/overlay.cpp
    src/overlay/renderer.cpp
    src/script/bind_engine.cpp
    src/script/bind_hooks.cpp
    src/script/bind_host.cpp
    src/script/bind_values.cpp
    src/script/console.cpp
    src/script/ffi.cpp
    src/script/ffi_x64.asm
    src/script/prelude.cpp
    src/script/reflect.cpp
    src/script/script_panel.cpp
    src/script/vm.cpp
    ${PRELUDE_BLOB}
    src/ui/draw.cpp
    src/ui/font.cpp
    src/ui/input.cpp
    src/ui/raster.cpp
    src/ui/ui.cpp
)
target_link_libraries(bridger_objects PUBLIC minhook nlohmann_json::nlohmann_json lua)
target_include_directories(bridger_objects PUBLIC src include ${minhook_SOURCE_DIR}/include)
target_compile_definitions(bridger_objects PUBLIC
    BRIDGER_VERSION="${PROJECT_VERSION}"
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)
target_compile_options(bridger_objects PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /utf-8>)

add_library(bridger_core SHARED $<TARGET_OBJECTS:bridger_objects>)
target_link_libraries(bridger_core PRIVATE minhook lua)
target_link_options(bridger_core PRIVATE /MAP)
set_target_properties(bridger_core PROPERTIES OUTPUT_NAME bridger)

add_executable(script_runtime_test tests/script_runtime_test.cpp)
target_link_libraries(script_runtime_test PRIVATE bridger_objects)
target_compile_options(script_runtime_test PRIVATE /W4 /permissive- /utf-8)
add_test(NAME script_runtime COMMAND script_runtime_test)

add_executable(debugger_preview tools/debugger_preview/main.cpp)
target_link_libraries(debugger_preview PRIVATE bridger_objects)
target_compile_definitions(debugger_preview PRIVATE BRIDGER_SOURCE_DIR="${CMAKE_SOURCE_DIR}" BRIDGER_SOURCE_DIR_W=L"${CMAKE_SOURCE_DIR}")
target_compile_options(debugger_preview PRIVATE /W4 /permissive- /utf-8)

add_executable(lua_check tools/script/lua_check.cpp)
target_link_libraries(lua_check PRIVATE lua)
target_compile_options(lua_check PRIVATE /W4 /permissive- /utf-8)
file(GLOB BRIDGER_SHIPPED_SCRIPTS CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/templates/script/*.lua")
add_test(NAME script_syntax COMMAND lua_check
    ${BRIDGER_SHIPPED_SCRIPTS} "${CMAKE_SOURCE_DIR}/src/script/prelude.lua"
    "${CMAKE_SOURCE_DIR}/include/bridger/lua/bridger.d.lua")

add_library(bridger_proxy SHARED src/proxy/dllmain.cpp)

target_include_directories(bridger_proxy PRIVATE src)
target_compile_definitions(bridger_proxy PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)
target_compile_options(bridger_proxy PRIVATE /W4 /permissive- /utf-8)
set_target_properties(bridger_proxy PROPERTIES OUTPUT_NAME winhttp)

add_executable(ui_preview
    tools/ui_preview/preview.cpp
    tools/ui_preview/fx_stub.cpp
    tools/ui_preview/script_stub.cpp
    src/fx/fx_panel.cpp
    src/script/script_panel.cpp
    src/ui/draw.cpp
    src/ui/font.cpp
    src/ui/input.cpp
    src/ui/ui.cpp
)
target_include_directories(ui_preview PRIVATE src include)
target_compile_definitions(ui_preview PRIVATE BRIDGER_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}" WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(ui_preview PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(ui_preview PRIVATE gdi32 user32)

add_library(bridger_headers STATIC tests/compile_headers.cpp)
target_include_directories(bridger_headers PUBLIC include)
target_compile_options(bridger_headers PRIVATE /W3 /permissive- /utf-8 /bigobj)

add_library(hello_mod SHARED examples/hello/hello.cpp)
target_include_directories(hello_mod PRIVATE include mods/freecam)
target_compile_definitions(hello_mod PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(hello_mod PRIVATE /W4 /permissive- /utf-8)
set_target_properties(hello_mod PROPERTIES OUTPUT_NAME hello)

add_library(fxdemo_mod SHARED examples/fxdemo/src/mod.cpp)
target_include_directories(fxdemo_mod PRIVATE include)
target_compile_definitions(fxdemo_mod PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(fxdemo_mod PRIVATE /W4 /permissive- /utf-8)
set_target_properties(fxdemo_mod PROPERTIES OUTPUT_NAME fxdemo)

add_library(photoreal_mod SHARED mods/photoreal/src/mod.cpp)
target_include_directories(photoreal_mod PRIVATE include)
target_compile_definitions(photoreal_mod PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(photoreal_mod PRIVATE /W4 /permissive- /utf-8)
set_target_properties(photoreal_mod PROPERTIES OUTPUT_NAME photoreal)

add_executable(fx_math_test tests/fx_math_test.cpp)
target_include_directories(fx_math_test PRIVATE src include)
target_compile_options(fx_math_test PRIVATE /W4 /permissive- /utf-8)
add_executable(fx_check tools/fx_check/check.cpp src/fx/prelude.cpp)
target_include_directories(fx_check PRIVATE src include)
target_compile_definitions(fx_check PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(fx_check PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(fx_check PRIVATE d3dcompiler)

enable_testing()
find_program(BRIDGER_TEST_PYTHON NAMES python python3)
if(BRIDGER_TEST_PYTHON)
    add_test(NAME fx_golden_tool COMMAND "${BRIDGER_TEST_PYTHON}"
        "${CMAKE_SOURCE_DIR}/tools/fx_check/test_golden.py")
endif()
add_test(NAME hook_regression COMMAND hook_regression)
add_test(NAME script_ffi COMMAND script_ffi_test)
add_test(NAME fx_math COMMAND fx_math_test)
add_executable(fx_dxbc_test tests/fx_dxbc_test.cpp src/fx/dxbc.cpp src/fx/bindings.cpp)
target_include_directories(fx_dxbc_test PRIVATE src)
target_compile_definitions(fx_dxbc_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(fx_dxbc_test PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(fx_dxbc_test PRIVATE d3d12 dxgi d3dcompiler)
add_test(NAME fx_dxbc COMMAND fx_dxbc_test)
add_executable(fx_upscale_test tests/fx_upscale_test.cpp)
target_link_libraries(fx_upscale_test PRIVATE bridger_objects d3d12 dxgi)
add_test(NAME fx_upscale COMMAND fx_upscale_test)

file(GLOB BRIDGER_SDK_BIN_DIRS LIST_DIRECTORIES true "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/10.*")
list(SORT BRIDGER_SDK_BIN_DIRS COMPARE NATURAL ORDER DESCENDING)
list(TRANSFORM BRIDGER_SDK_BIN_DIRS APPEND "/x64")
find_file(BRIDGER_DXCOMPILER_DLL dxcompiler.dll
    PATHS "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
          ${BRIDGER_SDK_BIN_DIRS}
    NO_DEFAULT_PATH)
find_file(BRIDGER_DXIL_DLL dxil.dll
    PATHS "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
          ${BRIDGER_SDK_BIN_DIRS}
    NO_DEFAULT_PATH)
if(NOT BRIDGER_DXCOMPILER_DLL OR NOT BRIDGER_DXIL_DLL)
    message(WARNING "dxcompiler.dll or dxil.dll not found in the Windows SDK; the raytracing runtime will not compile shaders")
endif()
get_filename_component(BRIDGER_DXC_DIR "${BRIDGER_DXCOMPILER_DLL}" DIRECTORY)

add_executable(fx_rt_test tests/fx_rt_test.cpp)
target_link_libraries(fx_rt_test PRIVATE bridger_objects d3d12 dxgi)
target_compile_definitions(fx_rt_test PRIVATE BRIDGER_DXC_DIR="${BRIDGER_DXC_DIR}")
add_test(NAME fx_rt COMMAND fx_rt_test)
add_test(NAME fxdemo_post_shaders COMMAND fx_check fullscreen
    "${CMAKE_SOURCE_DIR}/examples/fxdemo/shaders/grade.hlsl"
    "${CMAKE_SOURCE_DIR}/examples/fxdemo/shaders/bloom_extract.hlsl"
    "${CMAKE_SOURCE_DIR}/examples/fxdemo/shaders/bloom_blur.hlsl"
    "${CMAKE_SOURCE_DIR}/examples/fxdemo/shaders/bloom_combine.hlsl")
add_test(NAME fxdemo_world_shaders COMMAND fx_check world
    "${CMAKE_SOURCE_DIR}/examples/fxdemo/shaders/marker.hlsl")
add_test(NAME fx_pre_upscale_history COMMAND fx_check fullscreen
    "${CMAKE_SOURCE_DIR}/examples/fxdemo/shaders/motion_history.hlsl")
add_test(NAME fxdemo_screen_shaders COMMAND fx_check screen
    "${CMAKE_SOURCE_DIR}/examples/fxdemo/shaders/scanner.hlsl")
add_test(NAME photoreal_shaders COMMAND fx_check fullscreen
    "${CMAKE_SOURCE_DIR}/mods/photoreal/shaders/blur_down.hlsl"
    "${CMAKE_SOURCE_DIR}/mods/photoreal/shaders/blur.hlsl"
    "${CMAKE_SOURCE_DIR}/mods/photoreal/shaders/photoreal.hlsl")

if(BRIDGER_GAME_DIR)
    add_custom_target(deploy
        COMMAND ${CMAKE_COMMAND} -E make_directory "${BRIDGER_GAME_DIR}/Bridger"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:bridger_proxy> "${BRIDGER_GAME_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:bridger_core> "${BRIDGER_GAME_DIR}/Bridger"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${BRIDGER_DXCOMPILER_DLL}" "${BRIDGER_DXIL_DLL}" "${BRIDGER_GAME_DIR}/Bridger"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${BRIDGER_GAME_DIR}/Bridger/assets"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/assets/logo.bin" "${CMAKE_SOURCE_DIR}/assets/Bridges-Black.ttf"
                "${BRIDGER_GAME_DIR}/Bridger/assets"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${BRIDGER_GAME_DIR}/Bridger/scripts/.types"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/include/bridger/lua/bridger.d.lua"
                "${BRIDGER_GAME_DIR}/Bridger/scripts/.types"
        # The database, signatures and engine Lua types are generated locally by
        # tools/bootstrap.py and never committed; copy whichever exist and warn about the rest.
        COMMAND ${CMAKE_COMMAND} "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}" "-DGAME_DIR=${BRIDGER_GAME_DIR}"
                -P "${CMAKE_SOURCE_DIR}/tools/build/deploy_data.cmake"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/templates/scripts.luarc.json"
                "${BRIDGER_GAME_DIR}/Bridger/scripts/.luarc.json"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${BRIDGER_GAME_DIR}/Bridger/mods/hello"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:hello_mod> "${BRIDGER_GAME_DIR}/Bridger/mods/hello"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/examples/hello/manifest.json"
                "${BRIDGER_GAME_DIR}/Bridger/mods/hello"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${BRIDGER_GAME_DIR}/Bridger/mods/fxdemo"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:fxdemo_mod> "${BRIDGER_GAME_DIR}/Bridger/mods/fxdemo"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/examples/fxdemo/manifest.json"
                "${BRIDGER_GAME_DIR}/Bridger/mods/fxdemo"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_SOURCE_DIR}/examples/fxdemo/shaders"
                "${BRIDGER_GAME_DIR}/Bridger/mods/fxdemo/shaders"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${BRIDGER_GAME_DIR}/Bridger/mods/photoreal"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:photoreal_mod> "${BRIDGER_GAME_DIR}/Bridger/mods/photoreal"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/mods/photoreal/manifest.json"
                "${BRIDGER_GAME_DIR}/Bridger/mods/photoreal"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_SOURCE_DIR}/mods/photoreal/shaders"
                "${BRIDGER_GAME_DIR}/Bridger/mods/photoreal/shaders"
        DEPENDS bridger_proxy bridger_core hello_mod fxdemo_mod photoreal_mod
        COMMENT "Deploying Bridger to ${BRIDGER_GAME_DIR}"
    )
endif()
