# Polls the 2DS results page until the run finishes (or 10 minutes pass) and saves the final text.
#   powershell -File wait_results.ps1 -Ip <3ds-ip> [-Out results.txt]
param([Parameter(Mandatory = $true)][string]$Ip, [string]$Out = "$PSScriptRoot\..\results\latest.txt")
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
$deadline = (Get-Date).AddMinutes(10)
$last = ''
while ((Get-Date) -lt $deadline) {
    try {
        $last = (Invoke-WebRequest -Uri "http://${Ip}:8333/" -TimeoutSec 8 -UseBasicParsing).Content
        Set-Content -Path $Out -Value $last -Encoding utf8
        if ($last -match 'ALL DONE') { Write-Output 'FINISHED'; exit 0 }
    } catch { }
    Start-Sleep 10
}
Write-Output 'TIMED OUT (partial results saved)'
exit 1
