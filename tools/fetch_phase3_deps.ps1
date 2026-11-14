param(
    [switch]$SkipModels
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
$ToolchainsRoot = Join-Path $env:LOCALAPPDATA 'Aether\toolchains'
$OrtDir = Join-Path $ToolchainsRoot 'ort-1.24.1'
$StagingDir = Join-Path $ToolchainsRoot '_ort_stage'
$ToolsVenv = Join-Path $ToolchainsRoot 'tools-venv'

New-Item -ItemType Directory -Force -Path $ToolchainsRoot | Out-Null

$Packages = @(
    @{
        Name   = 'microsoft.ml.onnxruntime.directml'
        Ver    = '1.24.1'
        Sha256 = 'D5521507B32C638F1F29DE42CB18F21182D40A7541398786D44E38F4A1D869F1'
    },
    @{
        Name   = 'intel.ml.onnxruntime.openvino'
        Ver    = '1.24.1'
        Sha256 = 'F53AD5F90E3D616970A5C65E4880EBBE92C9774E9727020661DB591CEA74A110'
    }
)

$StampFile = Join-Path $OrtDir '.aether_ort_1.24.1_ok'
if (-not (Test-Path $StampFile)) {
    Write-Host "Provisioning pinned ONNX Runtime + OpenVINO + DirectML 1.24.1 into $OrtDir..." -ForegroundColor Cyan
    New-Item -ItemType Directory -Force -Path $StagingDir | Out-Null

    foreach ($p in $Packages) {
        $zipPath = Join-Path $StagingDir "$($p.Name).$($p.Ver).zip"
        $url = "https://api.nuget.org/v3-flatcontainer/$($p.Name)/$($p.Ver)/$($p.Name).$($p.Ver).nupkg"
        if (-not (Test-Path $zipPath)) {
            Write-Host "  Downloading $url"
            Invoke-WebRequest -Uri $url -OutFile $zipPath -UseBasicParsing
        }
        $actualHash = (Get-FileHash -Algorithm SHA256 -Path $zipPath).Hash.ToUpperInvariant()
        if ($actualHash -ne $p.Sha256) {
            Remove-Item -Force $zipPath
            throw "SHA-256 mismatch for $($p.Name) $($p.Ver): expected $($p.Sha256), got $actualHash"
        }
        $extPath = Join-Path $StagingDir $p.Name
        if (Test-Path $extPath) { Remove-Item -Recurse -Force $extPath }
        Expand-Archive -Path $zipPath -DestinationPath $extPath -Force
    }

    if (Test-Path $OrtDir) { Remove-Item -Recurse -Force $OrtDir }
    New-Item -ItemType Directory -Force -Path "$OrtDir\include","$OrtDir\lib","$OrtDir\bin" | Out-Null

    $ovRoot = Join-Path $StagingDir 'intel.ml.onnxruntime.openvino'
    $dmlRoot = Join-Path $StagingDir 'microsoft.ml.onnxruntime.directml'

    Copy-Item "$ovRoot\build\native\include\*" "$OrtDir\include\" -Force
    Copy-Item "$dmlRoot\build\native\include\*" "$OrtDir\include\" -Force

    # Intel OpenVINO EP + OpenVINO 2025.4.1 runtime plugins + oneTBB (exclude debug DLLs).
    Get-ChildItem "$ovRoot\runtimes\win-x64\native\*.dll" |
        Where-Object { $_.Name -notlike '*_debug.dll' } |
        Copy-Item -Destination "$OrtDir\bin\" -Force

    # Microsoft ONNX Runtime 1.24.1 built with DirectML EP + shared provider loader.
    Copy-Item "$dmlRoot\runtimes\win-x64\native\onnxruntime.dll" "$OrtDir\bin\onnxruntime.dll" -Force
    Copy-Item "$dmlRoot\runtimes\win-x64\native\onnxruntime_providers_shared.dll" "$OrtDir\bin\onnxruntime_providers_shared.dll" -Force
    Copy-Item "$dmlRoot\runtimes\win-x64\native\onnxruntime.lib" "$OrtDir\lib\onnxruntime.lib" -Force

    Copy-Item "$dmlRoot\LICENSE" "$OrtDir\LICENSE-ONNXRUNTIME.txt" -Force
    Copy-Item "$ovRoot\ThirdPartyNotices.txt" "$OrtDir\ThirdPartyNotices.txt" -Force

    Remove-Item -Recurse -Force $StagingDir
    $inspectDir = Join-Path $ToolchainsRoot 'ort-1.24.1-inspect'
    if (Test-Path $inspectDir) { Remove-Item -Recurse -Force $inspectDir }

    Set-Content -Path $StampFile -Value "ort=1.24.1;openvino=2025.4.1;directml=inbox" -Encoding ascii
    Write-Host "Installed pinned ORT 1.24.1 SDK cleanly." -ForegroundColor Green
} else {
    Write-Host "Pinned ORT 1.24.1 SDK already verified at $OrtDir."
}

if (-not $SkipModels) {
    $ModelsManifest = Join-Path $RepoRoot 'models\manifest.toml'
    $ModelsOk = $false
    if (Test-Path $ModelsManifest) {
        $ModelsOk = $true
    }
    if (-not (Test-Path "$ToolsVenv\Scripts\python.exe")) {
        Write-Host "Creating build-time model tooling venv at $ToolsVenv..." -ForegroundColor Cyan
        python -m venv $ToolsVenv
        & "$ToolsVenv\Scripts\pip.exe" install --quiet onnx onnxruntime onnxconverter-common flatbuffers tflite numpy pillow opencv-python openvino
    }
    Write-Host "Building & verifying Apache-2.0 static ONNX tracking models into $RepoRoot\models..." -ForegroundColor Cyan
    & "$ToolsVenv\Scripts\python.exe" (Join-Path $RepoRoot 'tools\models\build_models.py') --repo-root $RepoRoot
    if ($LASTEXITCODE -ne 0) {
        throw "Model build/quantization/NME gate failed with exit code $LASTEXITCODE"
    }
}
