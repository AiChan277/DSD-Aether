param(
    [switch]$SkipClangFormat,
    [switch]$SkipClangTidy,
    [switch]$SkipPresets,
    [string[]]$Presets = @('windows-debug', 'windows-relwithdebinfo', 'windows-release', 'ci-validation')
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
Push-Location $RepoRoot

function Invoke-Step {
    param([string]$Title, [scriptblock]$Action)
    Write-Host "`n=== $Title ===" -ForegroundColor Cyan
    & $Action
    if ($LASTEXITCODE -ne 0) {
        throw "Step failed ($LASTEXITCODE): $Title"
    }
}

function Import-MsvcEnv {
    $vcvars = 'D:\VS2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path $vcvars)) {
        throw "Missing vcvars64.bat at $vcvars"
    }
    $lines = cmd /c "`"$vcvars`" >nul && set"
    foreach ($line in $lines) {
        if ($line -match '^([^=]+)=(.*)$') {
            [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2])
        }
    }
    $vsCmake = 'D:\VS2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'
    $vsNinja = 'D:\VS2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
    $vsLlvm  = 'D:\VS2022\BuildTools\VC\Tools\Llvm\x64\bin'
    $cleanPath = ($env:PATH -split ';' | Where-Object { $_ -and ($_ -notmatch 'GNU_Octave|mingw|msys|Cygwin') }) -join ';'
    $env:PATH = "$vsCmake;$vsNinja;$vsLlvm;$cleanPath"
    if (-not $env:VCPKG_ROOT) {
        $env:VCPKG_ROOT = 'D:\VS2022\BuildTools\VC\vcpkg'
    }
    if ((Test-Path 'D:\vcpkg-cache\archives') -and (-not $env:VCPKG_DEFAULT_BINARY_CACHE)) {
        $env:VCPKG_DEFAULT_BINARY_CACHE = 'D:\vcpkg-cache\archives'
    }
}

try {
    Import-MsvcEnv

    Invoke-Step 'Static HR & UI Token Gates + Tabular Numerals' {
        python "$RepoRoot\tools\check_gates.py"
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        python "$RepoRoot\tools\gen_params_doc.py" --check
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        python "$RepoRoot\tools\font_tnum_freeze.py"
    }

    if (-not $SkipClangFormat) {
        Invoke-Step 'clang-format Check' {
            $cppFiles = Get-ChildItem -Path "$RepoRoot\src", "$RepoRoot\tests" -Recurse -File |
                Where-Object { $_.Extension -in '.h', '.hpp', '.cpp' } |
                ForEach-Object { $_.FullName }
            if ($cppFiles.Count -gt 0) {
                & clang-format --dry-run --Werror @cppFiles
            }
        }
    }

    Invoke-Step 'Rust Format, Clippy (default & test-hooks), Cargo Deny, Cargo Tests' {
        cargo fmt --check
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        cargo clippy --workspace --all-targets --offline -- -D warnings
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        cargo clippy --workspace --all-features --all-targets --offline -- -D warnings
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        cargo deny --offline check licenses bans sources
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        cargo test --workspace --offline
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        cargo test --workspace --features test-hooks --offline
    }

    Invoke-Step 'FFI Header Parity Check (cbindgen)' {
        powershell -NoProfile -ExecutionPolicy Bypass -File "$RepoRoot\tools\gen_ffi.ps1" -CheckOnly
    }

    if (-not $SkipPresets) {
        foreach ($preset in $Presets) {
            $buildDir = Join-Path $RepoRoot "build\$preset"

            Invoke-Step "CMake Configure ($preset)" {
                cmake --preset $preset
            }
            Invoke-Step "CMake Build ($preset)" {
                $pyBuild = @'
import subprocess, sys, time
rc = 0
for attempt in (1, 2, 3):
    p = subprocess.Popen(sys.argv[1:], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding='utf-8', errors='replace')
    out = []
    for line in p.stdout:
        sys.stdout.write(line)
        out.append(line)
    rc = p.wait()
    if rc == 0:
        break
    joined = ''.join(out)
    if attempt < 3 and ('LNK1104' in joined or 'LNK1168' in joined):
        sys.stderr.write(f'[ci.ps1] Transient MSVC lock (LNK1104/LNK1168) on attempt {attempt}; retrying...\n')
        time.sleep(2.0)
        continue
    break
sys.exit(rc)
'@
                python -c $pyBuild cmake --build --preset $preset
            }

            if ($preset -ne 'windows-release') {
                Invoke-Step "CTest Suite ($preset)" {
                    $layerDir = Join-Path $buildDir 'vcpkg_installed\x64-windows\bin'
                    $prevLayer = $env:VK_ADD_LAYER_PATH
                    $prevPath = $env:PATH
                    $env:VK_ADD_LAYER_PATH = $layerDir
                    $env:PATH = "$layerDir;$env:PATH"
                    try {
                        ctest --preset $preset --output-on-failure
                    } finally {
                        $env:VK_ADD_LAYER_PATH = $prevLayer
                        $env:PATH = $prevPath
                    }
                }
            }

            if ($preset -eq 'windows-debug') {
                Invoke-Step 'App Windowed Vulkan Validation Smoke (windows-debug, 30 frames)' {
                    $exe = Join-Path $buildDir 'src\app\aether_studio.exe'
                    $layerDir = Join-Path $buildDir 'vcpkg_installed\x64-windows\bin'
                    $prevLayer = $env:VK_ADD_LAYER_PATH
                    $prevPath = $env:PATH
                    $env:VK_ADD_LAYER_PATH = $layerDir
                    $env:PATH = "$layerDir;$env:PATH"
                    try {
                        & $exe --smoke-frames 30 --require-validation
                        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
                        $refAvatar = Join-Path $RepoRoot 'tests\data\reference_avatar.vrm'
                        if (Test-Path $refAvatar) {
                            & $exe --smoke-frames 30 --require-validation --avatar $refAvatar
                        }
                    } finally {
                        $env:VK_ADD_LAYER_PATH = $prevLayer
                        $env:PATH = $prevPath
                    }
                }
            }
        }
    }

    if (-not $SkipClangTidy) {
        $ccDb = Join-Path $RepoRoot 'build\windows-relwithdebinfo\compile_commands.json'
        if (Test-Path $ccDb) {
            Invoke-Step 'clang-tidy Check (core/rhi/ui/app)' {
                $tidyFiles = Get-ChildItem -Path "$RepoRoot\src" -Recurse -Filter '*.cpp' |
                    ForEach-Object { $_.FullName }
                & clang-tidy -p (Join-Path $RepoRoot 'build\windows-relwithdebinfo') `
                    --extra-arg=-Wno-unused-command-line-argument `
                    --extra-arg=-Wno-missing-field-initializers `
                    --quiet @tidyFiles
            }
        }
    }

    Write-Host "`nALL AETHER-2.5D CI GATES PASSED." -ForegroundColor Green
} finally {
    Pop-Location
}
