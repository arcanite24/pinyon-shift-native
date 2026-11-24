# Launches carview.exe in Playtest Session & Bug Recorder mode.
#
# Usage:
#   .\tools\launch-playtest.ps1 [extra carview args...]
#   .\tools\launch-playtest.ps1 -PlayDir "D:\path\to\play" --car NIS_SkylineGTRVSpecII_02 --hour 18
#
# Controls during playtest:
#   F11 (keyboard) or Back + RB (gamepad): instant bug bookmark (bug_00N.png + bug_00N.json + repro_cmd)
#
# Outputs under out/playtests/YYYYMMDD-HHMMSS/:
#   session.log      stdout + stderr (missing textures, warnings, errors)
#   telemetry.jsonl  4 Hz periodic + every hitch (>25 ms), ground anomaly, audio glitch, and user mark
#   bug_00N.png/.json  user-marked bug screenshots + state snapshots + copy-pasteable repro CLI
#   summary.json     auto-generated exit summary (p50/p95/p99/max ms, top 20 hitches, anomalies, bugs)
[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$PlayDir = (Join-Path $env:APPDATA 'shift-engine\play'),
    [string]$Exe = '',
    [string]$OutRoot = '',
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ExtraArgs = @()
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

if (-not $Exe) {
    $candidates = @(
        (Join-Path $root 'build\apps\carview\carview.exe'),
        (Join-Path $root 'build\apps\carview\Release\carview.exe')
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) {
            $Exe = $c
            break
        }
    }
    if (-not $Exe) {
        throw "carview.exe not found (checked $($candidates -join ', ')). Build first with .\tools\build-windows.ps1"
    }
} elseif (-not (Test-Path $Exe)) {
    throw "carview.exe not found at: $Exe"
}

if (-not $OutRoot) {
    $OutRoot = Join-Path $root 'out\playtests'
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$sessionDir = Join-Path $OutRoot $stamp
if (Test-Path $sessionDir) {
    $suffix = 1
    while (Test-Path "${sessionDir}_$suffix") { $suffix++ }
    $sessionDir = "${sessionDir}_$suffix"
}
New-Item -ItemType Directory -Path $sessionDir -Force | Out-Null

$logPath = Join-Path $sessionDir 'session.log'
$cmdArgs = @('--drive', '--play', $PlayDir, '--record-session', $sessionDir)
if ($ExtraArgs -and $ExtraArgs.Count -gt 0) {
    $cmdArgs += $ExtraArgs
}

Write-Host "============================================================"
Write-Host "Shift Engine Playtest Recorder"
Write-Host "  Executable : $Exe"
Write-Host "  Play dir   : $PlayDir"
Write-Host "  Session dir: $sessionDir"
Write-Host "  Log file   : $logPath"
Write-Host "  Bug mark   : F11 (keyboard) or Back + RB (gamepad)"
Write-Host "============================================================"

$prevEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    & $Exe @cmdArgs 2>&1 | ForEach-Object { "$_" } | Tee-Object -FilePath $logPath
    $exitCode = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $prevEap
}

Write-Host ""
Write-Host "============================================================"
Write-Host "Playtest session saved to:"
Write-Host "  $sessionDir"
Write-Host "Files:"
Write-Host "  Log      : $logPath"

$telemetryPath = Join-Path $sessionDir 'telemetry.jsonl'
if (Test-Path $telemetryPath) {
    Write-Host "  Telemetry: $telemetryPath"
}

$summaryPath = Join-Path $sessionDir 'summary.json'
if (Test-Path $summaryPath) {
    Write-Host "  Summary  : $summaryPath"
    try {
        $sum = Get-Content -Path $summaryPath -Raw | ConvertFrom-Json
        Write-Host ("  Metrics  : {0} frames, {1:N1}s | p50={2:N2}ms p95={3:N2}ms p99={4:N2}ms max={5:N2}ms" -f `
            $sum.total_frames, $sum.duration_s, $sum.p50_ms, $sum.p95_ms, $sum.p99_ms, $sum.max_ms)
        Write-Host ("  Issues   : hitches(>25ms)={0}, ground_anomalies={1}, audio_glitches={2}, bugs_marked={3}" -f `
            $sum.hitch_count, $sum.ground_anomaly_count, $sum.audio_glitch_count, $sum.bugs.Count)
    } catch {
        Write-Host "  (could not parse summary.json preview: $_)"
    }
}

$bugFiles = Get-ChildItem -Path $sessionDir -Filter 'bug_*' -ErrorAction SilentlyContinue | Sort-Object Name
if ($bugFiles -and $bugFiles.Count -gt 0) {
    Write-Host "  Bookmarks:"
    foreach ($bf in $bugFiles) {
        Write-Host "    $($bf.FullName)"
    }
}
Write-Host "============================================================"

exit $exitCode
