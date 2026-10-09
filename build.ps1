param([switch]$Report, [string]$CrtPath)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path $PSScriptRoot).Path
$out = Join-Path $root ('build/accel-' + $(if ($Report) { 'report' } else { 'prod' }))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (!(Test-Path -LiteralPath $vswhere)) { throw 'Install Visual Studio with the C++ desktop workload.' }
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'No Visual Studio x86 C++ toolchain found.' }
$vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvars32.bat'
New-Item -ItemType Directory -Force -Path $out | Out-Null
# The batch file only bootstraps MSVC. All output stays in the repository build directory.
$definitions = if ($Report) { '' } else { '/DAOTR_PROD' }
$batch = @"
@echo off
call "$vcvars" >nul
if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /W3 /WX /I"$PSScriptRoot/vendor/rpmalloc" /c "$PSScriptRoot/vendor/rpmalloc/rpmalloc.c"
if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /W3 /WX $definitions /LD /I"$PSScriptRoot/vendor/rpmalloc" /Fe:bfme2_accel.dll "$PSScriptRoot/src/aotr_accel.cpp" rpmalloc.obj /link kernel32.lib user32.lib advapi32.lib /MAP
if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /W3 /WX /DAOTR_PROD /I"$PSScriptRoot/vendor/rpmalloc" /Fe:compat_test.exe "$PSScriptRoot/src/compat_test.cpp" rpmalloc.obj /link kernel32.lib user32.lib advapi32.lib
if errorlevel 1 exit /b 1
compat_test.exe
if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /W3 /WX /Fe:crt_test.exe "$PSScriptRoot/src/crt_test2.cpp" /link kernel32.lib
if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /W3 /WX /Fe:testload.exe "$PSScriptRoot/src/testload.cpp" /link kernel32.lib
if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /W3 /WX /Fe:api_test.exe "$PSScriptRoot/src/api_test.cpp" /link kernel32.lib
if errorlevel 1 exit /b 1
api_test.exe
if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /W3 /WX /Fe:inject.exe "$PSScriptRoot/src/inject.cpp" /link kernel32.lib
if errorlevel 1 exit /b 1
call "$PSScriptRoot/src/prepare_art.bat"
if errorlevel 1 exit /b 1
pushd "$PSScriptRoot/src"
rc /nologo /fo "$out/launcher.res" launcher.rc
if errorlevel 1 exit /b 1
popd
cl /nologo /O2 /MT /W3 /WX /Fe:bfme2_accel_loader.exe "$PSScriptRoot/src/launcher.cpp" launcher.res /link kernel32.lib user32.lib advapi32.lib gdi32.lib gdiplus.lib ole32.lib comdlg32.lib /SUBSYSTEM:WINDOWS /MANIFEST:NO
if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /W3 /WX /Fe:rt_harness.exe "$PSScriptRoot/src/rt_harness.cpp" /link kernel32.lib user32.lib
exit /b %errorlevel%
"@
$script = Join-Path $out 'build.cmd'
Set-Content -LiteralPath $script -Value $batch -Encoding ascii
Push-Location $out
try {
    & $script
    if ($LASTEXITCODE -ne 0) { throw "Accelerator build/test failed with exit code $LASTEXITCODE" }
    if ($CrtPath) {
        Copy-Item -LiteralPath (Resolve-Path -LiteralPath $CrtPath).Path -Destination (Join-Path $out 'msvcr71.dll') -Force
        & (Join-Path $out 'crt_test.exe')
        if ($LASTEXITCODE -ne 0) { throw "CRT comparison failed with exit code $LASTEXITCODE" }
    }
} finally { Pop-Location }
Write-Output "Built and tested: $(Join-Path $out 'bfme2_accel.dll')"
