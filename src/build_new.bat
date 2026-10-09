@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul 2>&1
cd /d "%~dp0"
rc /nologo /fo version.res version.rc
cl /nologo /O2 /MT /W3 /LD /I..\vendor\rpmalloc /Fe:bfme2_accel.new.dll aotr_accel.cpp rpmalloc.obj version.res /link kernel32.lib user32.lib advapi32.lib /MAP
