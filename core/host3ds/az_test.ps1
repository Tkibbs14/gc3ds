# Builds gc3ds with a program set to autorun, runs it in Azahar (software renderer) and saves a screenshot.
#   powershell -File az_test.ps1 -Prog graphics_gx_triangle [-Seconds 25]
param([Parameter(Mandatory = $true)][string]$Prog, [int]$Seconds = 25)
$root = Split-Path (Split-Path $PSScriptRoot)
$azahar = Join-Path (Split-Path $root) "tools\azahar\azahar-windows-msvc-2126.2\azahar.exe"
wsl -d Debian -- bash -c "cd /mnt/c/Users/tyler/TP3DS/emu && AUTORUN=$Prog bash core/host3ds/build.sh 2>&1 | grep -E 'error|warning: [^/]' ; true"
& "$PSScriptRoot\azahar_shot.ps1" -Azahar $azahar -App "$PSScriptRoot\gc3ds.3dsx" -Seconds $Seconds -Out "$root\core\out\az_$Prog.png"
# Azahar logs faults that real hardware would take (reads near address 0 return 0 there instead of aborting), so
# a clean screenshot is not enough: list them.
$log = Join-Path (Split-Path $azahar) "user\log\azahar_log.txt"
$bad = Select-String -Path $log -Pattern "UnmappedAccess|GetPhysMemRegionInfo|Fatal|Critical" | Select-Object -Last 5
if ($bad) { Write-Output "AZAHAR FAULTS:"; $bad | ForEach-Object { $_.Line.Substring(0, [Math]::Min(200, $_.Line.Length)) } } else { Write-Output "azahar log: no faults" }
