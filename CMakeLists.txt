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

# A luz e resolvida por compute shader, e compute shader e OpenGL 4.3.
#
# A raylib compila para 3.3 por padrao, e a 3.3 nao tem nem `rlLoadComputeShaderProgram`
# nem SSBO: os simbolos existem no cabecalho mas o corpo inteiro esta atras de
# `#if defined(GRAPHICS_API_OPENGL_43)` e vira uma chamada vazia que devolve zero.
# O sintoma disso nao e um erro de link, e um shader que "carrega" com id 0 e um
# buffer que le zeros - por isso o valor esta fixado aqui e nao deixado ao acaso.
#
# 4.3 e de 2012 e nao restringe nada na pratica; a maquina alvo aqui e uma RTX 3070.
set(OPENGL_VERSION "4.3" CACHE STRING "" FORCE)

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
    src/drop.cpp
    src/editor.cpp
    src/fixture.cpp
    src/flora.cpp
    src/grove.cpp
    src/hotbar.cpp
    src/inventory.cpp
    src/backdrop.cpp
    src/light.cpp
    src/picture.cpp
    src/liquid_layer.cpp
    src/lit_layer.cpp
    src/player.cpp
    src/scuff.cpp
    src/console.cpp
    src/water.cpp
    src/weather.cpp
    src/soil.cpp
    src/sod.cpp
    src/world.cpp
    src/cave.cpp
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

# O runtime do compilador vai dentro do executavel, nao ao lado dele.
#
# Sem isto o binario importa libstdc++-6.dll, libgcc_s_seh-1.dll e
# libwinpthread-1.dll, que sao do MinGW-w64 e nao existem numa maquina que nunca
# instalou o toolchain. O jogo abre aqui e nao abre na maquina de mais ninguem,
# com uma caixa de erro sobre uma DLL ausente e nada que explique o que fazer.
#
# A raylib ja e estatica (libraylib.a), entao o que sobra depois disto sao apenas
# DLLs do proprio Windows: kernel32, user32, gdi32, shell32, winmm, opengl32 e o
# UCRT (api-ms-win-crt-*), que acompanha o Windows 10 em diante.
#
# Verificavel, e vale verificar depois de trocar de toolchain:
#
#     objdump -p build/CppGame.exe | grep "DLL Name"
#
# Se aparecer qualquer libgcc/libstdc++/libwinpthread nessa lista, o zip nao roda
# na maquina do outro.
if(MINGW)
    target_link_options(${PROJECT_NAME} PRIVATE -static)
endif()

# A bank of scenes for testing the light, built on its own so that running it does
# not mean starting the game and walking somewhere. Not part of the default build:
# `cmake --build build --target LightScenes`, or tools/lightscenes.bat.
#
# It links only the solver, deliberately. The whole value of it is that it exercises
# the light against scenes whose answer is known, and pulling the world in would
# make its answers depend on the generator.
add_executable(LightScenes EXCLUDE_FROM_ALL
    tools/light_scenes.cpp
    src/light.cpp
)

target_include_directories(LightScenes PRIVATE
    src
    ${raylib_SOURCE_DIR}/src/external
)

target_compile_features(LightScenes PRIVATE cxx_std_26)
target_link_libraries(LightScenes PRIVATE raylib)

# Assets are loaded at runtime through paths relative to the executable, which
# switches its working directory to its own location at startup.
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/assets
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/assets
    COMMENT "Copying assets next to the executable"
)

# Uma pasta pronta para zipar: `cmake --build build --target dist`.
#
# Existe porque build/ nao serve para isso. Sao 430 MB, quase tudo _deps/ com o
# codigo-fonte da raylib e os objetos intermediarios, e quem recebe o zip nao
# precisa de nada disso. dist/ tem o executavel e os assets, e mais nada.
#
# Recriada a cada vez em vez de acumular: um asset renomeado deixaria a copia
# antiga para tras, e o jogo carregaria a errada no computador de outra pessoa
# sem carregar aqui.
add_custom_target(dist
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_CURRENT_SOURCE_DIR}/dist
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_SOURCE_DIR}/dist
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PROJECT_NAME}> ${CMAKE_CURRENT_SOURCE_DIR}/dist/
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/assets ${CMAKE_CURRENT_SOURCE_DIR}/dist/assets
    DEPENDS ${PROJECT_NAME}
    COMMENT "Montando dist/ para distribuicao"
    VERBATIM
)
