param(
    [ValidateSet("Debug", "RelWithDebInfo", "Release")]
    [string]$Config = "Release",
    [string]$BuildDir = "build/vcam-x86",
    [string]$Version = "1.0.4",
    [string]$ReleaseTag = "a1.0.4B"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$absBuildDir = if ([System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $repoRoot $BuildDir }
$outBinDir = Join-Path $absBuildDir $Config

New-Item -ItemType Directory -Force -Path $outBinDir | Out-Null

$vcvarsX86 = "D:\VS2022\BuildTools\VC\Auxiliary\Build\vcvarsamd64_x86.bat"
if (-not (Test-Path $vcvarsX86)) {
    throw "MSVC x86 environment script not found at $vcvarsX86"
}

$optFlag = switch ($Config) {
    "Debug" { "/Od /Zi /MDd /D_DEBUG" }
    "RelWithDebInfo" { "/O2 /Zi /MD /DNDEBUG" }
    default { "/O2 /MD /DNDEBUG" }
}

$srcFile = Join-Path $repoRoot "src\vcam\aether_vcam_ds.cpp"
$defFile = Join-Path $repoRoot "src\vcam\aether_vcam_ds.def"
$rcInFile = Join-Path $repoRoot "src\vcam\aether_vcam_ds.rc.in"
$rcFile = Join-Path $outBinDir "aether_vcam_ds_x86.rc"
$resFile = Join-Path $outBinDir "aether_vcam_ds_x86.res"
$incDir = Join-Path $repoRoot "src"
$outDll = Join-Path $outBinDir "aether_vcam_ds_x86.dll"
$outLib = Join-Path $outBinDir "aether_vcam_ds_x86.lib"
$outPdb = Join-Path $outBinDir "aether_vcam_ds_x86.pdb"
$outObj = Join-Path $outBinDir "aether_vcam_ds_x86.obj"

$verParts = $Version.Split('.')
while ($verParts.Count -lt 4) { $verParts += "0" }
$verComma = ($verParts[0..3] -join ",")
$verStr = ($verParts[0..3] -join ".")
$rcText = (Get-Content -Raw -Encoding UTF8 $rcInFile).
    Replace("@AETHER_VCAM_VER_COMMA@", $verComma).
    Replace("@AETHER_VCAM_VER_STR@", $verStr).
    Replace("@AETHER_VCAM_ARCH@", "x86").
    Replace("@AETHER_RELEASE_TAG@", $ReleaseTag)
[System.IO.File]::WriteAllText($rcFile, $rcText, [System.Text.Encoding]::ASCII)

$cmdLine = "call `"$vcvarsX86`" >nul && rc.exe /nologo /fo `"$resFile`" `"$rcFile`" && cl.exe /nologo /std:c++20 /W4 /WX /permissive- /Zc:__cplusplus /utf-8 $optFlag /I`"$incDir`" /Fo`"$outObj`" /Fd`"$outPdb`" /LD `"$srcFile`" `"$resFile`" /link /DEF:`"$defFile`" /IMPLIB:`"$outLib`" /OUT:`"$outDll`" ole32.lib oleaut32.lib uuid.lib strmiids.lib advapi32.lib gdi32.lib winmm.lib"

Write-Host "[vcam-x86] Building $outDll ($Config)..."
cmd.exe /c $cmdLine
if ($LASTEXITCODE -ne 0) {
    throw "x86 Virtual Cam build failed with exit code $LASTEXITCODE"
}
Write-Host "[vcam-x86] OK: $outDll"
