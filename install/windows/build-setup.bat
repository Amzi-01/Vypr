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

REM The interface runs in WebView2. Its SDK is the Microsoft.Web.WebView2 NuGet
REM package, unzipped into webview2\ here: only the headers and the static
REM loader are used, so nothing extra ships beside the exe.
if not exist webview2\build\native\include\WebView2.h (
    powershell -NoProfile -Command "Invoke-WebRequest https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2 -OutFile wv2.zip -UseBasicParsing; Expand-Archive wv2.zip -DestinationPath webview2 -Force"
)

cl /nologo /std:c++20 /EHsc /W4 /O2 /permissive- /Zc:__cplusplus ^
   /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
   /I webview2\build\native\include ^
   /Fe:vypr-setup.exe vypr-setup.cpp vypr-setup.res ^
   /link /SUBSYSTEM:WINDOWS /LIBPATH:webview2\build\native\x64 ^
   user32.lib gdi32.lib ole32.lib shlwapi.lib
echo BUILD_EXIT=%ERRORLEVEL%
