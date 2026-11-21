# Measure the native player already running in the specified installation.
param([string]$Install, [int]$SampleWindows, [string]$OutputPath, [switch]$ThreadCpu, [scriptblock]$OnMeasurementStart)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'presentmon.ps1')
$monitor = Find-PresentMon
$exe = Join-Path $Install 'game/WindowsPlayer.exe'
$matchesByPath = @(Get-Process WindowsPlayer -ErrorAction SilentlyContinue | Where-Object Path -eq $exe)
if ($matchesByPath.Count -ne 1) { throw "Expected one running Windows player in $Install. Found $($matchesByPath.Count)." }
$player = $matchesByPath[0]
$id = [guid]::NewGuid().ToString('N')
if (!$OutputPath) { $OutputPath = Join-Path $Install "data/benchmarks/live-$id.txt" }
$output = [IO.Path]::GetFullPath($OutputPath)
$dir = Split-Path -Parent $output
New-Item -ItemType Directory -Path $dir -Force | Out-Null
$base = Join-Path $dir ("live-" + $id)
$session = 'ReincompLive' + $id
$seconds = 5 * ($SampleWindows + 2) + 3
$capture = Start-PlayerCapture $monitor $player.Id $base $session $seconds
try {
    if ($OnMeasurementStart) { & $OnMeasurementStart }
    $limit = [Diagnostics.Stopwatch]::StartNew()
    while (!$capture.WaitForExit(250) -and !$player.HasExited -and $limit.Elapsed.TotalSeconds -lt $seconds + 20) {}
} finally {
    if (!$capture.HasExited) {
        & logman stop $session -ets | Out-Null
        if (!$capture.WaitForExit(8000)) { $capture.Kill(); $capture.WaitForExit() }
    }
}
if ($capture.ExitCode) { throw "PresentMon exited $($capture.ExitCode). See $base.stderr." }
Copy-Item -LiteralPath (Join-Path $Install 'game/frame-state.log') -Destination ($base+'.frames.log')
if (!(Test-Path -LiteralPath ($base+'.csv') -PathType Leaf)) {
    throw "PresentMon recorded no presentations from player $($player.Id). Check that the Windows session is unlocked and the game is visible. See $base.stderr."
}
$py = Join-Path $Install 'pods/python/python.exe'
$options = @(($base+'.csv'), '--live-windows', [string]$SampleWindows)
if ($ThreadCpu) { $options += '--thread-cpu' }
$lines = & $py (Join-Path $PSScriptRoot 'summarize_present.py') @options
if ($LASTEXITCODE) { throw "Could not summarize the presentation capture $base.csv." }
[IO.File]::WriteAllLines($output, [string[]]$lines)
$lines
Write-Output "Frame-time measurements: $output"
Write-Output "Presentation capture: $base.csv"
