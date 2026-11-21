# Launch a published installation with its own Bunker services and save.
[CmdletBinding()]
param([string]$Scene,[string]$Install,[string]$SaveArchive,[string]$Render,[int]$Fps,[string]$FieldScene,
      [switch]$Fullscreen,[string]$OpenScreen,[switch]$Play,[switch]$AutoWalk,[switch]$ThreadCpu,
      [switch]$CompareMenuPointer,[int]$SampleWindows,[string]$PythonPath,[string]$PreferencesSnapshot)
$ErrorActionPreference='Stop'
$location=Get-Item -LiteralPath $Install
$root=if($location.PSIsContainer){$location.FullName}else{$location.DirectoryName}
$portable=if(!$location.PSIsContainer){$location.FullName}else{Join-Path $root 'Nier Reincarnation.exe'}
if(Test-Path -LiteralPath $portable -PathType Leaf){
    $launcher=$portable
    & $launcher -prepare
    if($LASTEXITCODE){throw 'The portable installation could not be prepared.'}
    $root=Join-Path (Split-Path -Parent $launcher) '.bunker-player'
}else{
    $launcher=(Resolve-Path -LiteralPath (Join-Path $root 'reincomp.exe')).Path
}
$game=(Resolve-Path -LiteralPath (Join-Path $root 'game/WindowsPlayer.exe')).Path
$gameDir=Split-Path -Parent $game
function Find-Player { Get-Process WindowsPlayer -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $game } }
if(Find-Player){throw 'Close this installation before replaying its checkpoint.'}
if(!$Play){
    . (Join-Path $PSScriptRoot 'presentmon.ps1')
    $presentMon=Find-PresentMon
    if(!$PythonPath){$PythonPath=(Resolve-Path -LiteralPath (Join-Path $root 'pods/python/python.exe')).Path}
}
if($SaveArchive){
    $backup=(Resolve-Path -LiteralPath $SaveArchive).Path
    & $launcher -import-save $backup
    if($LASTEXITCODE -ne 0){throw 'The checkpoint import failed.'}
}
$start=[Diagnostics.ProcessStartInfo]::new()
$start.FileName=$launcher
$start.WorkingDirectory=Split-Path -Parent $launcher
$start.UseShellExecute=$false
$start.CreateNoWindow=$true
$start.RedirectStandardInput=$true
$start.Arguments="-fps $Fps -render $Render"
if($Fullscreen){$start.Arguments+=' -fullscreen'}
if($Scene -ne 'title'){$start.EnvironmentVariables['REINCOMP_ROUTE_CHECKPOINT']='1'}
if($OpenScreen){$start.EnvironmentVariables['REINCOMP_OPEN_SCREEN']=$OpenScreen}
if($PreferencesSnapshot){$start.EnvironmentVariables['REINCOMP_CHECKPOINT_PREFS']=$PreferencesSnapshot}
if($AutoWalk){$start.EnvironmentVariables['REINCOMP_NATIVE_AUTO_WALK']='1'}
if($ThreadCpu){$start.EnvironmentVariables['REINCOMP_PROFILE_MANAGED']='1'}
if(!$Play){$start.EnvironmentVariables['REINCOMP_BENCHMARK_HIGHEST']='1'}
if($CompareMenuPointer){$start.EnvironmentVariables['REINCOMP_COMPARE_MENU_POINTER']='1'}
$app=[Diagnostics.Process]::Start($start)
$app.StandardInput.Close()
if($Play){$app.WaitForExit();if($app.ExitCode){throw "The launcher exited $($app.ExitCode)."};return}
$capture=$null
$player=$null
try {
    $clock=[Diagnostics.Stopwatch]::StartNew()
    while(!$player -and !$app.HasExited -and $clock.Elapsed.TotalSeconds -lt 90){
        $player=Find-Player | Select-Object -First 1
        if(!$player){Start-Sleep -Milliseconds 100}
    }
    if(!$player){throw 'The launcher did not start WindowsPlayer. See data/server.log and data/game.log.'}
    $output=Join-Path $root 'data/benchmarks'
    New-Item -ItemType Directory -Path $output -Force | Out-Null
    $base=Join-Path $output ("$Scene-"+(Get-Date -Format 'yyyyMMdd-HHmmss')+'-'+$player.Id)
    $session='ReincompInstall'+$player.Id
    $startupSeconds=if($Scene -eq 'menu'){210}elseif($Scene -eq 'storybook'){165}else{110}
    $seconds=$startupSeconds+5*$SampleWindows
    $capture=Start-PlayerCapture $presentMon $player.Id $base $session $seconds
    Write-Output "Measuring $Scene in $base.csv"
    while(!$capture.WaitForExit(500) -and !$player.HasExited -and $clock.Elapsed.TotalSeconds -lt $seconds+100){}
} finally {
    if($capture -and !$capture.HasExited){
        & logman stop $session -ets | Out-Null
        if(!$capture.WaitForExit(8000)){$capture.Kill();$capture.WaitForExit()}
    }
    if($player -and !$player.HasExited){
        $null=$player.CloseMainWindow()
        if(!$player.WaitForExit(45000)){
            Write-Warning 'The player did not finish shutting down within 45 seconds.'
            $player.Kill();$player.WaitForExit()
        }
    }
}
Copy-Item -LiteralPath (Join-Path $gameDir 'frame-state.log') -Destination ($base+'.frames.log')
Copy-Item -LiteralPath (Join-Path $gameDir 'route-trace.log') -Destination ($base+'.route.log')
if($ThreadCpu){
    foreach($trace in @{'managed-profile.tsv'='.managed.tsv';'slow-callbacks.tsv'='.callbacks.tsv'}.GetEnumerator()){
        $file=Get-Item -LiteralPath (Join-Path $gameDir $trace.Key) -ErrorAction SilentlyContinue
        if($file -and $file.LastWriteTime -ge $app.StartTime){Copy-Item -LiteralPath $file.FullName -Destination ($base+$trace.Value)}
    }
}
if(!$app.WaitForExit(30000)){throw "The player closed but the launcher is still stopping its local services. Capture saved in $base.csv."}
if(!(Test-Path -LiteralPath ($base+'.csv') -PathType Leaf)){
    throw "PresentMon recorded no presentations. Check that the Windows session is unlocked and the player is visible. The game log is in $base.frames.log."
}
if($Scene -eq 'menu'){& $PythonPath (Join-Path $PSScriptRoot 'summarize_menu.py') ($base+'.csv') ($base+'.frames.log')}
else{
    $report=@($base+'.csv')
    if($AutoWalk){$report+='--require-motion'}
    if($FieldScene){$report+='--field-scene',$FieldScene}
    & $PythonPath (Join-Path $PSScriptRoot 'summarize_present.py') @report
}
if($LASTEXITCODE -ne 0){throw "The $Scene capture did not contain the requested focused scene. See $base.frames.log."}
if($app.ExitCode){throw "The launcher exited $($app.ExitCode). The timing report is in $base.csv."}
