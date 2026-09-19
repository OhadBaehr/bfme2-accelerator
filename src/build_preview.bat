@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul 2>&1
cd /d "%~dp0"
call "%~dp0prepare_art.bat"
rc /nologo /fo launcher.res launcher.rc
cl /nologo /O2 /MT /W3 /DBFME2_PREVIEW /Fe:preview.exe launcher.cpp launcher.res /link kernel32.lib user32.lib advapi32.lib gdi32.lib gdiplus.lib ole32.lib comdlg32.lib
