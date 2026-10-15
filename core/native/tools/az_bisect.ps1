# Native mode in Azahar with GX paths switched off (gx_3ds.c reads sdmc:/gc3ds/native/gxopt.txt): for each option set,
# runs ~28 s (past the opening scene's first draws, frame ~240) and reports the last perf.csv row and whether Azahar
# was still running.   powershell -File az_bisect.ps1 -Sets "", "q", "qmwtis"
param([string[]]$Sets = @("", "q", "m", "w", "t", "qmwtis"), [int]$Seconds = 28)
$A = "C:\Users\tyler\TP3DS\tools\azahar\azahar-windows-msvc-2126.2"
$N = "$A\user\sdmc\gc3ds\native"
$app = "C:\Users\tyler\TP3DS\emu\core\out\native_az.3dsx"
Copy-Item "C:\Users\tyler\TP3DS\emu\core\out\native.3dsx" $app -Force
foreach ($set in $Sets) {
    Set-Content -Path "$N\gxopt.txt" -Value $set -NoNewline -Encoding ascii
    Remove-Item "$N\perf.csv" -ErrorAction SilentlyContinue
    $p = Start-Process -FilePath "$A\azahar.exe" -ArgumentList "`"$app`"" -PassThru
    Start-Sleep -Seconds $Seconds
    $alive = -not $p.HasExited
    if ($alive) { Stop-Process -Id $p.Id -Force }
    $last = Get-Content "$N\perf.csv" -ErrorAction SilentlyContinue | Select-Object -Last 1
    Write-Output ("[{0}] {1}  {2}" -f $set, ($(if ($alive) { "alive" } else { "DIED" })), $last)
    Start-Sleep -Seconds 2
}
Remove-Item "$N\gxopt.txt" -ErrorAction SilentlyContinue