# Measure the running game's successful window presents without restarting it.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Install,
    [ValidateRange(1,24)][int]$SampleWindows = 6,
    [string]$OutputPath,
    [switch]$ThreadCpu,
    [scriptblock]$OnMeasurementStart
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath $Install).Path
if (Test-Path -LiteralPath (Join-Path $root 'game/WindowsPlayer.exe')) {
    & (Join-Path $PSScriptRoot 'native_player/measure-live.ps1') -Install $root -SampleWindows $SampleWindows -OutputPath $OutputPath -ThreadCpu:$ThreadCpu -OnMeasurementStart $OnMeasurementStart
    return
}
$gamePath = Join-Path $root 'game/bunker-game-bootstrap.exe'
$games = @(Get-Process -Name bunker-game-bootstrap -ErrorAction SilentlyContinue |
    Where-Object Path -eq $gamePath)
if ($games.Count -ne 1) { throw "Expected one running game in $root. Found $($games.Count)." }
$log = Join-Path $root 'data/game.log'
if (!$OutputPath) { $OutputPath = Join-Path $root ("data/frame-profile-{0}.txt" -f [guid]::NewGuid()) }

$file = [IO.File]::Open($log, 'Open', 'Read', 'ReadWrite')
try {
    $reader = [IO.StreamReader]::new($file)
    $pollMs = 100
    if ($ThreadCpu) {
        $pollMs = 25
        if (-not ('ReincompFrameFocus' -as [type])) {
            Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class ReincompFrameFocus {
    [DllImport("user32.dll")] private static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    public static uint ForegroundProcess() {
        uint process;
        return GetWindowThreadProcessId(GetForegroundWindow(), out process) == 0 ? 0 : process;
    }
}
'@
        }
        $focusSamples = $foregroundSamples = $unknownFocus = 0
        # The game's startup log identifies the attached UnityMain within this process.
        # Read only as far as the first match in what can be a large log.
        $mainId = 0
        while ($null -ne ($line = $reader.ReadLine())) {
            if ($line -match '^Game socket import: .*? native_tid=(\d+) .*? lifetime=attached_main\b') {
                $mainId = [int]$Matches[1]
                break
            }
        }
        if (!$mainId) { throw "UnityMain thread identity is missing from $log" }
        $clock = [Diagnostics.Stopwatch]::StartNew()
        $reader.DiscardBufferedData()
    }
    [void]$file.Seek(0, 'End')
    $measurements = [Collections.Generic.List[string]]::new()
    $discardFirst = $true
    $deadline = [DateTime]::UtcNow.AddSeconds(60 + 6 * $SampleWindows)
    while ($measurements.Count -lt $SampleWindows) {
        if ($games[0].HasExited) { throw "The game exited during measurement. See $log" }
        if ([DateTime]::UtcNow -gt $deadline) { throw "Insufficient presentation samples. See $log" }
        $line = $reader.ReadLine()
        if ($null -eq $line) {
            if ($ThreadCpu) {
                $owner = [ReincompFrameFocus]::ForegroundProcess()
                $focusSamples++
                if (!$owner) { $unknownFocus++ }
                elseif ($owner -eq $games[0].Id) { $foregroundSamples++ }
            }
            Start-Sleep -Milliseconds $pollMs
            continue
        }
        if ($line -notlike 'Present rate:*frame ms p50=*p95=*p99=*max=*') { continue }
        if ($ThreadCpu) {
            if ($line -notmatch '\((\d+) presents in ([\d.]+)s\)') { throw "Missing presentation interval in $line" }
            $frames = [int]$Matches[1]
            $intervalMs = 1000 * [double]::Parse($Matches[2], [cultureinfo]::InvariantCulture)
            if ($frames -le 0) { throw 'Cannot measure CPU time without a successful window present.' }
            $games[0].Refresh()
            $main = $games[0].Threads | Where-Object Id -eq $mainId | Select-Object -First 1
            if (!$main) { throw "UnityMain thread $mainId exited during measurement." }
            $cpuMs = $main.TotalProcessorTime.TotalMilliseconds
            $wallMs = $clock.Elapsed.TotalMilliseconds
            if (!$discardFirst) {
                $cpuDelta = $cpuMs - $previousCpu
                $wallDelta = $wallMs - $previousWall
                # A delayed reader can drain multiple old records at once. In that case
                # the sampled CPU interval cannot be attributed to this set of presents.
                if ([math]::Abs($wallDelta - $intervalMs) -le 250) {
                    $line += [string]::Format([cultureinfo]::InvariantCulture,
                        ' | UnityMain CPU {0:F2} ms/frame ({1:F1}% of elapsed)', $cpuDelta / $frames, 100 * $cpuDelta / $wallDelta)
                } else {
                    $line += ' | UnityMain CPU unavailable (delayed sample)'
                }
            }
            $previousCpu = $cpuMs
            $previousWall = $wallMs
            $line += " | game_foreground_samples=$foregroundSamples/$focusSamples unknown=$unknownFocus"
            $focusSamples = $foregroundSamples = $unknownFocus = 0
        }
        # The first window starts before this measurement and can include capture readback.
        if ($discardFirst) {
            $discardFirst = $false
            if ($OnMeasurementStart) { & $OnMeasurementStart }
            continue
        }
        $measurements.Add($line)
        Write-Output $line
    }
} finally {
    $file.Dispose()
}
[IO.File]::WriteAllLines($OutputPath, $measurements)
$strict = [regex]::Matches(($measurements -join "`n"), '\bat_or_above_8_30ms=(\d+)/(\d+)\b')
$budgets = @(if ($strict.Count -eq $measurements.Count) { $strict } else {
    [regex]::Matches(($measurements -join "`n"), '\bover_120hz_budget=(\d+)/(\d+)\b')
})
if ($budgets.Count -eq $measurements.Count) {
    $late = 0
    $total = 0
    foreach ($budget in $budgets) {
        $late += [int]$budget.Groups[1].Value
        $total += [int]$budget.Groups[2].Value
    }
    if ($total -gt 0) {
        $limit = if ($strict.Count -eq $measurements.Count) { 'took 8.30 ms or longer' } else { 'exceeded the 8.33 ms budget for 120 FPS' }
        Write-Output ([string]::Format([cultureinfo]::InvariantCulture,
            '{0:N0} of {1:N0} frames ({2:F1}%) {3}.', $late, $total, 100.0 * $late / $total, $limit))
    }
}
Write-Output "Frame-time measurements: $OutputPath"
