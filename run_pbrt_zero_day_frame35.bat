@echo off
setlocal

set "ROOT=%~dp0"
set "SCENE=D:\models\pbrt-v4-scenes\zero-day\frame35.pbrt"

set "EXE=%ROOT%out\cmake-release\bin\Release\pbrt_kitchen.exe"
set "BUILD_DIR=%ROOT%out\cmake-release"


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
