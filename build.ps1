#requires -Version 5.1
[CmdletBinding()]
param(
    [string]$BuildDirectory = '',
    [string]$MsysRoot = 'C:\msys64',
    [ValidateRange(1, 128)][int]$Jobs = [Math]::Max(1, [Environment]::ProcessorCount),
    [switch]$SkipConfigure
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$project = $PSScriptRoot
if (!$BuildDirectory) { $BuildDirectory = Join-Path $project 'build' }
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)

$bash = Join-Path $MsysRoot 'usr\bin\bash.exe'
$ninja = Join-Path $MsysRoot 'ucrt64\bin\ninja.exe'
if (!(Test-Path -LiteralPath $bash -PathType Leaf)) { throw "MSYS2 bash not found: $bash" }
if (!(Test-Path -LiteralPath $ninja -PathType Leaf)) { throw "ninja not found: $ninja" }

# MSYS2 path for the repository and build directory (drive C: -> /c).
$repoPosix = '/' + ($project -replace '\\','/') -replace '^([A-Za-z]):','$1'
$repoPosix = $repoPosix -replace '^/([A-Za-z])/', { '/$1/'.ToLower() }
$buildPosix = '/' + ($BuildDirectory -replace '\\','/') -replace '^([A-Za-z]):','$1'
$buildPosix = $buildPosix -replace '^/([A-Za-z])/', { '/$1/'.ToLower() }
$tmpPosix = Join-Path $project '.tmp'
New-Item -ItemType Directory -Path $tmpPosix -Force | Out-Null
$tmpP = $tmpPosix -replace '\\','/'

Write-Host "Repository : $project"
Write-Host "Build dir  : $BuildDirectory"
Write-Host "MSYS2 root : $MsysRoot"

function Invoke-Msys([string]$script) {
    $old = $env:PATH
    try {
        $env:PATH = (Join-Path $MsysRoot 'ucrt64\bin') + ';' + (Join-Path $MsysRoot 'usr\bin') + ';' + $old
        & $bash -lc $script
        if ($LASTEXITCODE -ne 0) { throw "MSYS2 command failed (exit $LASTEXITCODE): $script" }
    } finally { $env:PATH = $old }
}

if ($SkipConfigure) {
    Write-Host 'Skipping configure (using existing build tree).'
} else {
    Write-Host 'Configuring mipsel-softmmu in MSYS2 UCRT64 environment...'
    # Configure from the repository itself. The Book2 sources are already in
    # hw/mips and registered in Kconfig/meson.build by this repository.
    Invoke-Msys "cd '$repoPosix' && export PATH='/ucrt64/bin:/usr/bin:`$PATH' && export PYTHON=/ucrt64/bin/python.exe && export TMP='$tmpP' && export TEMP='$tmpP' && export TMPDIR='$tmpP' && ./configure --target-list=mipsel-softmmu --python=/ucrt64/bin/python.exe --disable-werror"
}

Write-Host "Building with $Jobs jobs..."
Invoke-Msys "cd '$repoPosix' && export PATH='/ucrt64/bin:/usr/bin:`$PATH' && export TMP='$tmpP' && export TEMP='$tmpP' && export TMPDIR='$tmpP' && '$ninja' -C '$buildPosix' -j$Jobs qemu-system-mipsel.exe"

$exe = Join-Path $BuildDirectory 'qemu-system-mipsel.exe'
if (!(Test-Path -LiteralPath $exe -PathType Leaf)) { throw "Build did not produce $exe" }
Write-Host "Built: $exe"
