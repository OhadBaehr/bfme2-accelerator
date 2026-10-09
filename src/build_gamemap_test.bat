@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul 2>&1
cd /d "%~dp0"
cl /nologo /O2 /MT /W3 /EHsc /Fe:gamemap_test.exe gamemap_test.cpp /link kernel32.lib /BASE:0x70000000 /FIXED /DYNAMICBASE:NO
