@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul 2>&1
cd /d "%~dp0"
cl /nologo /O2 /MT /W3 /D_CRT_SECURE_NO_WARNINGS /Fe:lerp_test.exe lerp_test.cpp /link kernel32.lib user32.lib
