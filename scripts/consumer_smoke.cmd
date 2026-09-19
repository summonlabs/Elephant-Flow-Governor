@echo off
rem Elephant Flow Governor - install the package and build an independent consumer.
rem Copyright 2026 Summon Software Labs.
rem SPDX-License-Identifier: Apache-2.0
rem
rem This is the closure proof for the exported CMake package: the consumer is a
rem separate project that only knows what find_package(EFG) publishes.

setlocal
call "%~dp0env.cmd" || exit /b 1

set "ROOT=%~dp0.."
set "WORK=%ROOT%\build-consumer-smoke"
set "PREFIX=%WORK%\install"
set "CONSUMER=%WORK%\consumer"

if exist "%WORK%" rmdir /s /q "%WORK%"

echo EFG: building and installing the package into "%PREFIX%"
cmake -S "%ROOT%" -B "%WORK%\governor" -G Ninja -DCMAKE_BUILD_TYPE=Release -DEFG_BUILD_TESTS=OFF || exit /b 1
cmake --build "%WORK%\governor" || exit /b 1
cmake --install "%WORK%\governor" --prefix "%PREFIX%" || exit /b 1

echo EFG: configuring the independent consumer against the installed package
cmake -S "%ROOT%\examples\downstream_consumer" -B "%CONSUMER%" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=%PREFIX%" || exit /b 1
cmake --build "%CONSUMER%" || exit /b 1

echo EFG: running the consumer
"%CONSUMER%\efg_consumer.exe"
exit /b %ERRORLEVEL%
