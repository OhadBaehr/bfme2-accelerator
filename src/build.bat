@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul 2>&1
if errorlevel 1 ( echo vcvars32 failed & exit /b 1 )
cd /d "%~dp0"
echo === building aotr_accel.dll (32-bit) ===
cl /nologo /O2 /MT /W3 /I..\vendor\rpmalloc /c ..\vendor\rpmalloc\rpmalloc.c
if errorlevel 1 ( echo RPMALLOC BUILD FAILED & exit /b 5 )
cl /nologo /O2 /MT /W3 /LD /I..\vendor\rpmalloc /Fe:aotr_accel.dll aotr_accel.cpp rpmalloc.obj /link kernel32.lib user32.lib advapi32.lib /MAP
if errorlevel 1 ( echo DLL BUILD FAILED & exit /b 2 )
echo === building inject.exe (32-bit) ===
cl /nologo /O2 /MT /W3 /Fe:inject.exe inject.cpp /link kernel32.lib
if errorlevel 1 ( echo INJECTOR BUILD FAILED & exit /b 3 )
echo === building testload.exe (32-bit) ===
cl /nologo /O2 /MT /W3 /Fe:testload.exe testload.cpp /link kernel32.lib
if errorlevel 1 ( echo TESTLOAD BUILD FAILED & exit /b 4 )
echo === done ===
dir /b aotr_accel.dll inject.exe testload.exe
