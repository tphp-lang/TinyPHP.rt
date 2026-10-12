@echo off
rem TinyPHP.rt test runner wrapper. ASCII only.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run.ps1"
exit /b %ERRORLEVEL%
