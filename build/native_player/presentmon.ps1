function Find-PresentMon {
    if ($env:REINCOMP_PRESENTMON) { $path = $env:REINCOMP_PRESENTMON }
    else {
        $found = Get-Command PresentMon.exe -ErrorAction SilentlyContinue
        $path = if ($found) { $found.Source } else {
            'C:\Program Files (x86)\RivaTuner Statistics Server\Plugins\Client\PresentMonDataProvider\PresentMon-2.3.1-x64.exe'
        }
    }
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw 'Set REINCOMP_PRESENTMON to the PresentMon executable.' }
    return $path
}

function Start-PlayerCapture([string]$Monitor, [int]$PlayerId, [string]$Base, [string]$Session, [int]$Seconds) {
    $arguments = @('--process_id',$PlayerId,'--qpc_time_ms','--session_name',$Session,'--set_circular_buffer_size',8192,
        '--timed',$Seconds,'--terminate_after_timed','--no_track_input','--no_console_stats','--output_file',('"{0}.csv"' -f $Base))
    [IO.File]::WriteAllText($Base+'.pid',[string]$PlayerId)
    Start-Process -FilePath $Monitor -ArgumentList $arguments -PassThru -WindowStyle Hidden -RedirectStandardError ($Base+'.stderr')
}
