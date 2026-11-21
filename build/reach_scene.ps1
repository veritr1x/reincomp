# Start the installed game and leave it open at the requested scene.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('title', 'field', 'storybook')][string]$Scene,
    [Parameter(Mandatory)][string]$Install,
    [ValidatePattern('^[a-z0-9-]{1,64}$')][string]$Profile,
    [ValidatePattern('^[1-9][0-9]*x[1-9][0-9]*$')][string]$Render,
    [ValidateRange(2,16)][int]$GuestCpuCount,
    [ValidateSet('low','highest')][string]$GraphicsPreset,
    [switch]$Restart,
    [ValidateRange(1,1000)][int]$Fps = 165,
    [ValidateRange(0,24)][int]$SampleWindows = 3,
    [string]$PreviousInstall,
    [string]$SaveArchive,
    [switch]$ThreadCpu,
    [switch]$AutoWalk
)
$ErrorActionPreference = 'Stop'
if ($AutoWalk -and $Scene -ne 'field') { throw 'Auto walking requires the 3D field checkpoint.' }
$root = (Resolve-Path -LiteralPath $Install).Path
$launcher = Join-Path $root 'reincomp.exe'
$log = Join-Path $root 'data/game.log'
$capture = Join-Path $root ("data/scene-{0}-{1}.png" -f $Scene, [guid]::NewGuid())
if (!(Test-Path -LiteralPath $launcher -PathType Leaf)) { throw "Missing $launcher" }
if ($SaveArchive) {
    $archive = (Get-Item -LiteralPath $SaveArchive -ErrorAction Stop).FullName
    if (!(Test-Path -LiteralPath $archive -PathType Leaf)) { throw "Expected a save archive at $archive" }
    $server = Join-Path $root 'server\bunker-server.exe'
    if (!(Test-Path -LiteralPath $server -PathType Leaf)) { throw "Missing $server" }
}
if ($GraphicsPreset -and $Scene -eq 'title') { throw 'Select the field or storybook scene to benchmark a graphics preset.' }
$installations = @($root)
if ($PreviousInstall) {
    if (!$Restart) { throw 'Switching installations requires -Restart.' }
    $previous = (Resolve-Path -LiteralPath $PreviousInstall).Path
    if ($previous -ne $root) { $installations += $previous }
}
foreach ($installation in $installations) {
    $gamePath = Join-Path $installation 'game\bunker-game-bootstrap.exe'
    $launcherPath = Join-Path $installation 'reincomp.exe'
    $game = @(Get-Process -Name bunker-game-bootstrap -ErrorAction SilentlyContinue | Where-Object Path -eq $gamePath)
    $runningLaunchers = @(Get-Process -Name reincomp -ErrorAction SilentlyContinue | Where-Object Path -eq $launcherPath)
    if ($game.Count -gt 1 -or $runningLaunchers.Count -gt 1) { throw "Multiple processes are using $installation. Close the extra instances first." }
    if (!$game.Count -and !$runningLaunchers.Count) { continue }
    if (!$Restart) { throw "This installation is open. Pass -Restart to close the game and run the requested scene." }
    if (!$game.Count) { throw "The launcher in $installation is open without a game window. Close it before restarting." }
    if (!$game[0].CloseMainWindow() -or !$game[0].WaitForExit(15000)) { throw "The game did not finish closing in $installation." }
    if ($runningLaunchers.Count -and !$runningLaunchers[0].WaitForExit(15000)) { throw "The launcher is still closing in $installation." }
}

