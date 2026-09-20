cmake_minimum_required(VERSION 3.21)
message("Using toolchain file ${CMAKE_TOOLCHAIN_FILE}.")

project(
    AddictionFramework
    VERSION 0.1.0
    DESCRIPTION "General-purpose player addiction framework: revives Skyrim's dormant addiction system and exposes it to mods."
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

macro(set_from_environment VARIABLE)
    if (NOT DEFINED ${VARIABLE} AND DEFINED ENV{${VARIABLE}})
        set(${VARIABLE} $ENV{${VARIABLE}})
    endif ()
endmacro()
set_from_environment(VCPKG_ROOT)

add_compile_definitions(SKYRIM)

########################################################################################################################
## CommonLibSSE-NG (alandtse/CommonLibVR, pinned v8.4.0 — Skyrim AE 1.7.x/1799 support) — git submodule
########################################################################################################################
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)  # CommonLib defaults tests ON → would drag in Catch2
add_subdirectory(extern/CommonLibSSE-NG CommonLibSSE EXCLUDE_FROM_ALL)

########################################################################################################################
## Our plugin
########################################################################################################################
set(sources
    src/main.cpp
    src/Hooks.cpp
    src/Papyrus.cpp
    src/Addiction.cpp
    src/Blackout.cpp
    src/Display.cpp
    src/Config.cpp
    src/Casting.cpp
    src/UI.cpp
    src/PluginAPI.cpp
)

add_library(${PROJECT_NAME} SHARED ${sources}
    src/PCH.h src/Hooks.h src/Papyrus.h src/Addiction.h src/Blackout.h src/Display.h src/Config.h
    src/Casting.h src/UI.h src/PluginAPI.h api/AddictionFramework_API.h)

target_include_directories(${PROJECT_NAME}
    PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/api>                        # shipped C++ API header
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/extern/SKSEMenuFramework>)  # vendored menu header

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_compile_options(${PROJECT_NAME} PRIVATE /Zc:preprocessor)  # required to consume CommonLib headers

find_package(nlohmann_json CONFIG REQUIRED)  # JSON config loader (Step 3)

target_link_libraries(${PROJECT_NAME} PRIVATE CommonLibSSE::CommonLibSSE nlohmann_json::nlohmann_json)

target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)

########################################################################################################################
## Deploy into an MO2 mod folder (root == Data). Override -DOUTPUT_FOLDER=... to change.
########################################################################################################################
set(OUTPUT_FOLDER "C:/Steam/steamapps/common/Skyrim Special Edition/ModOrganizer Data/mods/Addiction Framework"
    CACHE PATH "Mod folder to deploy the built DLL/PDB into")

set(DLL_FOLDER "${OUTPUT_FOLDER}/SKSE/Plugins")
message(STATUS "SKSE plugin output folder: ${DLL_FOLDER}")

add_custom_command(
    TARGET ${PROJECT_NAME}
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${DLL_FOLDER}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${PROJECT_NAME}>" "${DLL_FOLDER}/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_PDB_FILE:${PROJECT_NAME}>" "${DLL_FOLDER}/$<TARGET_PDB_FILE_NAME:${PROJECT_NAME}>"
    VERBATIM)
