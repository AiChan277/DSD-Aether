param(
    [string]$ReleaseBuildDir = "build/windows-release",
    [string]$VcamX86BuildDir = "build/vcam-x86/Release",
    [string]$OutMsi = "dist/AetherStudio-0.1.0-x64.msi"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$wixExe = Get-Command wix -ErrorAction SilentlyContinue
if (-not $wixExe) {
    Write-Warning "WiX v4 CLI ('wix') not found on PATH. Install via 'dotnet tool install --global wix' before packaging the signed MSI."
    exit 2
}

$stageDir = Join-Path $repoRoot ($ReleaseBuildDir + "\src\app")
$vcamX64 = Join-Path $repoRoot ($ReleaseBuildDir + "\bin\aether_vcam_ds_x64.dll")
$vcamX86 = Join-Path $repoRoot ($VcamX86BuildDir + "\aether_vcam_ds_x86.dll")

foreach ($required in @((Join-Path $stageDir "aether_studio.exe"), $vcamX64, $vcamX86)) {
    if (-not (Test-Path $required)) {
        throw "Required installer payload missing: $required"
    }
}

$absOut = if ([System.IO.Path]::IsPathRooted($OutMsi)) { $OutMsi } else { Join-Path $repoRoot $OutMsi }
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $absOut) | Out-Null

$wxs = Join-Path $PSScriptRoot "AetherStudio.wxs"
& $wixExe.Source build $wxs -arch x64 `
    -d "StageDir=$stageDir" `
    -d "VcamX64Dll=$vcamX64" `
    -d "VcamX86Dll=$vcamX86" `
    -o $absOut

if ($LASTEXITCODE -ne 0) {
    throw "wix build failed with exit code $LASTEXITCODE"
}
Write-Host "[installer] Built MSI (unsigned — sign with EV/OV Authenticode before distribution): $absOut"