if ($SaveArchive) {
    # The Bunker importer checks the database and adopts this installation's
    # player ID before swapping saves. Give each older rollback its own name.
    $data = (Get-Item -LiteralPath (Join-Path $root 'data') -ErrorAction Stop).FullName
    $save = Join-Path $data 'server'
    $previousSave = Join-Path $data 'server.before-import'
    $lock = [IO.FileStream]::new((Join-Path $data '.scene-restore.lock'),
        [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None,
        4096, [IO.FileOptions]::DeleteOnClose)
    $olderBackup = $null
    try {
        $active = @(Get-Process -Name bunker-server -ErrorAction SilentlyContinue | Where-Object Path -eq $server)
        if ($active.Count) { throw "The benchmark server is still running at $server." }
        if (Test-Path -LiteralPath "$save.importing") { throw "A previous import is unfinished at $save.importing." }
        if (!(Test-Path -LiteralPath (Join-Path $save 'game.db') -PathType Leaf)) { throw "Register a player in $root before restoring a checkpoint." }
        if (Test-Path -LiteralPath $previousSave) {
            $old = Get-Item -LiteralPath $previousSave -Force
            if (!$old.PSIsContainer -or $old.Parent.FullName -ne $data) { throw "Unexpected backup path $previousSave." }
            $name = 'server.before-import-' + [guid]::NewGuid()
            Rename-Item -LiteralPath $old.FullName -NewName $name -ErrorAction Stop
            $olderBackup = Join-Path $data $name
            $prefix = $previousSave + [IO.Path]::DirectorySeparatorChar
            if ($archive.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
                $archive = Join-Path $olderBackup $archive.Substring($prefix.Length)
            }
        }
        & $server saves import -data $save -from $archive
        if ($LASTEXITCODE -ne 0) { throw "Bunker could not restore $archive (exit $LASTEXITCODE)." }
        if ($olderBackup) { Write-Output "Earlier backup kept at $olderBackup" }
        Write-Output "Restored $archive. Previous progress is in $previousSave"
    } catch {
        if ($olderBackup -and !(Test-Path -LiteralPath $previousSave)) {
            Rename-Item -LiteralPath $olderBackup -NewName 'server.before-import' -ErrorAction Stop
        }
        throw
    } finally { $lock.Dispose() }
}

$start = New-Object System.Diagnostics.ProcessStartInfo
$start.FileName = $launcher
$start.WorkingDirectory = $root
$start.Arguments = "-fps $Fps"
if ($Render) { $start.Arguments += " -render $Render" }
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.RedirectStandardInput = $true
$start.EnvironmentVariables['REINCOMP_SCENE_ROUTE'] = $Scene
if ($Profile) { $start.EnvironmentVariables['REINCOMP_BENCH_PROFILE'] = $Profile }
if ($GraphicsPreset) {
    $start.EnvironmentVariables['REINCOMP_BENCH_QUALITY'] = $GraphicsPreset.ToLowerInvariant()
}
if ($PSBoundParameters.ContainsKey('GuestCpuCount')) {
    $start.EnvironmentVariables['BUNKER_GUEST_CPU_COUNT'] = [string]$GuestCpuCount
}
$start.EnvironmentVariables['REINCOMP_SCENE_CAPTURE'] = $capture
$process = [Diagnostics.Process]::Start($start)
# Unattended runs should return the launcher's error instead of waiting for Enter.
$process.StandardInput.Close()
$deadline = [DateTime]::UtcNow.AddMinutes(5)
$filename = [IO.Path]::GetFileName($capture)
while ($true) {
    if ((Test-Path -LiteralPath $capture) -and (Test-Path -LiteralPath $log)) {
        $tail = Get-Content -LiteralPath $log -Tail 400
        $presented = $tail -match ("Frame snapshot: PNG=.*" + [regex]::Escape($filename) + " .*same_original_swap_EGL_TRUE=1")
        if ($presented -match 'nonuniform=0') { throw "Scene capture contains a uniform frame. See $capture" }
        if ($presented -match 'nonuniform=1') {
            $frame = @($presented)[-1]
            if ($frame -notmatch ' (\d+x\d+) nonuniform=1 ') { throw "Missing scene render dimensions in $frame" }
            $actualRender = $Matches[1]
            if ($Render -and $actualRender -ne $Render) { throw "Scene rendered at $actualRender. Requested $Render." }
            break
        }
    }
    if ($process.HasExited) { throw "Launcher exited ($($process.ExitCode)). See $log" }
    if ([DateTime]::UtcNow -gt $deadline) { throw "Route did not produce a confirmed capture. See $log" }
    Start-Sleep -Milliseconds 250
}
$result = Select-String -LiteralPath $log -Pattern "^Scene route: $Scene (arrived|timed out);" | Select-Object -Last 1
if (!$result -or $result.Line -notmatch ' arrived;') { throw "Scene unconfirmed. Inspect $capture and $log" }
Write-Output "Reached $Scene at $actualRender. Scene capture: $capture"
if ($GraphicsPreset) { Write-Output "Game graphics preset confirmed: $GraphicsPreset" }
$measurementOptions = @{}
if ($AutoWalk) {
    $exe = Join-Path $root 'game\bunker-game-bootstrap.exe'
    $running = @(Get-Process -Name bunker-game-bootstrap | Where-Object Path -eq $exe)
    if ($running.Count -ne 1) { throw "Expected one benchmark game at $exe" }
    if (-not ('ReincompFieldWalk' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
public static class ReincompFieldWalk {
    [StructLayout(LayoutKind.Sequential)]
    private struct Rect { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] private static extern bool GetClientRect(IntPtr hwnd, out Rect rect);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] private static extern int GetClassName(IntPtr hwnd, StringBuilder name, int size);
    [DllImport("user32.dll")] private static extern bool PostMessage(IntPtr hwnd, uint msg, IntPtr button, IntPtr position);
    public static void TapAuto(IntPtr hwnd) {
        Rect rect;
        var name = new StringBuilder(96);
        if (GetClassName(hwnd, name, name.Capacity) == 0 || name.ToString() != "BunkerRecompNativeWindow" ||
            !GetClientRect(hwnd, out rect) || rect.Right <= 0 || rect.Bottom <= 0)
            throw new InvalidOperationException("Cannot locate the bridge game window.");
        // The bridge HUD anchors Auto to the top right. Its centre on a 960x540 surface is (902,78).
        int x = rect.Right - 58 * rect.Bottom / 540, y = 78 * rect.Bottom / 540;
        var point = new IntPtr((y << 16) | (x & 0xffff));
        if (!PostMessage(hwnd, 0x0201, new IntPtr(1), point))
            throw new InvalidOperationException("Could not press Auto.");
        Thread.Sleep(150);
        if (!PostMessage(hwnd, 0x0202, IntPtr.Zero, point))
            throw new InvalidOperationException("Could not release Auto.");
    }
}
'@
    }
    $gameWindow = $running[0].MainWindowHandle
    $startWalk = {
        [ReincompFieldWalk]::TapAuto($gameWindow)
        Write-Output 'Tapped Auto on the field HUD.'
    }.GetNewClosure()
    if ($SampleWindows -eq 0) { & $startWalk }
    else {
        # Start each measured route at the same point in the five-second present window.
        $measurementOptions.OnMeasurementStart = $startWalk
    }
}
if ($SampleWindows -eq 0) { return }

$profilePath = [IO.Path]::ChangeExtension($capture, '.txt')
& (Join-Path $PSScriptRoot 'measure_fps.ps1') -Install $root -SampleWindows $SampleWindows -OutputPath $profilePath -ThreadCpu:$ThreadCpu @measurementOptions
