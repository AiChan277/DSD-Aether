# Rebuilds the AETHER-2.5D paper: figures, IEEEtran LaTeX, Word (.docx), and PDF (via Microsoft Word).
# Usage: powershell -ExecutionPolicy Bypass -File docs\paper\build_paper.ps1 [-NodeModules <dir containing node_modules\docx>]
#        [-Snapshots build\windows-debug\tests\ui_snapshots]
# Note: Word automation can stall on ExportAsFixedFormat when launched from a nested, non-interactive
# PowerShell. If that happens, run this script directly in an interactive PowerShell window, or open the
# .docx in Word and use File > Save As > PDF.
param(
    [string]$NodeModules = '',
    [string]$Snapshots = 'build\windows-debug\tests\ui_snapshots'
)
$ErrorActionPreference = 'Stop'
$Paper = $PSScriptRoot
$Repo = Resolve-Path (Join-Path $Paper '..\..')
Set-Location $Repo

if (Test-Path $Snapshots) {
    python "$Paper\src\make_figures.py" $Snapshots "$Paper\figures"
} else {
    Write-Host "Snapshots not found at $Snapshots; keeping existing figures."
}

if ($NodeModules) { $env:NODE_PATH = $NodeModules }
node "$Paper\src\gen_tex.mjs"
node "$Paper\src\gen_docx.mjs"

$docx = Join-Path $Paper 'AETHER-2.5D_IEEE_Paper.docx'
$pdf = Join-Path $Paper 'AETHER-2.5D_IEEE_Paper.pdf'
$word = New-Object -ComObject Word.Application
$word.Visible = $true      # A hidden Word instance can stall on start-up prompts; visible export is reliable.
$word.DisplayAlerts = 0   # wdAlertsNone
try {
    $d = $word.Documents.Open($docx, $false, $true)   # FileName, ConfirmConversions, ReadOnly
    $d.ExportAsFixedFormat($pdf, 17)   # wdExportFormatPDF
    Write-Host ("PDF pages: " + $d.ComputeStatistics(2))
    $d.Close(0)
} finally {
    $word.Quit()
}
