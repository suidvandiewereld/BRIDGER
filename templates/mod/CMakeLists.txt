cmake_minimum_required(VERSION 3.28)

project(__MOD_ID__ VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(BRIDGER_DIR "" CACHE PATH "Path to the Bridger checkout")
set(BRIDGER_GAME_DIR "" CACHE PATH "Death Stranding install directory")

if(NOT BRIDGER_DIR)
    message(FATAL_ERROR "Set BRIDGER_DIR to your Bridger checkout")
endif()

add_library(__MOD_ID__ SHARED src/mod.cpp)
target_include_directories(__MOD_ID__ PRIVATE "${BRIDGER_DIR}/include")
target_compile_definitions(__MOD_ID__ PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(__MOD_ID__ PRIVATE /W4 /permissive- /utf-8 /bigobj)
set_target_properties(__MOD_ID__ PROPERTIES OUTPUT_NAME __MOD_ID__)

if(BRIDGER_GAME_DIR)
    add_custom_target(install_mod
        COMMAND ${CMAKE_COMMAND} -E make_directory "${BRIDGER_GAME_DIR}/Bridger/mods/__MOD_ID__"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:__MOD_ID__> "${BRIDGER_GAME_DIR}/Bridger/mods/__MOD_ID__"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/manifest.json" "${BRIDGER_GAME_DIR}/Bridger/mods/__MOD_ID__"
        DEPENDS __MOD_ID__
        COMMENT "Installing __MOD_ID__")
endif()
