# Capture the main window of a process to a PNG (works for D3D/Vulkan windows,
# also when covered: PrintWindow with PW_RENDERFULLCONTENT).
#   tools\window_shot.ps1 -ProcessId 1234 -Out shot.png
param([Parameter(Mandatory = $true)][int]$ProcessId, [Parameter(Mandatory = $true)][string]$Out,
      [ValidateSet("print", "screen")][string]$Method = "print")
Add-Type -AssemblyName System.Drawing
if (-not ("Win32Shot" -as [type])) {
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Win32Shot {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
}
"@
}
$p = Get-Process -Id $ProcessId -ErrorAction Stop
$h = $p.MainWindowHandle
if ($h -eq [IntPtr]::Zero) { throw "process $ProcessId has no main window" }
$r = New-Object Win32Shot+RECT
[Win32Shot]::GetClientRect($h, [ref]$r) | Out-Null
$w = [Math]::Max(1, $r.R - $r.L); $hgt = [Math]::Max(1, $r.B - $r.T)
$bmp = New-Object System.Drawing.Bitmap $w, $hgt
$g = [System.Drawing.Graphics]::FromImage($bmp)
if ($Method -eq "screen") {
    # what DWM actually shows in the client area (needs the window uncovered)
    $pt = New-Object Win32Shot+POINT
    [Win32Shot]::ClientToScreen($h, [ref]$pt) | Out-Null
    $g.CopyFromScreen($pt.X, $pt.Y, 0, 0, (New-Object System.Drawing.Size $w, $hgt))
    $g.Dispose()
} else {
    $hdc = $g.GetHdc()
    [Win32Shot]::PrintWindow($h, $hdc, 3) | Out-Null   # PW_CLIENTONLY | PW_RENDERFULLCONTENT
    $g.ReleaseHdc($hdc); $g.Dispose()
}
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
"$Out ($w x $hgt)"
