# Runs a .3dsx in Azahar (3DS emulator) for some seconds, saves a screenshot of its window, then closes it.
#   powershell -File azahar_shot.ps1 -Azahar <path\azahar.exe> -App gc3ds.3dsx -Seconds 20 -Out shot.png
param(
    [Parameter(Mandatory = $true)][string]$Azahar,
    [Parameter(Mandatory = $true)][string]$App,
    [int]$Seconds = 20,
    [string]$Out = "$PSScriptRoot\..\out\azahar.png"
)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Win {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
}
"@
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
$p = Start-Process -FilePath $Azahar -ArgumentList "`"$App`"" -PassThru
Start-Sleep -Seconds $Seconds
$p.Refresh()
$h = $p.MainWindowHandle
if ($h -eq [IntPtr]::Zero) { Write-Output "no window"; Stop-Process -Id $p.Id -Force; exit 1 }
$r = New-Object Win+RECT
[void][Win]::GetWindowRect($h, [ref]$r)
$w = $r.R - $r.L; $hh = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap $w, $hh
$g = [System.Drawing.Graphics]::FromImage($bmp)
# Plain screen copy of the window's area. Never PrintWindow with PW_RENDERFULLCONTENT: together with Azahar's
# OpenGL renderer it preceded a graphics-driver bugcheck (0x119) on this PC on 2026-10-10. Azahar is set to its
# software renderer for the same reason.
[void][Win]::SetForegroundWindow($h)
Start-Sleep -Milliseconds 300
$g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Stop-Process -Id $p.Id -Force
Write-Output "saved $Out (${w}x${hh})"
