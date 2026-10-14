param(
    [string]$Cxx = 'g++', [string]$Cc = 'gcc', [string]$Ar = 'ar',
    [string]$Nm = 'nm', [string]$Python = 'python',
    [string]$Build = 'src/build-windows'
)
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path $PSScriptRoot -Parent)
# One recipe selects the relay source set and statically links mimalloc.
& $Python -B src/build_native.py --platform windows --cxx $Cxx --cc $Cc --ar $Ar --nm $Nm --build $Build
if ($LASTEXITCODE -ne 0) { throw "Native build or N4 audit failed (exit $LASTEXITCODE)" }
