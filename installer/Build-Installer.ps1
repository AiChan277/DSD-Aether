<#
.SYNOPSIS
  Builds the AETHER-2.5D Studio Windows installer (MSI) and portable ZIP from a windows-release build.

.DESCRIPTION
  1. Stages a clean payload in dist\stage (exe, plugin runner, runtime DLLs, app-local VC++ runtime, tracking
     models, fonts, default config, license / readme). No PDBs, import libraries, or build intermediates.
  2. Stages the x64 / x86 DirectShow virtual camera DLLs, each with its own VC++ runtime.
  3. Builds dist\AetherStudio-<tag>-x64.msi with WiX v5 and dist\AetherStudio-<tag>-win64-portable.zip.
  4. Optionally Authenticode-signs the exe, DLLs and MSI when -CertThumbprint is given.
  5. Writes dist\SHA256SUMS.txt.

  Prerequisites:
    cmake --build --preset windows-release
    powershell -File tools\build_vcam_x86.ps1 -Config Release
    dotnet tool install --global wix --version 5.0.2
    wix extension add -g WixToolset.UI.wixext/5.0.2 ; wix extension add -g WixToolset.Util.wixext/5.0.2
#>
param(
    [string]$ReleaseBuildDir = "build/windows-release",
    [string]$VcamX86BuildDir = "build/vcam-x86/Release",
    [string]$Version = "1.0.3",
    [string]$ReleaseTag = "a1.0.3B",
    [string]$CrtRedistRoot = "",
    # Optional: SHA-1 thumbprint of a code-signing certificate in Cert:\CurrentUser\My (OV/EV or Azure Trusted Signing).
    [string]$CertThumbprint = "",
    [string]$TimestampUrl = "http://timestamp.digicert.com"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
function Resolve-RepoPath([string]$p) {
    if ([System.IO.Path]::IsPathRooted($p)) { return $p }
    return (Join-Path $repoRoot $p)
}

$wix = Get-Command wix -ErrorAction SilentlyContinue
if (-not $wix) {
    $cand = Join-Path $env:USERPROFILE ".dotnet\tools\wix.exe"
    if (Test-Path $cand) { $wix = Get-Item $cand } else {
        throw "WiX v5 CLI not found. Install with: dotnet tool install --global wix --version 5.0.2"
    }
}
$wixExe = if ($wix -is [System.Management.Automation.CommandInfo]) { $wix.Source } else { $wix.FullName }

$release = Resolve-RepoPath $ReleaseBuildDir
$appDir = Join-Path $release "src\app"
$runner = Join-Path $release "src\plugin_runner\aether-plugin-runner.exe"
$vcamX64 = Join-Path $release "bin\aether_vcam_ds_x64.dll"
$vcamX86 = Join-Path (Resolve-RepoPath $VcamX86BuildDir) "aether_vcam_ds_x86.dll"
foreach ($req in @((Join-Path $appDir "aether_studio.exe"), $runner, $vcamX64, $vcamX86)) {
    if (-not (Test-Path $req)) { throw "Required payload missing: $req" }
}

# Visual C++ 2015-2022 (VC143) app-local runtime (redistributable per the Visual Studio license).
if (-not $CrtRedistRoot) {
    $vsRoots = @("D:\VS2022\BuildTools", "${env:ProgramFiles}\Microsoft Visual Studio\2022\BuildTools",
                 "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community")
    foreach ($r in $vsRoots) {
        $found = Get-ChildItem -Path (Join-Path $r "VC\Redist\MSVC") -Directory -ErrorAction SilentlyContinue |
            Where-Object { Test-Path (Join-Path $_.FullName "x64\Microsoft.VC143.CRT") } |
            Sort-Object Name -Descending | Select-Object -First 1
        if ($found) { $CrtRedistRoot = $found.FullName; break }
    }
}
if (-not $CrtRedistRoot) { throw "VC143 CRT redistributable folder not found; pass -CrtRedistRoot." }
$crtX64 = Join-Path $CrtRedistRoot "x64\Microsoft.VC143.CRT"
$crtX86 = Join-Path $CrtRedistRoot "x86\Microsoft.VC143.CRT"
$crtFiles = @("msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll", "msvcp140_atomic_wait.dll",
              "msvcp140_codecvt_ids.dll", "vcruntime140.dll", "vcruntime140_1.dll", "concrt140.dll")

$dist = Join-Path $repoRoot "dist"
$stage = Join-Path $dist "stage"
$vcStage64 = Join-Path $dist "vcam\x64"
$vcStage86 = Join-Path $dist "vcam\x86"
foreach ($d in @($stage, (Join-Path $dist "vcam"))) {
    if (Test-Path $d) { Remove-Item -Recurse -Force $d }
}
New-Item -ItemType Directory -Force -Path $stage, $vcStage64, $vcStage86 | Out-Null

Write-Host "[installer] Staging payload -> $stage"
Copy-Item (Join-Path $appDir "aether_studio.exe") $stage
Copy-Item $runner $stage
Get-ChildItem $appDir -Filter *.dll | Copy-Item -Destination $stage
foreach ($f in $crtFiles) { Copy-Item (Join-Path $crtX64 $f) $stage }

New-Item -ItemType Directory -Force -Path (Join-Path $stage "models"), (Join-Path $stage "assets\fonts"),
    (Join-Path $stage "config") | Out-Null
$manifest = Join-Path $repoRoot "models\manifest.toml"
Copy-Item $manifest (Join-Path $stage "models")
# Only ship the model files the manifest references.
$modelFiles = Select-String -Path $manifest -Pattern 'file_\w+\s*=\s*"([^"]+)"' -AllMatches |
    ForEach-Object { $_.Matches } | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique
foreach ($m in $modelFiles) {
    $src = Join-Path $repoRoot "models\$m"
    if (-not (Test-Path $src)) { throw "Model referenced by manifest is missing: $src (run tools\models\build_models.py)" }
    Copy-Item $src (Join-Path $stage "models")
}
Copy-Item (Join-Path $repoRoot "assets\fonts\*") (Join-Path $stage "assets\fonts")
Copy-Item (Join-Path $repoRoot "config\*.json") (Join-Path $stage "config")
Copy-Item (Join-Path $repoRoot "LICENSE") (Join-Path $stage "LICENSE.txt")
Copy-Item (Join-Path $repoRoot "README.md") (Join-Path $stage "README.md")

Copy-Item $vcamX64 $vcStage64
Copy-Item $vcamX86 $vcStage86
foreach ($f in @("vcruntime140.dll", "vcruntime140_1.dll")) { Copy-Item (Join-Path $crtX64 $f) $vcStage64 }
Copy-Item (Join-Path $crtX86 "vcruntime140.dll") $vcStage86

# License text -> RTF for the wizard's license page.
$rtf = Join-Path $dist "LICENSE.rtf"
$lic = Get-Content (Join-Path $repoRoot "LICENSE") -Raw -Encoding UTF8
$esc = $lic.Replace('\', '\\').Replace('{', '\{').Replace('}', '\}')
$sb = New-Object System.Text.StringBuilder
foreach ($ch in $esc.ToCharArray()) {
    $c = [int]$ch
    if ($ch -eq "`n") { [void]$sb.Append("\par`n") }
    elseif ($ch -eq "`r") { }
    elseif ($c -gt 127) { [void]$sb.Append("\u" + $c + "?") }
    else { [void]$sb.Append($ch) }
}
$rtfText = "{\rtf1\ansi\deff0{\fonttbl{\f0 Segoe UI;}}\fs17 " + $sb.ToString() + "}"
[System.IO.File]::WriteAllText($rtf, $rtfText, [System.Text.Encoding]::ASCII)

function Invoke-Sign([string[]]$files) {
    if (-not $CertThumbprint) { return }
    $signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe" |
        Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $signtool) { throw "signtool.exe not found (install the Windows 10/11 SDK)" }
    & $signtool.FullName sign /sha1 $CertThumbprint /fd SHA256 /tr $TimestampUrl /td SHA256 /d "AETHER-2.5D Studio" $files
    if ($LASTEXITCODE -ne 0) { throw "signtool failed ($LASTEXITCODE)" }
}
# Sign only our own binaries (third-party DLLs keep their vendor signatures).
Invoke-Sign @((Join-Path $stage "aether_studio.exe"), (Join-Path $stage "aether-plugin-runner.exe"),
              (Join-Path $vcStage64 "aether_vcam_ds_x64.dll"), (Join-Path $vcStage86 "aether_vcam_ds_x86.dll"))

$msi = Join-Path $dist "AetherStudio-$ReleaseTag-x64.msi"
$wxs = Join-Path $PSScriptRoot "AetherStudio.wxs"
Write-Host "[installer] Building $msi"
& $wixExe build $wxs -arch x64 `
    -ext WixToolset.UI.wixext -ext WixToolset.Util.wixext `
    -d "ProductVersion=$Version" -d "ReleaseTag=$ReleaseTag" `
    -d "StageDir=$stage" -d "VcamX64Dir=$vcStage64" -d "VcamX86Dir=$vcStage86" `
    -d "AssetsDir=$(Join-Path $PSScriptRoot 'assets')" -d "LicenseRtf=$rtf" `
    -o $msi
if ($LASTEXITCODE -ne 0) { throw "wix build failed with exit code $LASTEXITCODE" }
Remove-Item -Force -ErrorAction SilentlyContinue ([System.IO.Path]::ChangeExtension($msi, ".wixpdb"))
Invoke-Sign @($msi)

# Portable ZIP (same payload; portable.flag keeps settings next to the exe).
$zipRoot = Join-Path $dist "AetherStudio-$ReleaseTag-win64"
if (Test-Path $zipRoot) { Remove-Item -Recurse -Force $zipRoot }
Copy-Item -Recurse $stage $zipRoot
Copy-Item -Recurse (Join-Path $dist "vcam") (Join-Path $zipRoot "vcam")
Set-Content -Path (Join-Path $zipRoot "portable.flag") -Value "Settings are stored in .\UserData" -Encoding ASCII
$portableNote = @"
AETHER-2.5D Studio ($ReleaseTag) - Portable Build
==================================================

1. Double-click aether_studio.exe to launch.
2. Because portable.flag is present in this folder, all settings, caches,
   crash dumps, and custom presets are stored locally inside .\UserData.
3. Spout2 zero-copy output works out of the box without admin rights.
4. DirectShow Virtual Camera (optional in portable mode):
   Run an Administrator Command Prompt in this folder and execute:
     regsvr32 vcam\x64\aether_vcam_ds_x64.dll
     regsvr32 vcam\x86\aether_vcam_ds_x86.dll
   (Or use AetherStudio-$ReleaseTag-x64.msi, which registers both automatically.)
"@
Set-Content -Path (Join-Path $zipRoot "README-PORTABLE.txt") -Value $portableNote -Encoding ASCII
$zip = Join-Path $dist "AetherStudio-$ReleaseTag-win64-portable.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $zipRoot "*") -DestinationPath $zip -CompressionLevel Optimal
Remove-Item -Recurse -Force $zipRoot

$sums = Join-Path $dist "SHA256SUMS.txt"
Get-FileHash -Algorithm SHA256 $msi, $zip |
    ForEach-Object { "{0}  {1}" -f $_.Hash.ToLowerInvariant(), (Split-Path -Leaf $_.Path) } |
    Set-Content -Path $sums -Encoding ASCII
Write-Host "[installer] Done:"
Get-Item $msi, $zip, $sums | ForEach-Object { "  {0}  ({1:N1} MB)" -f $_.Name, ($_.Length / 1MB) }
if (-not $CertThumbprint) {
    Write-Warning "Unsigned build. Windows SmartScreen will show 'Unknown publisher' until the files are Authenticode-signed (see installer\README.md)."
}
