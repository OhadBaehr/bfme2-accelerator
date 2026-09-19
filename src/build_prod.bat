@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul 2>&1
cd /d "%~dp0"
cl /nologo /O2 /MT /W3 /DAOTR_PROD /LD /I..\vendor\rpmalloc /Fe:bfme2_accel.new.dll aotr_accel.cpp rpmalloc.obj /link kernel32.lib user32.lib advapi32.lib /MAP
