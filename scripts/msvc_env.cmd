@echo off
rem ============================================================
rem  Qingzhou RPC - local MSVC environment (no vcvars64.bat)
rem
rem  Usage: call scripts\msvc_env.cmd
rem  Why:   reg.exe is blocked by endpoint security on this host, so
rem         vcvars64.bat cannot locate the Windows SDK and cl.exe
rem         fails with "cannot open include file: stdio.h".
rem  NOTE:  keep this file ASCII-only. cmd.exe parses .cmd as the
rem         OEM code page (936 here); UTF-8 Chinese breaks parsing.
rem ============================================================
set "QZ_VC=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231"
set "QZ_SDK=C:\Program Files (x86)\Windows Kits\10"
set "QZ_SDKV=10.0.26100.0"

if not exist "%QZ_VC%\bin\Hostx64\x64\cl.exe" (
  echo [msvc_env] ERROR: cl.exe not found at %QZ_VC%
  exit /b 1
)

set "INCLUDE=%QZ_VC%\include;%QZ_SDK%\Include\%QZ_SDKV%\ucrt;%QZ_SDK%\Include\%QZ_SDKV%\um;%QZ_SDK%\Include\%QZ_SDKV%\shared;%QZ_SDK%\Include\%QZ_SDKV%\winrt;%QZ_SDK%\Include\%QZ_SDKV%\cppwinrt"
set "LIB=%QZ_VC%\lib\x64;%QZ_SDK%\Lib\%QZ_SDKV%\ucrt\x64;%QZ_SDK%\Lib\%QZ_SDKV%\um\x64"
rem SDK bin is needed for rc.exe / mt.exe (CMake's link check requires rc)
set "PATH=%QZ_VC%\bin\Hostx64\x64;%QZ_SDK%\bin\%QZ_SDKV%\x64;%PATH%"

rem CMake / Ninja shipped with Visual Studio
set "QZ_CMAKE=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake"
if exist "%QZ_CMAKE%\CMake\bin\cmake.exe" set "PATH=%QZ_CMAKE%\CMake\bin;%PATH%"
if exist "%QZ_CMAKE%\Ninja\ninja.exe" set "PATH=%QZ_CMAKE%\Ninja;%PATH%"
exit /b 0
