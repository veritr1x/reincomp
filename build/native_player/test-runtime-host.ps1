param([switch]$Graphics,[switch]$Windowed,[switch]$CheckRoots,[switch]$InspectEnums,[switch]$InspectMovie,[switch]$Play,[ValidateRange(15,300)][int]$TimeoutSeconds=45,[ValidatePattern('^[1-9][0-9]*x[1-9][0-9]*$')][string]$Render='3840x2160')
$ErrorActionPreference='Stop'
$nativeRoot=if($env:REINCOMP_NATIVE_PLAYER_ROOT){$env:REINCOMP_NATIVE_PLAYER_ROOT}else{$PSScriptRoot}
$folder=Join-Path $nativeRoot 'runtime-player'
$exe=Join-Path $folder 'WindowsPlayer.exe'
$stamp=Get-Date
$log=Join-Path $folder 'native-boot.log'
$env:BUNKER_PROBE_FILE=Join-Path $nativeRoot 'runtime-throws.txt'
if($CheckRoots){$env:REINCOMP_VERIFY_NATIVE_ROOTS='1'}else{$env:REINCOMP_VERIFY_NATIVE_ROOTS=$null}
if($InspectEnums){$env:REINCOMP_INSPECT_ENUMS='1'}else{$env:REINCOMP_INSPECT_ENUMS=$null}
if($InspectMovie){$env:REINCOMP_INSPECT_MOVIE='1'}else{$env:REINCOMP_INSPECT_MOVIE=$null}
$argsList=@('-batchmode','-nographics')
if($Graphics){$argsList=@('-batchmode','-force-glcore45','-screen-width','3840','-screen-height','2160','-screen-fullscreen','0')}
$style='Hidden'
if($Windowed){
    $width,$height=$Render.Split('x')
    $argsList=@('-force-glcore45','-screen-width',$width,'-screen-height',$height,'-screen-fullscreen','0')
    if($Play -or $env:REINCOMP_OPEN_SCREEN){
        $env:REINCOMP_INTERACTIVE_WINDOW='1'
        $env:REINCOMP_WINDOW_WIDTH=$width
        $env:REINCOMP_WINDOW_HEIGHT=$height
    }
    if($env:REINCOMP_NATIVE_FULLSCREEN){
        $argsList=@('-force-glcore45','-screen-width',$width,'-screen-height',$height,'-screen-fullscreen','1','-window-mode','borderless')
    }
    $style='Normal'
}
if($env:REINCOMP_NATIVE_JOB_WORKERS){ $argsList+=@('-job-worker-count',$env:REINCOMP_NATIVE_JOB_WORKERS) }
$capture=$null
$run=$null
$outputCsv=$env:REINCOMP_NATIVE_PRESENT
$presentMon=$env:REINCOMP_PRESENTMON
if(!$presentMon){
    $found=Get-Command PresentMon.exe -ErrorAction SilentlyContinue
    $presentMon=if($found){$found.Source}else{'C:\Program Files (x86)\RivaTuner Statistics Server\Plugins\Client\PresentMonDataProvider\PresentMon-2.3.1-x64.exe'}
}
$launchArgs=$argsList+@('-logFile',('"{0}"' -f $log))
if($outputCsv -and $env:REINCOMP_OPEN_SCREEN -eq 'Menu'){
    if(!(Test-Path -LiteralPath $presentMon)){throw 'The timed Menu benchmark needs PresentMon. Set REINCOMP_PRESENTMON to its executable.'}
    # The tab opens well after startup. Select the actual PID immediately so
    # ETW can include its first navigation even without process-name events.
    $run=Start-Process -FilePath $exe -WorkingDirectory $folder -WindowStyle $style -ArgumentList $launchArgs -PassThru
}
if($outputCsv -and !$env:REINCOMP_NATIVE_RENDER_ONLY -and (Test-Path -LiteralPath $presentMon)){
    # Observe the OpenGL context from creation. Match the actual PID in the
    # report so an unrelated WindowsPlayer.exe cannot contribute any frames.
    $captureSession="Reincomp"+$PID
    $target=if($run){@('--process_id',[string]$run.Id)}else{@('--process_name','WindowsPlayer.exe')}
    $captureArgs=$target+@('--qpc_time_ms','--session_name',$captureSession,
        '--timed',($TimeoutSeconds+2),'--terminate_after_timed','--no_track_input','--no_console_stats',
        '--set_circular_buffer_size','8192','--output_file',('"{0}"' -f $outputCsv))
    $capture=Start-Process -FilePath $presentMon -ArgumentList $captureArgs -WindowStyle Hidden -PassThru -RedirectStandardOutput ($outputCsv+'.stdout') -RedirectStandardError ($outputCsv+'.stderr')
    Start-Sleep -Milliseconds 750
}
if(!$run){$run=Start-Process -FilePath $exe -WorkingDirectory $folder -WindowStyle $style -ArgumentList $launchArgs -PassThru}
$timer=[Diagnostics.Stopwatch]::StartNew()
if($capture){[IO.File]::WriteAllText([IO.Path]::ChangeExtension($outputCsv,'.pid'),[string]$run.Id)}
if($Play){$run.WaitForExit(); $result=$run.ExitCode}
elseif(!$run.WaitForExit([Math]::Max(0,[int](1000*$TimeoutSeconds-$timer.Elapsed.TotalMilliseconds)))){
    $diagnostic=Get-Process -Id $run.Id -ErrorAction SilentlyContinue
    if($diagnostic -and $diagnostic.Path -eq $exe){$diagnostic.Kill();$diagnostic.WaitForExit()}
    Write-Output "Diagnostic reached the $TimeoutSeconds second timeout."
    $result=124
}else{$result=$run.ExitCode;Write-Output "Diagnostic exit $result"}
if($capture){
    if((!$capture.HasExited -and $result -ne 124) -or !$capture.WaitForExit(10000)){
        & logman stop $captureSession -ets | Out-Null
        if(!$capture.HasExited){$capture.Kill();$capture.WaitForExit()}
    }
    if(Test-Path -LiteralPath ($outputCsv+'.stderr')){Get-Content -LiteralPath ($outputCsv+'.stderr') -Tail 5}
    if(!(Test-Path -LiteralPath $outputCsv)){Write-Output 'PresentMon produced no CSV. Checking the native renderer capture.'}
}
foreach($name in 'native-root-test.txt','boundary.txt','frame-state.log','runtime.log','native-boot.log'){
    $file=Get-Item (Join-Path $folder $name) -ErrorAction SilentlyContinue
    if($file -and $file.LastWriteTime -ge $stamp.AddSeconds(-1)){Write-Output $name;Get-Content -LiteralPath $file.FullName -Tail 8 | Where-Object {$_.Length -lt 600}}
}
exit $result
