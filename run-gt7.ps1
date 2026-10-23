# Runs GT7 (PPSA01317 v01.710) with the settings docs/GT7.md lists for the launcher, from the
# command line. The game folder holds the decrypted ELFs under their packaged names (hard links,
# see docs/GT7-PORT.md). Output goes to _kyty.txt in the working directory.
param(
    [string]$Game = 'D:\Emu\ps5\GT7-kyty-game',
    [int]$Width = 1920,
    [int]$Height = 1080,
    [ValidateSet('Fifo', 'Mailbox', 'Immediate')][string]$PresentMode = 'Mailbox',
    [string[]]$Extra = @()
)
$exe = Join-Path $PSScriptRoot '_Build\windows-noqt\kyty_emulator.exe'
$arguments = @(
    '--game', $Game,
    '--screen-width', $Width, '--screen-height', $Height,
    '--present-mode', $PresentMode,
    '--vblank-frequency', 60,
    '--shader-optimization-type', 'Performance',
    '--tessellation', '--bindless', '--redzone',
    '--printf-direction', 'File'
) + $Extra
Set-Location $PSScriptRoot
& $exe @arguments
"exit code: $LASTEXITCODE"
