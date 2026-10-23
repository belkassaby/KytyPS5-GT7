# Runs GT7 (PPSA01317 v01.710) with the settings docs/GT7.md lists for the launcher, from the
# command line. The game folder holds the decrypted ELFs under their packaged names (hard links,
# see docs/GT7-PORT.md). Output goes to _kyty.txt in the working directory.
param(
    [string]$Game = 'D:\Emu\ps5\GT7-kyty-game',
    [int]$Width = 1920,
    [int]$Height = 1080,
    [ValidateSet('Fifo', 'Mailbox', 'Immediate')][string]$PresentMode = 'Mailbox',
    [ValidateSet('None', 'Size', 'Performance')][string]$ShaderOptimization = 'Performance',
    [switch]$NoBindless,
    # Guest shaders to skip. 3a2026af8161d470: a compute shader NVIDIA's 591.86 laptop driver crashes
    # compiling (in vkCreateComputePipelines, at the GT logo), whatever the SPIR-V optimization.
    [string]$SkipShaders = '3a2026af8161d470',
    [string[]]$Extra = @()
)
$exe = Join-Path $PSScriptRoot '_Build\windows-noqt\kyty_emulator.exe'
$arguments = @(
    '--game', $Game,
    '--screen-width', $Width, '--screen-height', $Height,
    '--present-mode', $PresentMode,
    '--vblank-frequency', 60,
    '--shader-optimization-type', $ShaderOptimization,
    '--tessellation', '--redzone',
    '--printf-direction', 'File'
) + $(if ($SkipShaders) { @('--skip-shaders', $SkipShaders) } else { @() }) + $(if ($NoBindless) { @() } else { @('--bindless') }) + $Extra
Set-Location $PSScriptRoot
& $exe @arguments
"exit code: $LASTEXITCODE"
