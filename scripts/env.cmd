@echo off
rem Elephant Flow Governor - locate a C++ toolchain.
rem Copyright 2026 Summon Software Labs.
rem SPDX-License-Identifier: Apache-2.0
rem
rem Sourced by the other scripts. If a compiler is already on PATH nothing is
rem done; otherwise the newest Visual Studio C++ build environment is entered so
rem that a fresh clone can be validated from a plain shell.

where cl >nul 2>nul
if not errorlevel 1 goto :eof

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo EFG: no C++ compiler on PATH and vswhere.exe was not found. 1>&2
    exit /b 1
)

set "VSPATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"

if not defined VSPATH (
    echo EFG: no Visual Studio installation with the C++ toolset was found. 1>&2
    exit /b 1
)

call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo EFG: entering the Visual Studio environment failed. 1>&2
    exit /b 1
)
