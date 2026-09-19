@echo off
rem build_variant.bat <outname.dll> [extra cl /D switches...]  - bisecting builds of aotr_accel
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul 2>&1
cd /d "%~dp0"
set OUT=%1
shift
set DEFS=
:loop
if "%~1"=="" goto build
set DEFS=%DEFS% %1
shift
goto loop
:build
cl /nologo /O2 /MT /W3 /LD /I..\vendor\rpmalloc %DEFS% /Fe:%OUT% /Fo:variant_obj\ aotr_accel.cpp rpmalloc.obj /link kernel32.lib user32.lib advapi32.lib
