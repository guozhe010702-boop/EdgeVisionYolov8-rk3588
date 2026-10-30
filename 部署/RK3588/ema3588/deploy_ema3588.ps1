param(
    [Parameter(Mandatory = $true)]
    [string]$Serial,

    [string]$AdbPath,
    [switch]$UpdateSources
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$prepareSource = Join-Path $PSScriptRoot 'prepare_source.py'
$sourceArchive = Join-Path $projectRoot '.cache\ema3588-source.tar.gz'
$remoteRoot = '/userdata/edgevision-yolov8'
$remoteApp = "$remoteRoot/ema-runtime"

if (-not $AdbPath) {
    $candidates = @(
        (Join-Path $projectRoot '..\..\Tools\platform-tools\adb.exe'),
        (Join-Path $projectRoot '..\Tools\platform-tools\adb.exe')
    )
    $AdbPath = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
if (-not $AdbPath -or -not (Test-Path -LiteralPath $AdbPath)) {
    throw 'adb.exe not found. Pass -AdbPath with its full path.'
}
foreach ($path in @($prepareSource)) {
    if (-not (Test-Path -LiteralPath $path)) { throw "Required source path missing: $path" }
}

$deviceRows = & $AdbPath devices
if ($LASTEXITCODE -ne 0) { throw 'adb devices failed.' }
$escapedSerial = [regex]::Escape($Serial)
if (-not ($deviceRows | Where-Object { $_ -match "^$escapedSerial\s+device$" })) {
    throw "Serial '$Serial' is not an authorized ADB device. Check 'adb devices -l' and pass the EVM3588 serial."
}

function Invoke-CheckedAdb {
    param([string[]]$Arguments)
    $result = & $script:AdbPath -s $script:Serial @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "ADB command failed (exit $LASTEXITCODE): adb -s $script:Serial $($Arguments -join ' ')"
    }
    return $result
}

$architecture = (Invoke-CheckedAdb @('shell', 'uname', '-m') | Out-String).Trim()
if ($architecture -ne 'aarch64') { throw "Refusing deployment: device reports '$architecture', expected aarch64." }
# Vendor adbd can publish generic Android product names and omit shell exit
# status. Inspect the real device tree and require an explicit success marker.
$boardModel = (Invoke-CheckedAdb @('shell', 'cat /proc/device-tree/model') | Out-String).Trim([char]0, [char]13, [char]10, [char]32)
if ($boardModel -notmatch 'RK3588') { throw "Refusing deployment: unexpected device tree '$boardModel'." }
function Assert-RemoteCheck {
    param([string]$Command, [string]$Message)
    $output = Invoke-CheckedAdb @('shell', "$Command && echo __EDGEVISION_OK__") | Out-String
    if ($output -notmatch '__EDGEVISION_OK__') { throw $Message }
}
Assert-RemoteCheck 'test -e /dev/video44' 'Refusing deployment: /dev/video44 is absent.'
Assert-RemoteCheck 'test -r /sys/kernel/debug/rknpu/version' 'Refusing deployment: RKNPU driver is unavailable.'
if (-not $UpdateSources) {
    Assert-RemoteCheck "test ! -e $remoteApp" "Existing native runtime found: $remoteApp. Stop EdgeVision first and use -UpdateSources for an intentional rebuild."
}
Assert-RemoteCheck 'command -v python3 >/dev/null && command -v cmake >/dev/null && command -v g++ >/dev/null' 'Native build tools are unavailable.'
# Detect both managed and manually launched processes, including an old inode
# marked deleted by a prior installation. Do not depend on optional fuser.
$probeCode = @'
import json,os
from pathlib import Path
pids=[]
for p in Path('/proc').iterdir():
    if not p.name.isdigit(): continue
    try: executable=os.readlink(str(p/'exe')).split(' (deleted)',1)[0]
    except OSError: continue
    if executable=='/userdata/edgevision-yolov8/ema-runtime/rknn_yolov8_demo_stream': pids.append(int(p.name))
print(json.dumps({'pids':pids}))
'@
# Numeric source bytes avoid nested quoting differences in Windows adb.exe.
$probeBytes = [System.Text.Encoding]::UTF8.GetBytes($probeCode) -join ','
$statusText = Invoke-CheckedAdb @('shell', "python3 -c 'exec(bytes([$probeBytes]))'") | Out-String
try { $runtimeStatus = $statusText | ConvertFrom-Json } catch { throw 'Cannot confirm the current EdgeVision process state.' }
if ($null -eq $runtimeStatus.PSObject.Properties['pids']) { throw 'Process probe returned no PID list.' }
if ($runtimeStatus.pids.Count -gt 0) { throw 'Stop the running EdgeVision program before rebuilding it.' }

& python -X utf8 $prepareSource
if ($LASTEXITCODE -ne 0) { throw 'Source packaging failed.' }
$digest = (Get-FileHash -LiteralPath $sourceArchive -Algorithm SHA256).Hash.ToLowerInvariant()
Invoke-CheckedAdb @('shell', 'mkdir', '-p', $remoteRoot) | Out-Null
Invoke-CheckedAdb @('push', $sourceArchive, "$remoteRoot/ema3588-source.tar.gz") | Out-Null
$buildCommand = "cd $remoteRoot && echo '$digest  ema3588-source.tar.gz' | sha256sum -c - && tar xzf ema3588-source.tar.gz && bash ema3588/build_native.sh && echo __EDGEVISION_BUILD_OK__"
$buildOutput = Invoke-CheckedAdb @('shell', $buildCommand) | Out-String
Write-Host $buildOutput
if ($buildOutput -notmatch '__EDGEVISION_BUILD_OK__') { throw 'Native build failed. Review the output above.' }

Write-Host "Native EdgeVision runtime ready in $remoteApp on ADB device $Serial."
Write-Host "Start on the board: python3 $remoteRoot/ema3588/board_control.py start --take-camera"
