cmake_minimum_required(VERSION 3.25)
cmake_policy(SET CMP0091 NEW)

project(nite3d VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
    add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

add_library(nite3d_core STATIC
    src/core/binary_reader.cpp
    src/core/assets.cpp
    src/core/config.cpp
    src/core/recordings.cpp
    src/core/sound.cpp
    src/core/sha256.cpp
    src/core/winhelp.cpp
    src/game/level.cpp
    src/game/demo.cpp
    src/game/movement.cpp
    src/game/guards.cpp
    src/game/world.cpp
    src/game/core.cpp
    src/game/savegame.cpp
    src/render/scene.cpp
    src/render/hud.cpp
    src/render/help.cpp
    src/render/ui.cpp
    src/render/settings_menu.cpp
)
target_include_directories(nite3d_core PUBLIC include)
set_target_properties(nite3d_core PROPERTIES
    CXX_EXTENSIONS OFF
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

if(WIN32)
    add_library(nite3d_platform_audio STATIC
        src/platform/midi.cpp
        src/platform/effects.cpp
    )
    target_include_directories(nite3d_platform_audio PUBLIC include)
    target_link_libraries(nite3d_platform_audio PUBLIC nite3d_core winmm xaudio2 ole32)
    set_target_properties(nite3d_platform_audio PROPERTIES
        CXX_EXTENSIONS OFF
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    add_library(nite3d_platform_window STATIC
        src/platform/window.cpp
        src/platform/input.cpp
        src/platform/paths.cpp
        src/platform/config_storage.cpp
        src/platform/save_storage.cpp
        src/platform/map_browser_win32.cpp
        src/platform/help_viewer_win32.cpp
    )
    target_include_directories(nite3d_platform_window PUBLIC include)
    target_link_libraries(nite3d_platform_window PUBLIC nite3d_core user32 gdi32 comdlg32 shell32 ole32 uuid winmm)
    set_target_properties(nite3d_platform_window PROPERTIES
        CXX_EXTENSIONS OFF
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    add_executable(Nitemare3D WIN32 src/platform/win32_main.cpp)
    target_link_libraries(Nitemare3D PRIVATE nite3d_platform_window nite3d_platform_audio)
    set_target_properties(Nitemare3D PROPERTIES
        CXX_EXTENSIONS OFF
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(n3d_tests
        tests/test_main.cpp
        tests/binary_reader_tests.cpp
        tests/assets_tests.cpp
        tests/fli_player_tests.cpp
        tests/recordings_tests.cpp
        tests/demo_tests.cpp
        tests/demo_story_integration_tests.cpp
        tests/config_tests.cpp
        tests/sound_tests.cpp
        tests/ui_tests.cpp
        tests/winhelp_tests.cpp
        tests/help_tests.cpp
        tests/puzzle_tests.cpp
        tests/weapons_tests.cpp
        tests/level_remap_tests.cpp
        tests/object_animation_tests.cpp
        tests/scene_tests.cpp
        tests/sensor_tests.cpp
        tests/settings_menu_tests.cpp
        tests/savegame_tests.cpp
        tests/save_menu_tests.cpp
    )
    target_link_libraries(n3d_tests PRIVATE nite3d_core)
    if(WIN32)
        target_sources(n3d_tests PRIVATE tests/platform_input_tests.cpp)
        target_sources(n3d_tests PRIVATE tests/midi_tests.cpp)
        target_sources(n3d_tests PRIVATE tests/config_storage_tests.cpp)
        target_sources(n3d_tests PRIVATE tests/save_storage_tests.cpp)
        target_link_libraries(n3d_tests PRIVATE nite3d_platform_window)
        target_link_libraries(n3d_tests PRIVATE nite3d_platform_audio)
    endif()
    set_target_properties(n3d_tests PROPERTIES
        CXX_EXTENSIONS OFF
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    add_test(NAME n3d_tests COMMAND n3d_tests)
endif()
