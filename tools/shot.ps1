param([string]$Out = "build\screen_diag.png")
Add-Type -AssemblyName System.Drawing
$cs = @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class ShotApi {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern int GetWindowTextLength(IntPtr h);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
    public static IntPtr Find(string needle) {
        IntPtr hit = IntPtr.Zero;
        EnumWindows(delegate(IntPtr h, IntPtr l) {
            if (!IsWindowVisible(h)) return true;
            int n = GetWindowTextLength(h); if (n <= 0) return true;
            StringBuilder sb = new StringBuilder(n + 1); GetWindowText(h, sb, sb.Capacity);
            if (sb.ToString().Contains(needle)) { hit = h; return false; }
            return true;
        }, IntPtr.Zero);
        return hit;
    }
}
"@
if (-not ('ShotApi' -as [type])) { Add-Type -TypeDefinition $cs }
$h = [ShotApi]::Find("MeOS")
if ($h -eq [IntPtr]::Zero) { throw "no MeOS window" }
$null = [ShotApi]::SetForegroundWindow($h)
Start-Sleep -Milliseconds 400
$r = New-Object ShotApi+RECT
$null = [ShotApi]::GetWindowRect($h, [ref]$r)
$w = $r.Right - $r.Left; $ht = $r.Bottom - $r.Top
$bmp = New-Object System.Drawing.Bitmap $w, $ht
$g = [System.Drawing.Graphics]::FromImage($bmp)
$dc = $g.GetHdc()
$null = [ShotApi]::PrintWindow($h, $dc, 0)
$g.ReleaseHdc($dc)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Write-Output ("saved {0} {1}x{2}" -f $Out, $w, $ht)
