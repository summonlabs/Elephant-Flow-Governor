@echo off
rem Elephant Flow Governor - fresh clone closure check.
rem Copyright 2026 Summon Software Labs.
rem SPDX-License-Identifier: Apache-2.0
rem
rem Clones the repository from its own committed history into a temporary
rem directory, then configures, builds, tests and consumes it there. Anything the
rem build needs that was never committed shows up as a failure here.

setlocal
call "%~dp0env.cmd" || exit /b 1

set "ROOT=%~dp0.."
set "CLONE=%TEMP%\efg-fresh-clone-%RANDOM%%RANDOM%"

echo EFG: cloning into "%CLONE%"
git clone --quiet "%ROOT%" "%CLONE%" || exit /b 1
pushd "%CLONE%"

git rev-parse HEAD || goto :fail

echo EFG: configuring the fresh clone
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release || goto :fail
echo EFG: building the fresh clone
cmake --build build || goto :fail
echo EFG: testing the fresh clone
pushd build
ctest --output-on-failure || (popd & goto :fail)
popd

echo EFG: installing the fresh clone
cmake --install build --prefix build\install || goto :fail

echo EFG: consuming the fresh clone
cmake -S examples\downstream_consumer -B build\consumer -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=%CLONE%\build\install" || goto :fail
cmake --build build\consumer || goto :fail
build\consumer\efg_consumer.exe || goto :fail

popd
rmdir /s /q "%CLONE%"
echo EFG: fresh clone closure verified
exit /b 0

:fail
set "RESULT=%ERRORLEVEL%"
popd
rmdir /s /q "%CLONE%"
echo EFG: fresh clone closure FAILED 1>&2
exit /b %RESULT%
