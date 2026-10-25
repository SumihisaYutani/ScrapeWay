@echo off
rem Build ScrapeWay and collect everything needed to run it into dist\ScrapeWay (also dist\ScrapeWay.zip)
setlocal
cd /d "%~dp0"

set QTDIR=C:\Qt\6.10.3\msvc2022_64
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || goto :error
set PATH=C:\Qt\Tools\CMake_64\bin;C:\Qt\Tools\Ninja;%QTDIR%\bin;%PATH%

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=%QTDIR% || goto :error
cmake --build build || goto :error

if exist dist rmdir /s /q dist
mkdir dist\ScrapeWay || goto :error
copy /y build\ScrapeWay.exe dist\ScrapeWay\ >nul || goto :error
windeployqt --release --no-translations --no-system-d3d-compiler --no-system-dxc-compiler --no-opengl-sw --no-compiler-runtime dist\ScrapeWay\ScrapeWay.exe || goto :error
rem Visual C++ runtime next to the exe so no separate install is needed
copy /y "%VCToolsRedistDir%x64\Microsoft.VC143.CRT\*.dll" dist\ScrapeWay\ >nul || goto :error
rem User manual (opened from Help menu)
copy /y docs\manual.html dist\ScrapeWay\ >nul || goto :error
powershell -NoProfile -Command "Compress-Archive -Path dist\ScrapeWay -DestinationPath dist\ScrapeWay.zip -Force" || goto :error

echo.
echo Done: dist\ScrapeWay\ScrapeWay.exe
exit /b 0

:error
echo.
echo Build failed.
exit /b 1
