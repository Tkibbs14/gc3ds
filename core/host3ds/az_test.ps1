# Builds gc3ds with a program set to autorun, runs it in Azahar (software renderer) and saves a screenshot.
#   powershell -File az_test.ps1 -Prog graphics_gx_triangle [-Seconds 25]
param([Parameter(Mandatory = $true)][string]$Prog, [int]$Seconds = 25)
$root = Split-Path (Split-Path $PSScriptRoot)
$azahar = Join-Path (Split-Path $root) "tools\azahar\azahar-windows-msvc-2126.2\azahar.exe"
wsl -d Debian -- bash -c "cd /mnt/c/Users/tyler/TP3DS/emu && AUTORUN=$Prog bash core/host3ds/build.sh 2>&1 | grep -E 'error|warning: [^/]' ; true"
& "$PSScriptRoot\azahar_shot.ps1" -Azahar $azahar -App "$PSScriptRoot\gc3ds.3dsx" -Seconds $Seconds -Out "$root\core\out\az_$Prog.png"
