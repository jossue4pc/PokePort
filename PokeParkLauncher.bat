@echo off
rem Opens the PokePark Wii PC launcher (Windows PowerShell + WPF, included with Windows 10/11).
start "" powershell.exe -NoProfile -ExecutionPolicy Bypass -STA -WindowStyle Hidden -File "%~dp0launcher\PokeParkLauncher.ps1"
