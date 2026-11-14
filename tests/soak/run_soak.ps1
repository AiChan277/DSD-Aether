param(
    [string]$Preset = 'windows-relwithdebinfo',
    [int]$SmokeFrames = 300,
    [int]$Iterations = 2,
    [int]$MaxDurationSec = 180
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$BuildDir = Join-Path $RepoRoot "build\$Preset"
$StudioExe = Join-Path $BuildDir 'src\app\aether_studio.exe'
$UiTestExe = Join-Path $BuildDir 'tests\studio_ui_test.exe'
$PluginTestExe = Join-Path $BuildDir 'tests\native_plugin_runner_test.exe'
$RefAvatar = Join-Path $RepoRoot 'tests\data\reference_avatar.vrm'
$BinDir = Join-Path $BuildDir 'vcpkg_installed\x64-windows\bin'

if (-not (Test-Path $StudioExe)) {
    throw "Missing studio executable: $StudioExe (build preset $Preset first)"
}

$prevLayer = $env:VK_ADD_LAYER_PATH
$prevPath = $env:PATH
$env:VK_ADD_LAYER_PATH = $BinDir
$env:PATH = "$BinDir;$env:PATH"

$sw = [System.Diagnostics.Stopwatch]::StartNew()
$wsSamplesMb = New-Object System.Collections.Generic.List[double]

try {
    for ($iter = 1; $iter -le $Iterations; $iter++) {
        if ($sw.Elapsed.TotalSeconds -ge $MaxDurationSec) {
            break
        }

        Write-Host "`n[SOAK iter $iter/$Iterations] Running aether_studio --smoke-frames $SmokeFrames --require-validation..." -ForegroundColor Cyan
        $argsList = @('--smoke-frames', "$SmokeFrames", '--require-validation')
        if (Test-Path $RefAvatar) {
            $argsList += @('--avatar', $RefAvatar)
        }

        $outLines = & $StudioExe @argsList 2>&1
        $rc = $LASTEXITCODE
        $outLines | ForEach-Object { Write-Host "  $_" }
        if ($rc -ne 0) {
            throw "Studio soak smoke run failed with exit code $rc at iteration $iter"
        }

        $smokeLine = ($outLines | Where-Object { $_ -match '\[AETHER_SMOKE\]' } | Select-Object -Last 1)
        if (-not $smokeLine) {
            throw "Missing [AETHER_SMOKE] metrics line from $StudioExe"
        }
        if ($smokeLine -match 'vk_errors=(\d+)' -and [int]$matches[1] -ne 0) {
            throw "Vulkan validation error reported during soak: $smokeLine"
        }
        if ($smokeLine -match 'hot_path_violations=(\d+)' -and [int]$matches[1] -ne 0) {
            throw "Hot-path allocations reported during soak: $smokeLine"
        }
        if ($smokeLine -match 'imgui_os_fallbacks=(\d+)' -and [int]$matches[1] -ne 0) {
            throw "ImGui arena OS fallback allocations reported during soak: $smokeLine"
        }
        if ($smokeLine -match 'imgui_steady_growth=(\d+)' -and [int]$matches[1] -ne 0) {
            throw "ImGui steady-state bump growth reported during soak: $smokeLine"
        }

        if (Test-Path $UiTestExe) {
            Write-Host "[SOAK iter $iter/$Iterations] Running studio_ui_test.exe..." -ForegroundColor Cyan
            $uiOut = & $UiTestExe 2>&1
            if ($LASTEXITCODE -ne 0) {
                $uiOut | ForEach-Object { Write-Host "  $_" }
                throw "studio_ui_test failed with exit code $LASTEXITCODE"
            }
            $uiPerf = ($uiOut | Where-Object { $_ -match '\[STUDIO_UI_PERF\]' } | Select-Object -Last 1)
            if ($uiPerf) { Write-Host "  $uiPerf" }
        }

        if (Test-Path $PluginTestExe) {
            Write-Host "[SOAK iter $iter/$Iterations] Running native_plugin_runner_test.exe..." -ForegroundColor Cyan
            $plgOut = & $PluginTestExe 2>&1
            if ($LASTEXITCODE -ne 0) {
                $plgOut | ForEach-Object { Write-Host "  $_" }
                throw "native_plugin_runner_test failed with exit code $LASTEXITCODE"
            }
        }

        $proc = Get-Process -Id $PID
        $wsSamplesMb.Add([math]::Round($proc.WorkingSet64 / 1MB, 2))
    }

    Write-Host "`nSOAK TEST PASSED ($($sw.Elapsed.TotalSeconds.ToString('F1')) s, iterations=$Iterations, preset=$Preset)." -ForegroundColor Green
} finally {
    $env:VK_ADD_LAYER_PATH = $prevLayer
    $env:PATH = $prevPath
}
