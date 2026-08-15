@echo off
setlocal

cd /d "%~dp0"

rem O gerador padrao no Windows e o Visual Studio, mas o MSVC instalado aqui
rem (19.38) nao conhece cxx_std_26 e o configure falha em CMakeLists.txt:83.
rem Ninja + GCC (MinGW-w64) suporta C++26 e e single-config, entao a saida cai
rem em build\CppGame.exe - o mesmo caminho que o build.sh anuncia.
set "GENERATOR=Ninja"

rem O CMake se recusa a trocar de gerador sobre um cache existente. Se o build\
rem foi configurado com outro gerador, limpamos o cache principal e o das
rem arvores auxiliares do FetchContent (_deps\*-subbuild, que carregam o gerador
rem antigo tambem). Os _deps\*-src ficam intactos: nada e baixado de novo.
call :limpar_se_outro_gerador "build"
for /d %%D in ("build\_deps\*-subbuild") do call :limpar_se_outro_gerador "%%D"

cmake -S . -B build -G %GENERATOR% ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_C_COMPILER=gcc ^
    -DCMAKE_CXX_COMPILER=g++ ^
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON || exit /b 1

cmake --build build %* || exit /b 1

echo.
echo pronto: build\CppGame.exe
exit /b 0

rem Apaga o cache de uma arvore de build configurada por outro gerador. Preserva
rem tudo o que nao e cache - em especial os _deps\*-src ja clonados.
:limpar_se_outro_gerador
if not exist "%~1\CMakeCache.txt" exit /b 0
findstr /c:"CMAKE_GENERATOR:INTERNAL=%GENERATOR%" "%~1\CMakeCache.txt" >nul && exit /b 0
echo Cache de %~1 usa outro gerador - descartando.
del /q "%~1\CMakeCache.txt"
if exist "%~1\CMakeFiles" rmdir /s /q "%~1\CMakeFiles"
exit /b 0
