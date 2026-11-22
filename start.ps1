#requires -Version 5.1
[CmdletBinding()]
param(
    [string]$SystemImage = '',
    [string]$ExternalSd = '',
    [string]$Qemu = '',
    [string]$MsysRoot = 'C:\msys64',
    [string]$StageRoot = '',
    [ValidateSet('gtk', 'sdl', 'none')][string]$Display = 'gtk',
    [ValidateSet('dsound', 'wav', 'none')][string]$Audio = 'dsound',
    [string]$RtcState = '',
    [switch]$CopyImageToStage,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$project = $PSScriptRoot
function Assert-Ascii([string]$Value, [string]$Label) {
    if ($Value -match '[^\x20-\x7E]') { throw "$Label must contain ASCII characters only: $Value" }
}
function Resolve-Image([string]$Value) {
    if (![IO.Path]::IsPathRooted($Value)) { $Value = Join-Path $project $Value }
    $full = [IO.Path]::GetFullPath($Value)
    if (!(Test-Path -LiteralPath $full -PathType Leaf)) { throw "Image not found: $full" }
    return $full
}
if (!$Qemu) {
    # build.ps1 writes the executable to <repo>\build\qemu-system-mipsel.exe.
    # Keep the packaged locations as fallbacks for users who ran package.py.
    foreach ($candidate in @((Join-Path $project 'build\qemu-system-mipsel.exe'), (Join-Path $project 'bin\qemu-system-mipsel.exe'), (Join-Path $project 'dist\bin\qemu-system-mipsel.exe'))) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { $Qemu = $candidate; break }
    }
    if (!$Qemu) { throw 'No QEMU executable found; supply -Qemu <path> or build/package the emulator first.' }
}
$Qemu = [IO.Path]::GetFullPath($Qemu)
if (!(Test-Path -LiteralPath $Qemu -PathType Leaf)) { throw "QEMU not found: $Qemu" }
if (!$SystemImage) {
    foreach ($candidate in @((Join-Path $project 'images\book2.img'), (Join-Path $project 'book2.img'), 'Z:\book2.img')) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { $SystemImage = $candidate; break }
    }
    if (!$SystemImage) { throw 'No system image found. Supply -SystemImage <path>, or place book2.img in images\.' }
}
$SystemImage = Resolve-Image $SystemImage
if ($ExternalSd) {
    $ExternalSd = Resolve-Image $ExternalSd
    if ($SystemImage -ieq $ExternalSd) { throw 'SystemImage and ExternalSd must be different files; MSC1 is an optional external card.' }
}
if (!$StageRoot) {
    foreach ($candidate in @((Join-Path $env:LOCALAPPDATA 'book2-runtime'), (Join-Path $env:PUBLIC 'book2-runtime'))) {
        if ($candidate -notmatch '[^\x20-\x7E]') { $StageRoot = $candidate; break }
    }
    if (!$StageRoot) { throw 'Supply an ASCII -StageRoot directory.' }
}
$StageRoot = [IO.Path]::GetFullPath($StageRoot)
Assert-Ascii $StageRoot 'StageRoot'
$runDirectory = Join-Path $StageRoot ('run-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + $PID)
$workingDirectory = [IO.Path]::GetDirectoryName($SystemImage)
$systemArgument = [IO.Path]::GetFileName($SystemImage)
$externalArgument = $ExternalSd
if ($ExternalSd -and (([IO.Path]::GetDirectoryName($ExternalSd)) -ieq $workingDirectory)) {
    $externalArgument = [IO.Path]::GetFileName($ExternalSd)
}
if ($CopyImageToStage) {
    $workingDirectory = $runDirectory
    $systemArgument = 'book2.img'
    if ($ExternalSd) { $externalArgument = 'external-sd.img' }
} else {
    Assert-Ascii $systemArgument 'System image filename (use -CopyImageToStage for a non-ASCII filename)'
    if ($externalArgument) { Assert-Ascii $externalArgument 'External SD path (use -CopyImageToStage if needed)' }
}
# Commas delimit QEMU machine properties, including image filenames.
foreach ($argument in @($systemArgument, $externalArgument)) {
    if ($argument -and $argument.Contains(',')) { throw 'Image paths containing commas require -CopyImageToStage.' }
}
$serialLog = Join-Path $runDirectory 'serial.log'
# Stage packaged runtime components as well when dist lives below a Unicode path.
$originalBinaryDirectory = [IO.Path]::GetDirectoryName($Qemu)
$originalRuntimeRoot = [IO.Path]::GetDirectoryName($originalBinaryDirectory)
$stageRuntime = ($originalRuntimeRoot -match '[^\x20-\x7E]') -and
    (Test-Path -LiteralPath (Join-Path $originalRuntimeRoot 'share') -PathType Container)
if ($stageRuntime) { $Qemu = Join-Path $runDirectory 'runtime\bin\qemu-system-mipsel.exe' }
$machine = 'book2,msc0-image=' + $systemArgument
if ($externalArgument) { $machine += ',msc1-image=' + $externalArgument }
# The Book2 keyboard is a host-side input device, not a USB HID one.  QEMU's GTK
# front end only forwards keystrokes while the console holds an input grab, so
# without grab-on-hover typing appears to do nothing until the user grabs the
# pointer.  Grab the keyboard automatically when the pointer is over the window.
$displayOption = $Display
if ($Display -eq 'gtk') { $displayOption = 'gtk,grab-on-hover=on' }
$qemuArguments = @('-M', $machine, '-icount', 'shift=auto', '-display', $displayOption,
    '-serial', ('file:' + $serialLog), '-monitor', 'none', '-nic', 'none')
if (!$RtcState) { $RtcState = Join-Path $StageRoot 'book2-rtc.state' }
$RtcState = [IO.Path]::GetFullPath($RtcState)
Assert-Ascii $RtcState 'RTC state path'
if ($RtcState.Contains(',')) { throw 'RTC state path must not contain commas.' }
$qemuArguments += @('-global', ('book2-rtc.state-file=' + $RtcState))
if ($Audio -eq 'dsound') {
    $qemuArguments += @('-audiodev', 'dsound,id=audio0', '-global', 'book2-aic.audiodev=audio0')
} elseif ($Audio -eq 'wav') {
    $qemuArguments += @('-audiodev', ('wav,id=audio0,path=' + (Join-Path $runDirectory 'audio.wav')), '-global', 'book2-aic.audiodev=audio0')
} else {
    $qemuArguments += @('-audiodev', 'none,id=audio0', '-global', 'book2-aic.audiodev=audio0')
}
# Firmware starts and handles first-run setup through its own drivers.
$binaryDirectory = [IO.Path]::GetDirectoryName($Qemu)
$runtimeRoot = [IO.Path]::GetDirectoryName($binaryDirectory)
$firmware = Join-Path $runtimeRoot 'share\qemu'
if ((Test-Path -LiteralPath $firmware -PathType Container) -or ($stageRuntime -and (Test-Path -LiteralPath (Join-Path $originalRuntimeRoot 'share\qemu') -PathType Container))) {
    Assert-Ascii $firmware 'Packaged QEMU firmware path (put dist under an ASCII directory)'
    $qemuArguments += @('-L', $firmware)
}
Write-Host "QEMU: $Qemu"
Write-Host "System card: $SystemImage"
if ($ExternalSd) { Write-Host "External SD: $ExternalSd" }
Write-Host "Working directory: $workingDirectory"
Write-Host ('Arguments: ' + ($qemuArguments -join ' '))
Write-Host "Serial log: $serialLog"
Write-Host "RTC state: $RtcState; audio: $Audio"
if ($CopyImageToStage) { Write-Host 'Explicit copy mode: guest writes affect the staged copies only; originals are not copied back.' }
if ($DryRun) { Write-Host 'DRY RUN: no image opened or copied, no directory created, no QEMU started.'; return }

# Validate only a tiny boot header, never hash/read the whole multi-GiB image.
$minimumSystemSize = [long]0x309000 * 512
$imageInfo = Get-Item -LiteralPath $SystemImage
if ($imageInfo.Length -lt $minimumSystemSize) { throw "System image is too small: $($imageInfo.Length) bytes; need at least $minimumSystemSize." }
$bootHeader = New-Object byte[] 64
$stream = [IO.File]::Open($SystemImage, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
try { $headerRead = $stream.Read($bootHeader, 0, $bootHeader.Length) } finally { $stream.Dispose() }
if ($headerRead -ne 64 -or @($bootHeader | Where-Object { $_ -ne 0 }).Count -eq 0) {
    throw 'System image has an empty/truncated Boot0 header (first 64 bytes); wait for a valid rebuilt image.'
}
New-Item -ItemType Directory -Path $runDirectory -Force | Out-Null
New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($RtcState)) -Force | Out-Null
if ($stageRuntime) {
    $stagedRuntimeRoot = Join-Path $runDirectory 'runtime'
    New-Item -ItemType Directory -Path $stagedRuntimeRoot -Force | Out-Null
    foreach ($component in @('bin', 'lib', 'share', 'etc')) {
        $sourceComponent = Join-Path $originalRuntimeRoot $component
        if (Test-Path -LiteralPath $sourceComponent -PathType Container) {
            Copy-Item -LiteralPath $sourceComponent -Destination $stagedRuntimeRoot -Recurse
        }
    }
}
if ($CopyImageToStage) {
    Copy-Item -LiteralPath $SystemImage -Destination (Join-Path $runDirectory 'book2.img')
    if ($ExternalSd) { Copy-Item -LiteralPath $ExternalSd -Destination (Join-Path $runDirectory 'external-sd.img') }
}
$environmentNames = @('PATH', 'XDG_DATA_DIRS', 'GSETTINGS_SCHEMA_DIR', 'GTK_DATA_PREFIX', 'GTK_EXE_PREFIX', 'GDK_PIXBUF_MODULEDIR', 'GDK_PIXBUF_MODULE_FILE')
$savedEnvironment = @{}
foreach ($name in $environmentNames) { $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
$exitCode = 1
$rtcLock = $null
try {
    $rtcLock = [IO.File]::Open(($RtcState + '.lock'), [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    $env:PATH = $binaryDirectory + ';' + (Join-Path $MsysRoot 'ucrt64\bin') + ';' + (Join-Path $MsysRoot 'usr\bin') + ';' + $env:PATH
    $share = Join-Path $runtimeRoot 'share'
    if (Test-Path -LiteralPath (Join-Path $share 'glib-2.0\schemas') -PathType Container) {
        Assert-Ascii $runtimeRoot 'Packaged runtime path (put dist under an ASCII directory)'
        $env:XDG_DATA_DIRS = $share
        $env:GSETTINGS_SCHEMA_DIR = Join-Path $share 'glib-2.0\schemas'
        $env:GTK_DATA_PREFIX = $runtimeRoot
        $env:GTK_EXE_PREFIX = $runtimeRoot
        $loaderVersion = Get-ChildItem -LiteralPath (Join-Path $runtimeRoot 'lib\gdk-pixbuf-2.0') -Directory -ErrorAction SilentlyContinue | Select-Object -First 1
        $queryLoaders = Join-Path $binaryDirectory 'gdk-pixbuf-query-loaders.exe'
        if ($loaderVersion -and (Test-Path -LiteralPath $queryLoaders -PathType Leaf)) {
            $env:GDK_PIXBUF_MODULEDIR = Join-Path $loaderVersion.FullName 'loaders'
            $env:GDK_PIXBUF_MODULE_FILE = Join-Path $runDirectory 'loaders.cache'
            $cache = & $queryLoaders
            if ($LASTEXITCODE -ne 0) { throw 'gdk-pixbuf-query-loaders failed.' }
            [IO.File]::WriteAllLines($env:GDK_PIXBUF_MODULE_FILE, [string[]]$cache, (New-Object Text.UTF8Encoding($false)))
        }
    }
    Push-Location -LiteralPath $workingDirectory
    try {
        & $Qemu @qemuArguments
        $exitCode = $LASTEXITCODE
    } finally { Pop-Location }
} finally {
    if ($rtcLock) { $rtcLock.Dispose() }
    foreach ($name in $environmentNames) { [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process') }
}
Write-Host "QEMU exited with code $exitCode; serial log: $serialLog"
exit $exitCode
