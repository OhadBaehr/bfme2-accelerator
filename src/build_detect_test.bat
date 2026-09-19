@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul 2>&1
cd /d "%~dp0"
cl /nologo /O2 /MT /W3 /DBFME2_DETECT_TEST /Fe:detect_test.exe launcher.cpp /link kernel32.lib user32.lib advapi32.lib gdi32.lib gdiplus.lib ole32.lib comdlg32.lib comctl32.lib
