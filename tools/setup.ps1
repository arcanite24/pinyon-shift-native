# Set up the drive to play: see tools/setup.py (README, Play the drive).
$py = if (Get-Command python -ErrorAction SilentlyContinue) { "python" } else { "py" }
& $py "$PSScriptRoot/setup.py" @args
exit $LASTEXITCODE
