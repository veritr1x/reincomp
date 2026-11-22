# Rebuild the authored DLLs against the local player and lifted game objects.
[CmdletBinding()]
param([string]$PythonPath)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$prepared = Join-Path $repo '.fps-bench/native-unity-player'
$packages = Join-Path $prepared 'checkpoint-client/asset-python.txt'
if (!$env:REINCOMP_ASSET_PYTHON -and (Test-Path -LiteralPath $packages)) {
    $env:REINCOMP_ASSET_PYTHON = (Get-Content -LiteralPath $packages -Raw).Trim()
}
$responseFile = Join-Path $repo '.fps-bench/main-bridge-mutex/control/link.rsp'
if (!(Test-Path -LiteralPath $responseFile) -or !(Test-Path -LiteralPath (Join-Path $prepared 'registered-api.txt'))) {
    throw 'The prepared native player, Unity callback inventory and original lifted objects are required.'
}
if (!$PythonPath) {
    $record = Join-Path $prepared 'checkpoint-client/python-runtime.txt'
    $PythonPath = if (Test-Path -LiteralPath $record) { (Get-Content -LiteralPath $record -Raw).Trim() }
                  else { (Get-Command python -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source }
}
foreach ($source in Get-ChildItem -LiteralPath $PSScriptRoot -File) {
    if ($source.Name -eq 'rebuild.ps1' -or $source.Extension -notin '.h','.cpp','.py','.ps1') { continue }
    $destination = Join-Path $prepared $source.Name
    if (!(Test-Path -LiteralPath $destination) -or
        (Get-FileHash -LiteralPath $source.FullName).Hash -ne (Get-FileHash -LiteralPath $destination).Hash) {
        Copy-Item -LiteralPath $source.FullName -Destination $destination
    }
}
foreach ($script in 'build-engine-calls.py','build-runtime-host.py','prepare-window.py') {
    & $PythonPath (Join-Path $prepared $script)
    if ($LASTEXITCODE -ne 0) { throw "$script exited $LASTEXITCODE." }
}
& $PythonPath (Join-Path $prepared 'brand-player.py') (Join-Path $repo '.fps-bench/emu-client/data/game-sound.apk') (Join-Path $prepared 'runtime-player/WindowsPlayer.exe')
if ($LASTEXITCODE -ne 0) { throw 'Could not install the game icon.' }
& $PythonPath (Join-Path $prepared 'prepare-screen-art.py') $prepared --packaged
if ($LASTEXITCODE -ne 0) { throw 'Could not prepare the packaged UI textures.' }
