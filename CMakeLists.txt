cmake_minimum_required(VERSION 3.25)

project(spintool LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SPINTOOL_BUILD_WEB "Build SpinTool for WebAssembly with Emscripten" OFF)

if(EMSCRIPTEN OR SPINTOOL_BUILD_WEB)
    if(NOT EMSCRIPTEN)
        message(FATAL_ERROR "SPINTOOL_BUILD_WEB requires the Emscripten toolchain (use emcmake cmake).")
    endif()

    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        SDL3
        URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.10/SDL3-3.4.10.tar.gz
        URL_HASH SHA256=12b34280415ec8418c864408b93d008a20a6530687ee613d60bfbd20411f2785
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(SDL3)
else()
    # SDL is supplied by the native Linux packages or by vcpkg when
    # cross-compiling from Linux to Windows with MinGW-w64.
    find_package(SDL3 CONFIG REQUIRED)
    find_package(SDL3_image CONFIG REQUIRED)
endif()

add_executable(spintool
        external/imgui/backends/imgui_impl_sdl3.cpp
        external/imgui/backends/imgui_impl_sdlrenderer3.cpp
        external/imgui/misc/cpp/imgui_stdlib.cpp
        external/imgui/imgui_draw.cpp
        external/imgui/imgui_demo.cpp
        external/imgui/imgui_tables.cpp
        external/imgui/imgui_widgets.cpp
        external/imgui/imgui.cpp
        src/editor/editor_brush.cpp
        src/editor/editor_level.cpp
        src/editor/editor_project.cpp
        src/editor/game_obj_manager.cpp
        src/editor/spline_manager.cpp
        src/editor/tile_brush_manager.cpp
        src/editor/tile_layout_manager.cpp
        src/rom/culling_tables/animated_object_culling_table.cpp
        src/rom/culling_tables/game_obj_collision_culling_table.cpp
        src/rom/culling_tables/spline_culling_table.cpp
        src/rom/game_objects/game_object_definition.cpp
        src/rom/game_objects/game_object_flipper.cpp
        src/rom/game_objects/game_object_ring.cpp
        src/rom/animated_object.cpp
        src/rom/animation_sequence.cpp
        src/rom/collision_tile.cpp
        src/rom/colour.cpp
        src/rom/level.cpp
        src/rom/lzss_decompressor.cpp
        src/rom/compressed2_optimizer.cpp
        src/rom/bonus_stage_decoder.cpp
        src/rom/tails_plane_decoder.cpp
        src/rom/palette.cpp
        src/rom/rom_asset_definitions.cpp
        src/rom/rom_data.cpp
        src/rom/spinball_rom.cpp
        src/rom/sprite.cpp
        src/rom/sprite_tile.cpp
        src/rom/ssc_compressor.cpp
        src/rom/ssc_decompressor.cpp
        src/rom/tile.cpp
        src/rom/tile_brush.cpp
        src/rom/tile_layout.cpp
        src/rom/tileset.cpp
        src/types/blit_settings.cpp
        src/types/bounding_box.cpp
        src/ui/ui_animation_navigator.cpp
        src/ui/ui_editor.cpp
        src/ui/ui_editor_window.cpp
        src/ui/ui_file_selector.cpp
        src/ui/ui_image_io.cpp
        src/ui/ui_palette.cpp
        src/ui/ui_palette_viewer.cpp
        src/ui/ui_sprite.cpp
        src/ui/ui_sprite_importer.cpp
        src/ui/ui_sprite_navigator.cpp
        src/ui/ui_sprite_viewer.cpp
        src/ui/ui_tile_editor.cpp
        src/ui/ui_tile_layout_viewer.cpp
        src/ui/ui_tile_picker.cpp
        src/ui/ui_tileset_navigator.cpp
        src/main.cpp
        src/render.cpp
        src/platform/web_platform.cpp
        src/rom/metadata/rom_metadata.cpp
        src/serialisation/editor_serialiser.cpp)

target_include_directories(spintool PRIVATE
    # Both include styles are used by the project:
    #   <nlohmann/json.hpp>      -> external
    #   "json_fwd.hpp"          -> external/nlohmann
    external
    external/imgui
    external/imgui/backends
    external/imgui/misc/cpp
    external/imgui/misc/freetype
    external/imgui/misc/single_file
    external/nlohmann
    redist
    redist/editor
    redist/rom
    redist/rom/culling_tables
    redist/rom/game_objects
    redist/types
    redist/ui
)

target_link_libraries(spintool PRIVATE SDL3::SDL3)

if(EMSCRIPTEN)
    target_sources(spintool PRIVATE src/platform/web_sdl_image_compat.cpp)
    target_include_directories(spintool BEFORE PRIVATE web/compat)
    target_compile_definitions(spintool PRIVATE SPINTOOL_WEB=1)
    target_compile_options(spintool PRIVATE -fexceptions)
    target_link_options(spintool PRIVATE
        -fexceptions
        -sALLOW_MEMORY_GROWTH=1
        -sMAXIMUM_MEMORY=1073741824
        -sFORCE_FILESYSTEM=1
        -lidbfs.js
        -sEXIT_RUNTIME=0
        -sENVIRONMENT=web
        -sSTACK_SIZE=8388608
        -sDISABLE_EXCEPTION_CATCHING=0
        -sMIN_WEBGL_VERSION=2
        -sMAX_WEBGL_VERSION=2
        "SHELL:--shell-file ${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html"
    )
    set_target_properties(spintool PROPERTIES
        OUTPUT_NAME "index"
        SUFFIX ".html"
    )
else()
    target_link_libraries(spintool PRIVATE SDL3_image::SDL3_image)
endif()

if(NOT EMSCRIPTEN)
    if(WIN32)
        target_compile_definitions(spintool PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
        )
        set_target_properties(spintool PROPERTIES
            OUTPUT_NAME "spintool"
            SUFFIX ".exe"
        )
    else()
        set_target_properties(spintool PROPERTIES
            OUTPUT_NAME "spintool"
        )
    endif()
endif()

include(GNUInstallDirs)
install(TARGETS spintool
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
