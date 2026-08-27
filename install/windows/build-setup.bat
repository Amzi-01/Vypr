@echo off
REM Builds vypr-setup.exe. Run after build-agent.bat: the agent is embedded in
REM the installer, so it has to exist first.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d C:\vypr\install\windows

if not exist vypr-agent.exe (
    echo MISSING: vypr-agent.exe - run build-agent.bat first, then copy it here.
    exit /b 1
)

rc /nologo /fo vypr-setup.res vypr-setup.rc
if errorlevel 1 exit /b 1

cl /nologo /std:c++20 /EHsc /W4 /O2 /permissive- /Zc:__cplusplus ^
   /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
   /Fe:vypr-setup.exe vypr-setup.cpp vypr-setup.res ^
   /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib ole32.lib shlwapi.lib
echo BUILD_EXIT=%ERRORLEVEL%
