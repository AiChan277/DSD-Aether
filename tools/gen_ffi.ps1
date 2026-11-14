param(
    [switch]$CheckOnly
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
$HeaderPath = Join-Path $RepoRoot 'include\aether_ffi_contract.h'
$CrateDir = Join-Path $RepoRoot 'crates\aether-ffi'
$ConfigPath = Join-Path $RepoRoot 'cbindgen.toml'

$TempOut = Join-Path ([System.IO.Path]::GetTempPath()) "aether_ffi_contract_gen_$PID.h"
try {
    & cbindgen --config $ConfigPath --crate aether-ffi --output $TempOut $CrateDir
    if ($LASTEXITCODE -ne 0) {
        throw "cbindgen exited with code $LASTEXITCODE"
    }

    if ($CheckOnly) {
        if (-not (Test-Path $HeaderPath)) {
            throw "Missing FFI header: $HeaderPath"
        }
        $expected = (Get-Content -Raw -Path $HeaderPath) -replace "`r`n", "`n"
        $actual = (Get-Content -Raw -Path $TempOut) -replace "`r`n", "`n"
        if ($expected -ne $actual) {
            throw "FFI header drift detected between cbindgen output and $HeaderPath. Run tools/gen_ffi.ps1."
        }
        Write-Host "FFI header check passed: $HeaderPath"
    } else {
        Copy-Item -Path $TempOut -Destination $HeaderPath -Force
        Write-Host "Regenerated $HeaderPath from crates/aether-ffi"
    }
} finally {
    if (Test-Path $TempOut) {
        Remove-Item -Force $TempOut
    }
}
