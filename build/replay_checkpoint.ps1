# Start one of the prepared local saves, then measure the scene it reaches.
[CmdletBinding()]
param(
    [Parameter(Mandatory, Position=0)][ValidateSet('3d','2d','field','storybook','title','menu')][string]$Scene,
    [string]$CheckpointRoot,
    [string]$Install,
    [string]$SaveArchive,
    [ValidatePattern('^[A-Za-z0-9_]+$')][string]$FieldScene,
    [string]$PlayerPrefs,
    [ValidateRange(0,24)][int]$SampleWindows = 6,
    [ValidateRange(2,16)][int]$GuestCpuCount,
    [ValidateRange(1,1000)][int]$Fps,
    [ValidatePattern('^[1-9][0-9]*x[1-9][0-9]*$')][string]$Render = '3840x2160',
    [ValidateSet('low','highest')][string]$GraphicsPreset = 'highest',
    [switch]$RestoreCheckpoint,
    [switch]$AutoWalk,
    [switch]$ThreadCpu,
    [switch]$NativePlayer,
    [switch]$CompareMenuPointer,
    [switch]$RenderTimingOnly,
    [switch]$Fullscreen,
    [string]$MenuSave,
    [ValidateSet('Summon','SummonRates','SummonNotice','Organization','Enhance','Quest','Subquests','Map','Library','Menu','Mama','All')][string]$OpenScreen,
    [switch]$Play,
    [string]$PythonPath
)
$ErrorActionPreference = 'Stop'
if (!$CheckpointRoot) { $CheckpointRoot = Join-Path (Split-Path -Parent $PSScriptRoot) '.fps-bench' }
if (!$PSBoundParameters.ContainsKey('NativePlayer') -and ($Install -or $Scene -ne 'title')) { $NativePlayer = $true }
function Invoke-WithGamePreferences([scriptblock]$launch) {
    if (!$PlayerPrefs) { & $launch $null; return }
    $source=(Resolve-Path -LiteralPath $PlayerPrefs).Path
    $snapshot=[IO.Path]::GetTempFileName()
    try {
        & $PythonPath (Join-Path $PSScriptRoot 'native_player/checkpoint_prefs.py') $source $snapshot
        if ($LASTEXITCODE) { throw 'Could not read the Android Unity PlayerPrefs XML.' }
        & $launch $snapshot
    } finally {
        if (Test-Path -LiteralPath $snapshot) { Remove-Item -LiteralPath $snapshot }
    }
}
if ($Scene -eq 'menu' -and !$Play) {
    $NativePlayer = $true
    if (!$OpenScreen) { $OpenScreen = 'Menu' }
    if ($OpenScreen -ne 'Menu') { throw 'Timed menu measurements use the Settings and Profile tab. Use -Play for other tabs.' }
}
if ($CompareMenuPointer -and ($Scene -ne 'menu' -or $Play)) { throw 'Use -CompareMenuPointer with a timed menu run.' }
if ($MenuSave -and $Scene -ne 'menu') { throw 'Use -MenuSave with the menu checkpoint.' }
if ($Fullscreen -and !$NativePlayer) { throw 'Use -Fullscreen with the native player.' }
if ($PlayerPrefs -and !$NativePlayer -and !$Install) { throw 'Use -PlayerPrefs with a native Windows player.' }
if ($OpenScreen -and ((!$Play -and $Scene -ne 'menu') -or $AutoWalk)) { throw 'Use the menu checkpoint or -Play to open a menu.' }
if ($OpenScreen -in 'SummonRates','SummonNotice' -and !$Install) { throw 'The embedded Summons pages use the Bunker services in -Install.' }
if ($RenderTimingOnly -and (!$NativePlayer -or $Play)) { throw 'Use -RenderTimingOnly for a timed native player run.' }
if ($Scene -eq '3d') { $Scene = 'field' }
if ($Scene -eq '2d') { $Scene = 'storybook' }
if ($FieldScene -and (!$Install -or $Scene -ne 'field' -or $Play)) { throw 'Use -FieldScene to measure a specific 3D scene in an installed player.' }
if ($Install) {
    if (!$NativePlayer -and $Scene -ne 'title') { throw '-Install uses the native Windows package.' }
    if ($RestoreCheckpoint -and !$MenuSave -and !$SaveArchive) {
        $SaveArchive=Join-Path (Join-Path $CheckpointRoot 'emulator-restore') ($Scene+'-checkpoint.zip')
        if ($Scene -eq 'menu' -and !(Test-Path -LiteralPath $SaveArchive)) {
            $SaveArchive=[IO.Path]::ChangeExtension($SaveArchive,'.db')
        }
    }
    if ($MenuSave) { $SaveArchive=$MenuSave }
    $routeScreen=switch ($OpenScreen) {
        'Summon' {'Gacha'}
        'SummonRates' {'SummonRates'}
        'SummonNotice' {'SummonNotice'}
        default {if ($OpenScreen) {$OpenScreen} elseif ($Scene -eq 'menu') {'Menu'}}
    }
    if ($AutoWalk -and $Scene -ne 'field') { throw 'Auto walking requires the 3D field.' }
    if (!$Fps) { $Fps=144 }
    if (!$Play -and ($Render -ne '3840x2160' -or $GraphicsPreset -ne 'highest' -or $Scene -eq 'title')) {
        throw 'Timed installed-player runs measure the field, storybook or menu at 3840x2160 with Best selected in game.'
    }
    if ($RenderTimingOnly -or $PSBoundParameters.ContainsKey('GuestCpuCount')) {
        throw 'The installed player uses Windows presentation timing and its normal worker settings.'
    }
    $options=@{Scene=$Scene;Install=$Install;SaveArchive=$SaveArchive;FieldScene=$FieldScene;Render=$Render;Fps=$Fps;
        Fullscreen=$Fullscreen;OpenScreen=$routeScreen;Play=$Play;AutoWalk=$AutoWalk;ThreadCpu=$ThreadCpu;
        CompareMenuPointer=$CompareMenuPointer;SampleWindows=$SampleWindows;PythonPath=$PythonPath}
    if ($PlayerPrefs -and !$PythonPath) { $PythonPath=(Resolve-Path -LiteralPath (Join-Path $Install 'pods/python/python.exe')).Path }
    Invoke-WithGamePreferences {
        param($snapshot)
        & (Join-Path $PSScriptRoot 'native_player/run-installed.ps1') @options -PreferencesSnapshot $snapshot
    }
    return
}
if ($SaveArchive) { throw 'Use -SaveArchive with -Install, or -RestoreCheckpoint for the prepared fixtures.' }
if ($NativePlayer) {
    if ($Scene -eq 'title') { throw 'Choose 3d or 2d for the native checkpoint player.' }
    if ((!$Play -and $Render -ne '3840x2160') -or $GraphicsPreset -ne 'highest' -or
        ($PSBoundParameters.ContainsKey('Fps') -and $Fps -ne 165) -or
        $PSBoundParameters.ContainsKey('GuestCpuCount')) {
        throw 'The native checkpoint profile uses 3840x2160, the highest game preset and a 165 FPS target.'
    }
    $native = Join-Path $CheckpointRoot 'native-unity-player'
    $client = Join-Path $native 'checkpoint-client'
    $packages = Join-Path $client 'asset-python.txt'
    if (!$env:REINCOMP_ASSET_PYTHON -and (Test-Path -LiteralPath $packages)) {
        $env:REINCOMP_ASSET_PYTHON = (Get-Content -LiteralPath $packages -Raw).Trim()
    }
    $scripts = Join-Path $PSScriptRoot 'native_player'
    $prepare = Join-Path $scripts 'prepare-native-checkpoint.py'
    $runner = Join-Path $scripts 'test-native-checkpoint.py'
    if (!(Test-Path -LiteralPath (Join-Path $native 'runtime-player/WindowsPlayer.exe') -PathType Leaf)) {
        throw "The prepared native player is missing at $native"
    }
    $interpreter = Join-Path $client 'python-runtime.txt'
    if (!$PythonPath -and (Test-Path -LiteralPath $interpreter)) {
        $PythonPath = (Get-Content -LiteralPath $interpreter -Raw).Trim()
    }
    if (!$PythonPath) { $PythonPath = (Get-Command python -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source }
    $active = Join-Path $client 'active-server.txt'
    $current = if (Test-Path -LiteralPath $active) { (Get-Content -LiteralPath $active -Raw).Trim() }
    if ($MenuSave -or $RestoreCheckpoint -or !$current -or !$current.StartsWith("$Scene-server-")) {
        $saveArgs = @($Scene, $native)
        if ($MenuSave) { $saveArgs += (Resolve-Path -LiteralPath $MenuSave).Path }
        & $PythonPath $prepare @saveArgs
        if ($LASTEXITCODE -ne 0) { throw 'Could not prepare the selected native checkpoint.' }
    }
    $duration = if ($Scene -eq 'menu') { 210 } elseif ($Scene -eq 'storybook') { 165 } else { 110 }
    $duration = [Math]::Min(300, $duration + 5 * $SampleWindows)
    $mode = if ($Play) { '--play' } else { '--present' }
    $runnerArgs = @($duration, '--route', $mode, '--root', $native, '--size', $Render)
    if ($Fullscreen) { $runnerArgs += '--borderless' }
    if ($OpenScreen) { $runnerArgs += @('--open-screen',$OpenScreen) }
    if ($CompareMenuPointer) { $runnerArgs += '--compare-menu-pointer' }
    if ($AutoWalk) {
        if ($Scene -ne 'field') { throw 'Auto walking requires the 3D field.' }
        $runnerArgs += '--auto-walk'
    }
    if ($RenderTimingOnly) { $runnerArgs += '--renderer-only' }
    if ($ThreadCpu) { $runnerArgs += '--managed-profile' }
    Invoke-WithGamePreferences {
        param($snapshot)
        if ($snapshot) { $runnerArgs += @('--player-prefs',$snapshot) }
        & $PythonPath $runner @runnerArgs
        if ($LASTEXITCODE -ne 0) { throw "Native checkpoint player exited $LASTEXITCODE." }
    }
    return
}
if ($Play -or $PythonPath) { throw 'Use -NativePlayer with -Play or -PythonPath.' }
$bridge = Join-Path $CheckpointRoot 'emu-client'
$book = Join-Path $CheckpointRoot 'storybook-client'
$storybook = $Scene -eq 'storybook'
$options = @{
    Scene = $Scene
    Install = $bridge
    PreviousInstall = $book
    Profile = 'emulator-scene8-prefread'
    GuestCpuCount = 4
    Fps = 165
    Render = $Render
    SampleWindows = $SampleWindows
    ThreadCpu = $ThreadCpu
    AutoWalk = $AutoWalk
    Restart = $true
}
if ($storybook) {
    $options.Install = $book
    $options.PreviousInstall = $bridge
    $options.Profile = 'emulator-storybook-prologue'
    $options.GuestCpuCount = 2
    $options.Fps = 144
}
if ($Scene -eq 'title') { $options.GuestCpuCount = 8 }
else { $options.GraphicsPreset = $GraphicsPreset }
if ($RestoreCheckpoint) {
    if ($Scene -eq 'title') { throw 'Choose field or storybook to restore a saved checkpoint.' }
    $archive = if ($storybook) { 'storybook-checkpoint.zip' } else { 'field-checkpoint.zip' }
    $options.SaveArchive = Join-Path (Join-Path $CheckpointRoot 'emulator-restore') $archive
}
if ($PSBoundParameters.ContainsKey('GuestCpuCount')) { $options.GuestCpuCount = $GuestCpuCount }
if ($PSBoundParameters.ContainsKey('Fps')) { $options.Fps = $Fps }
& (Join-Path $PSScriptRoot 'reach_scene.ps1') @options
