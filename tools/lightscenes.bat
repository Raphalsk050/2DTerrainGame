@echo off
setlocal

rem Builds and runs the light test bank, and puts the pictures somewhere you can
rem look at them. Arguments pass straight through:
rem
rem   tools\lightscenes.bat                     everything, into build\scenes
rem   tools\lightscenes.bat --checks            the closed-form checks alone
rem   tools\lightscenes.bat --only 08           one scene, by name
rem   tools\lightscenes.bat --out C:\somewhere  pictures elsewhere
rem
rem This needs a graphics device: the solver is compute shaders and wants an
rem OpenGL 4.3 context, so it opens a window and ignores it. Over a remote desktop
rem that forwards to a software renderer, expect it to be slow or to refuse.

cd /d "%~dp0.."

rem Same generator and compiler as build.bat, for the same reason: MSVC 19.38 does
rem not know cxx_std_26 and the configure step fails outright.
if not exist "build\CMakeCache.txt" (
    cmake -S . -B build -G Ninja ^
        -DCMAKE_BUILD_TYPE=Release ^
        -DCMAKE_C_COMPILER=gcc ^
        -DCMAKE_CXX_COMPILER=g++ ^
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON || exit /b 1
)

cmake --build build --target LightScenes || exit /b 1

if not exist "build\scenes" mkdir "build\scenes"

build\LightScenes.exe %* || exit /b 1

echo.
echo pronto: build\scenes\00-contact-sheet.png
exit /b 0
