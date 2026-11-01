# SPDX-License-Identifier: Apache-2.0
#
# Windows host actions of the VS Code tasks (.vscode/tasks.json), used when
# the folder is open on Windows (not in the Dev Container). The Linux and Dev
# Container counterpart is .vscode/workspace-menu.sh.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File .vscode\host-menu.ps1 "<action>"
#
# - Zephyr: west runs in the Docker image (compose.yaml, service zephyr) with
#   the same action of workspace-menu.sh, in the same doppelstep-west volume
#   as the Dev Container.
# - Debug probe: GDB servers and flashing with the tools installed on Windows
#   (scripts/host/).
# - GUI and documentation: Python virtual environment .venv with
#   requirements.txt; the PDF manual is printed with Microsoft Edge.

param(
    [Parameter(Mandatory = $true)]
    [string]$Action
)

# Native tools (pip, docker) write progress to stderr; failures are detected
# through exit codes below instead of PowerShell error records.
$ErrorActionPreference = 'Continue'

$Root = Split-Path -Parent $PSScriptRoot
$Venv = Join-Path $Root '.venv'
$VenvPython = Join-Path $Venv 'Scripts\python.exe'
$Requirements = Join-Path $Root 'requirements.txt'
$Gui = Join-Path $Root 'tools\gui\doppelstep_gui.py'
$BuildDir = Join-Path $Root 'build'
$Elf = Join-Path $BuildDir 'zephyr\zephyr.elf'
$Pdf = Join-Path $Root 'site\Doppelstep.pdf'
$HostScripts = Join-Path $Root 'scripts\host'

Set-Location $Root

function Stop-WithError([string]$Message) {
    Write-Host "ERROR: $Message" -ForegroundColor Red
    exit 1
}

function Invoke-Checked([string]$Exe, [string[]]$Arguments) {
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) {
        Stop-WithError "Command failed ($LASTEXITCODE): $Exe $($Arguments -join ' ')"
    }
}

function Test-PythonImport([string]$Python, [string]$Code) {
    & $Python -c $Code *> $null
    return ($LASTEXITCODE -eq 0)
}

# --- Zephyr in Docker ------------------------------------------------------------

function Invoke-ZephyrInDocker([string]$LinuxAction) {
    if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
        Stop-WithError 'Docker was not found. Install Docker Desktop (WSL 2 backend), or open the folder in the Dev Container.'
    }
    Write-Host "Running `"$LinuxAction`" in the Docker image (compose service zephyr)."
    Invoke-Checked 'docker' @('compose', 'run', '--rm', 'zephyr', 'bash', '.vscode/workspace-menu.sh', $LinuxAction)
}

# --- Debug probe on the host ----------------------------------------------------

function Invoke-HostFlash {
    if (-not (Test-Path $Elf)) {
        Invoke-ZephyrInDocker 'Build firmware'
    }
    if (-not (Get-Command openocd -ErrorAction SilentlyContinue)) {
        Stop-WithError 'openocd was not found in PATH. Install OpenOCD 0.12+ (for example xPack) and the ST-LINK driver.'
    }
    $cfg = Join-Path $Root 'apps\boards\st\b_g431b_esc1\support\openocd.cfg'
    # OpenOCD's Tcl parser takes forward slashes on Windows.
    $image = $Elf -replace '\\', '/'
    Invoke-Checked 'openocd' @('-f', $cfg, '-c', "program {$image} verify reset exit")
}

# --- Python tools ---------------------------------------------------------------

function New-Venv {
    if (Get-Command py -ErrorAction SilentlyContinue) {
        Invoke-Checked 'py' @('-3', '-m', 'venv', $Venv)
    }
    elseif (Get-Command python -ErrorAction SilentlyContinue) {
        Invoke-Checked 'python' @('-m', 'venv', $Venv)
    }
    else {
        Stop-WithError 'Python 3.10+ was not found. Install it from https://www.python.org/ (with "tcl/tk and IDLE") and try again.'
    }
}

function Initialize-Venv {
    if (-not (Test-Path $VenvPython)) {
        Write-Host "Creating virtual environment in $Venv"
        New-Venv
    }
    Write-Host 'Installing requirements.txt.'
    Invoke-Checked $VenvPython @('-m', 'pip', 'install', '--upgrade', 'pip')
    Invoke-Checked $VenvPython @('-m', 'pip', 'install', '-r', $Requirements)
    Write-Host ".venv is ready: $VenvPython" -ForegroundColor Green
}

# Creates or updates .venv unless the given imports already work.
function Confirm-Venv([string]$Imports) {
    if (-not ((Test-Path $VenvPython) -and (Test-PythonImport $VenvPython "import $Imports"))) {
        Initialize-Venv
    }
}

function Start-Gui([string[]]$GuiArgs) {
    if (-not (Test-Path $Gui)) {
        Stop-WithError "GUI not present yet: expected tools\gui\doppelstep_gui.py (see tools\gui\README.md)."
    }
    Confirm-Venv 'serial, tkinter'
    if (-not (Test-PythonImport $VenvPython 'import tkinter')) {
        Stop-WithError 'Tkinter is missing in this Python. Reinstall Python from python.org with "tcl/tk and IDLE", delete .venv and run "Create .venv" again.'
    }
    Write-Host 'Starting the Doppelstep GUI. Close the window to finish the task.'
    Invoke-Checked $VenvPython (@($Gui) + $GuiArgs)
}

function Build-PdfDocs {
    Confirm-Venv 'mkdocs, material, playwright, pypdf'
    Write-Host 'Building the PDF manual (MkDocs + Microsoft Edge/Chromium).'
    Invoke-Checked $VenvPython @((Join-Path $Root 'tools\docs\build_pdf.py'))
    Write-Host "PDF manual: $Pdf" -ForegroundColor Green
}

function Start-DocsServer {
    Confirm-Venv 'mkdocs, material'
    Write-Host 'Documentation preview on http://127.0.0.1:8000 (Ctrl+C stops it).'
    $env:NO_MKDOCS_2_WARNING = '1'
    Invoke-Checked $VenvPython @('-m', 'mkdocs', 'serve', '-a', '127.0.0.1:8000')
}

# --- Actions --------------------------------------------------------------------

switch ($Action) {
    'Initialize + update' { Invoke-ZephyrInDocker $Action }
    'Build firmware' { Invoke-ZephyrInDocker $Action }
    'Pristine build' { Invoke-ZephyrInDocker $Action }
    'Clean' {
        Write-Host "Removing $BuildDir"
        if (Test-Path $BuildDir) {
            Remove-Item -Recurse -Force $BuildDir
        }
    }
    'Flash (host OpenOCD, ST-LINK)' { Invoke-HostFlash }
    'GDB server (host OpenOCD, ST-LINK)' { & (Join-Path $HostScripts 'openocd-server.ps1') }
    'GDB server + RTT (host OpenOCD, ST-LINK)' { & (Join-Path $HostScripts 'openocd-server.ps1') -Rtt }
    'GDB server (host J-Link)' { & (Join-Path $HostScripts 'jlink-gdb-server.ps1') }
    'J-Link Remote Server (host)' { & (Join-Path $HostScripts 'jlink-remote-server.ps1') }
    'Create .venv' { Initialize-Venv }
    'Run GUI' { Start-Gui @() }
    'Run GUI (simulator)' { Start-Gui @('--sim') }
    'Serve documentation' { Start-DocsServer }
    'Build PDF documentation' { Build-PdfDocs }
    default {
        Write-Host "Unknown workspace action: $Action" -ForegroundColor Red
        exit 2
    }
}
