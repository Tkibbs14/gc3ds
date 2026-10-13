# Builds gc3ds with a program set to autorun, runs it in Azahar (software renderer) and saves a screenshot.
#   powershell -File az_test.ps1 -Prog graphics_gx_triangle [-Seconds 25] [-GpuPos 0]   (0: position transform on the CPU)
param([Parameter(Mandatory = $true)][string]$Prog, [int]$Seconds = 25, [int]$GpuPos = 1)
$root = Split-Path (Split-Path $PSScriptRoot)
$azahar = Join-Path (Split-Path $root) "tools\azahar\azahar-windows-msvc-2126.2\azahar.exe"
$wslRoot = (wsl -d Debian -- wslpath -a ($root -replace '\\', '/')).Trim()
wsl -d Debian -- bash -c "cd $wslRoot && GPUPOS=$GpuPos AUTORUN=$Prog bash core/host3ds/build.sh 2>&1 | grep -E 'error|warning: [^/]' ; true"
& "$PSScriptRoot\azahar_shot.ps1" -Azahar $azahar -App "$PSScriptRoot\gc3ds.3dsx" -Seconds $Seconds -Out "$root\core\out\az_${Prog}_gpupos$GpuPos.png"
# Azahar logs faults that real hardware would take (reads near address 0 return 0 there instead of aborting), so
# a clean screenshot is not enough: list them.
$log = Join-Path (Split-Path $azahar) "user\log\azahar_log.txt"
$bad = Select-String -Path $log -Pattern "UnmappedAccess|GetPhysMemRegionInfo|Fatal|Critical" | Select-Object -Last 5
if ($bad) { Write-Output "AZAHAR FAULTS:"; $bad | ForEach-Object { $_.Line.Substring(0, [Math]::Min(200, $_.Line.Length)) } } else { Write-Output "azahar log: no faults" }
# The program's own screenshots of the top screen (pica_capture, at 5 s and 15 s of the run) do not depend on which
# window is in front, unlike the window copy above: the newest one written during this run, as PNG.
$shots = Join-Path (Split-Path $azahar) "user\sdmc\gc3ds\shots"
$start = (Get-Item "$PSScriptRoot\gc3ds.3dsx").LastWriteTime
$shot = Get-ChildItem $shots -Filter "shot_*.bmp" -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -gt $start } | Sort-Object LastWriteTime | Select-Object -Last 1
if ($shot) {
    Add-Type -AssemblyName System.Drawing
    $img = [System.Drawing.Image]::FromFile($shot.FullName)
    $png = "$root\core\out\az_${Prog}_gpupos${GpuPos}_shot.png"
    $img.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
    $img.Dispose()
    Write-Output "program screenshot: $png ($($shot.Name))"
} else { Write-Output "no program screenshot written during the run (run longer: shots at 5 s and 15 s of the run)" }
