cmake_minimum_required(VERSION 3.24)

project(CppGame LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
                 Debug Release RelWithDebInfo MinSizeRel)
endif()

include(FetchContent)

set(FETCHCONTENT_QUIET OFF)

set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES    OFF CACHE BOOL "" FORCE)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

FetchContent_Declare(
    raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG 6.0
    GIT_SHALLOW 1
    GIT_PROGRESS TRUE
)
FetchContent_Declare(
    raylib_cpp
    GIT_REPOSITORY https://github.com/RobLoach/raylib-cpp.git
    GIT_TAG v6.0.3
    GIT_SHALLOW 1
    GIT_PROGRESS TRUE
)

# raygui: biblioteca de UI imediata, header-only, do mesmo autor da raylib.
# SOURCE_SUBDIR aponta para um diretorio inexistente de proposito - assim o
# FetchContent baixa o repositorio mas NAO faz add_subdirectory, o que evitaria
# arrastar junto os exemplos e ferramentas dele. So o cabecalho interessa.
FetchContent_Declare(
    raygui
    GIT_REPOSITORY https://github.com/raysan5/raygui.git
    GIT_TAG 4.0
    GIT_SHALLOW 1
    GIT_PROGRESS TRUE
    SOURCE_SUBDIR nao-configurar
)

FetchContent_MakeAvailable(raylib raylib_cpp raygui)

add_executable(${PROJECT_NAME}
    src/main.cpp
    src/grid.cpp
    src/marching_squares.cpp
    src/noise_view.cpp
    src/debug_view.cpp
    src/canopy.cpp
    src/editor.cpp
    src/flora.cpp
    src/grove.cpp
    src/hotbar.cpp
    src/light.cpp
    src/light_layer.cpp
    src/liquid_layer.cpp
    src/player.cpp
    src/water.cpp
    src/weather.cpp
    src/world.cpp
    src/terrain.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE
    src
    ${raygui_SOURCE_DIR}/src
    # stb_perlin.h is the noise generator raylib uses internally, but raylib's
    # public API does not expose its parameters (octaves, lacunarity, gain,
    # seed). The header contributes declarations only; the implementation is
    # already compiled into libraylib, so nothing is rebuilt.
    ${raylib_SOURCE_DIR}/src/external
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_26)

target_link_libraries(${PROJECT_NAME} PRIVATE raylib_cpp raylib)

# Assets are loaded at runtime through paths relative to the executable, which
# switches its working directory to its own location at startup.
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/assets
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/assets
    COMMENT "Copying assets next to the executable"
)
