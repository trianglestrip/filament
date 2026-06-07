@echo off
setlocal

set "ROOT=%~dp0"
set "BUILD_DIR=%ROOT%out\cmake-release"
set "EXE=%BUILD_DIR%\bin\Release\pbrt_viewer.exe"
set "SCENE=D:\models\pbrt-v4-scenes\barcelona-pavilion\pavilion-day.pbrt"

if not exist "%EXE%" (
    echo [INFO] pbrt_viewer.exe not found. Building Release target...
    cmake --build "%BUILD_DIR%" --config Release --target pbrt_viewer -- /m:1
    if errorlevel 1 (
        echo [ERROR] Build failed.
        exit /b 1
    )
)

if "%~1"=="" (
    if not exist "%SCENE%" (
        echo [ERROR] Default scene not found:
        echo         %SCENE%
        exit /b 1
    )
    start "" "%EXE%" --preview --scene "%SCENE%"
) else (
    start "" "%EXE%" %*
)

endlocal
