@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul 2>&1
cd /d "%~dp0"
cl /nologo /O2 /MT /EHsc /W3 /Fe:vblank_probe.exe vblank_probe.cpp /link kernel32.lib user32.lib gdi32.lib
