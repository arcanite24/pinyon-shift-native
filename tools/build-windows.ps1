# Configures and builds shift-engine on Windows with MSVC and Ninja.
#   .\tools\build-windows.ps1 [-BuildDir build] [-Config Release] [-Clean]
# Enters the newest Visual Studio developer environment (x64) itself, so it
# runs from a plain PowerShell. CMake 3.25+ and Ninja must be on PATH.
param(
    [string]$BuildDir = 'build',
    [string]$Config = 'Release',
    [switch]$Clean,
    [string]$Target = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { throw 'vswhere.exe not found: install Visual Studio Build Tools' }
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'no Visual Studio with the C++ x64 tools found' }
    $devCmd = Join-Path $vs 'Common7\Tools\VsDevCmd.bat'
    $lines = & cmd.exe /d /s /c "`"$devCmd`" -arch=x64 -host_arch=x64 -no_logo >nul && set"
    foreach ($line in $lines) {
        if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "Env:$($Matches[1])" -Value $Matches[2] }
    }
}

$build = Join-Path $root $BuildDir
if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }
cmake -S $root -B $build -G Ninja "-DCMAKE_BUILD_TYPE=$Config" -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl
if ($LASTEXITCODE) { exit $LASTEXITCODE }
if ($Target) { cmake --build $build --target $Target } else { cmake --build $build }
exit $LASTEXITCODE
