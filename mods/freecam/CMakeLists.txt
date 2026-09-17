cmake_minimum_required(VERSION 3.28)

project(freecam VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(BRIDGER_DIR "" CACHE PATH "Path to the Bridger checkout")
set(BRIDGER_GAME_DIR "" CACHE PATH "Death Stranding install directory")

if(NOT BRIDGER_DIR)
    message(FATAL_ERROR "Set BRIDGER_DIR to your Bridger checkout")
endif()

add_library(freecam SHARED src/mod.cpp)
target_include_directories(freecam PRIVATE "${BRIDGER_DIR}/include")
target_compile_definitions(freecam PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(freecam PRIVATE /W4 /permissive- /utf-8 /bigobj)
set_target_properties(freecam PROPERTIES OUTPUT_NAME freecam)

if(BRIDGER_GAME_DIR)
    add_custom_target(install_mod
        COMMAND ${CMAKE_COMMAND} -E make_directory "${BRIDGER_GAME_DIR}/Bridger/mods/freecam"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:freecam> "${BRIDGER_GAME_DIR}/Bridger/mods/freecam"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/manifest.json" "${BRIDGER_GAME_DIR}/Bridger/mods/freecam"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_SOURCE_DIR}/shaders" "${BRIDGER_GAME_DIR}/Bridger/mods/freecam/shaders"
        DEPENDS freecam
        COMMENT "Installing freecam")
endif()
