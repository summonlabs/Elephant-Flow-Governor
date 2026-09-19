@echo off
rem Elephant Flow Governor - configure, build and test one configuration.
rem Copyright 2026 Summon Software Labs.
rem SPDX-License-Identifier: Apache-2.0
rem
rem Usage: scripts\build.cmd [Release|Debug] [build-directory] [extra cmake args]
rem
rem No test timeout is applied anywhere: ctest runs the suites plainly.

setlocal enabledelayedexpansion
call "%~dp0env.cmd" || exit /b 1

set "ROOT=%~dp0.."
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Release"
set "BUILD=%~2"
if "%BUILD%"=="" set "BUILD=%ROOT%\build"

echo EFG: configuring %CONFIG% in "%BUILD%"
cmake -S "%ROOT%" -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% %3 %4 %5 || exit /b 1
cmake --build "%BUILD%" || exit /b 1

pushd "%BUILD%"
ctest --output-on-failure
set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%
