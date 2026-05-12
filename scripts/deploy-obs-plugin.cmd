@echo off
setlocal

REM Convenience wrapper for deploy-obs-plugin.ps1
set SCRIPT_DIR=%~dp0
powershell -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%deploy-obs-plugin.ps1" %*

