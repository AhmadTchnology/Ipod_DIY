@echo off
cd /d "%~dp0"
python mypod_sync.py
if errorlevel 1 pause
