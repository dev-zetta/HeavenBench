@echo off
setlocal
if not defined HEAVEN_USER_DIR set "HEAVEN_USER_DIR=%LOCALAPPDATA%\HeavenBench"
if not exist "%HEAVEN_USER_DIR%\." mkdir "%HEAVEN_USER_DIR%"
if not exist "%HEAVEN_USER_DIR%\." (
    echo Cannot create writable user directory: "%HEAVEN_USER_DIR%" 1>&2
    exit /b 1
)
set "HEAVEN_RESOURCE_DIR=%~dp0resources"
if not defined HEAVEN_DEFAULT_DATA set "HEAVEN_DEFAULT_DATA=%~dp0assets"
rem The CLI parser applies repeated options in order, so explicit --data wins.
if defined HEAVEN_DATA goto with_data
"%~dp0heaven_benchmark.exe" %*
exit /b %errorlevel%
:with_data
"%~dp0heaven_benchmark.exe" --data "%HEAVEN_DATA%" %*
exit /b %errorlevel%
