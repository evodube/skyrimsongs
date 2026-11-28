cmake_minimum_required(VERSION 3.24)
project(SkyrimSongs VERSION 0.5.11 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- CommonLibSSE-NG (git submodule, the same commit as SkyrimYacht): one DLL for SE 1.5.97, AE and 1.7.99+
set(ENABLE_SKYRIM_SE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(extern/CommonLibSSE-NG EXCLUDE_FROM_ALL)
include(extern/CommonLibSSE-NG/cmake/CommonLibSSE.cmake)

find_package(imgui CONFIG REQUIRED)

set(MUSIC_SOURCES src/music/Synth.cpp src/music/Midi.cpp src/music/Songs.cpp src/music/Judge.cpp src/music/Crowd.cpp)

add_commonlibsse_plugin(${PROJECT_NAME}
    AUTHOR "evodube"
    USE_ADDRESS_LIBRARY
    SOURCES src/plugin/main.cpp src/plugin/Config.cpp src/plugin/Overlay.cpp src/plugin/Audio.cpp src/plugin/MidiIn.cpp
            src/plugin/Performance.cpp src/plugin/Menu.cpp src/plugin/InstrumentHud.cpp src/plugin/Lang.cpp ${MUSIC_SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE src/plugin extern/SKSEMenuFramework)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/plugin/PCH.h)
target_compile_definitions(${PROJECT_NAME} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(${PROJECT_NAME} PRIVATE imgui::imgui d3d11 dxgi ole32 winmm)

if (MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /utf-8 /permissive- /Zc:preprocessor)
    target_link_options(${PROJECT_NAME} PRIVATE "$<$<CONFIG:Release>:/DEBUG:FULL;/OPT:REF;/OPT:ICF>")
    # the music code is plain C++ with no PCH of its own (it's tested on Linux too)
    set_source_files_properties(${MUSIC_SOURCES} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
endif()
