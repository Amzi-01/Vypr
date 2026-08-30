@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d C:\vypr\guest
if not exist build mkdir build
cd build
cl /nologo /std:c++20 /EHsc /W4 /permissive- /Zc:__cplusplus /bigobj ^
   /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
   /I C:\vypr\guest\src /I C:\vypr\include ^
   /Fe:vypr-agent.exe ^
   ..\src\main.cpp ..\src\audio.cpp ..\src\capture.cpp ..\src\control.cpp ^
   ..\src\input.cpp ^
   ..\src\clipboard.cpp ..\src\drop.cpp ..\src\gamepad.cpp ..\src\ivshmem.cpp ..\src\publisher.cpp ^
   ..\src\windows_list.cpp ^
   /link d3d11.lib dxgi.lib windowsapp.lib setupapi.lib ws2_32.lib dwmapi.lib user32.lib gdi32.lib ole32.lib shell32.lib
echo BUILD_EXIT=%ERRORLEVEL%
