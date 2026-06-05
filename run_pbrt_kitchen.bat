@echo off
setlocal

set "ROOT=%~dp0"
set "EXE=%ROOT%out\bin\Release\pbrt_kitchen.exe"
set "SCENE=D:\gitProject\VLR_WF\models\kitchen\scene-v4.pbrt"

if not exist "%EXE%" (
    echo [INFO] pbrt_kitchen.exe not found. Building Release target...
    cmake --build "%ROOT%out" --config Release --target pbrt_kitchen -- /m:1
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
