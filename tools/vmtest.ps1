<#
    MeOS · 虚拟机一键验证

    干四件事：
        1. 关掉旧实例、清掉旧日志；
        2. 启动虚拟机（图形界面），等它引导完；
        3. 截图客户机画面；
        4. 读 vmware.log，判定虚拟机是「活着」还是「panic 了」。

    截图这一段有两个坑，都踩过了：
      · 不能按进程名找 vmware.exe 的 MainWindowHandle——那是 Workstation 主界面，
        虚拟机控制台只是它里面的一个标签页；得按窗口标题找 VMUIFrame。
      · 光置顶还不够，桌面上别的窗口（比如正在跑 Codex 的终端）照样会被截进来。
        所以先把与虚拟机窗口重叠的窗口临时最小化，截完再恢复。

    用法：
        .\tools\vmtest.ps1                  默认等 10 秒后截图
        .\tools\vmtest.ps1 -Wait 15 -Keep    等 15 秒，并且不关虚拟机
#>
param(
    [int]$Wait = 10,
    [string]$Title = "MeOS",
    [string]$Out = "",
    [switch]$Keep
)

$ErrorActionPreference = 'Continue'

$Root     = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $Root 'build'
$Vmx      = Join-Path $BuildDir 'meos.vmx'
$Log      = Join-Path $BuildDir 'vmware.log'
$Vmrun    = 'D:\Program Files\vmrun.exe'
if (-not $Out) { $Out = Join-Path $BuildDir 'screen.png' }

Write-Host '[1/4] 关掉旧实例 ...'
$null = & $Vmrun -T ws stop $Vmx hard
Start-Sleep -Seconds 2
if (Test-Path -LiteralPath $Log) { Remove-Item -LiteralPath $Log -Force }

Write-Host '[2/4] 启动虚拟机（图形界面，不阻塞） ...'
Start-Process -FilePath $Vmrun -ArgumentList @('-T', 'ws', 'start', $Vmx, 'gui') -WindowStyle Hidden

Write-Host '[3/4] 等虚拟机控制台窗口 ...'
Add-Type -AssemblyName System.Drawing
$cs = @(
    'using System;',
    'using System.Collections.Generic;',
    'using System.Text;',
    'using System.Runtime.InteropServices;',
    'public class MeOSApi {',
    '    [StructLayout(LayoutKind.Sequential)]',
    '    public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }',
    '    public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);',
    '    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);',
    '    [DllImport("user32.dll")] public static extern int GetWindowTextLength(IntPtr hWnd);',
    '    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr hWnd, StringBuilder s, int n);',
    '    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr hWnd, StringBuilder s, int n);',
    '    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);',
    '    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr hWnd);',
    '    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);',
    '    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);',
    '    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT r);',
    '    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hWnd, IntPtr after, int x, int y, int cx, int cy, uint flags);',
    '    public static readonly IntPtr HWND_TOPMOST = new IntPtr(-1);',
    '    public static readonly IntPtr HWND_NOTOPMOST = new IntPtr(-2);',
    '    public const uint SWP_NOSIZE=0x1, SWP_NOMOVE=0x2, SWP_SHOWWINDOW=0x40;',
    '    public static List<IntPtr> Find(string needle) {',
    '        List<IntPtr> hits = new List<IntPtr>();',
    '        EnumWindows(delegate(IntPtr h, IntPtr p) {',
    '            if (!IsWindowVisible(h)) return true;',
    '            int n = GetWindowTextLength(h);',
    '            if (n <= 0) return true;',
    '            StringBuilder sb = new StringBuilder(n + 1);',
    '            GetWindowText(h, sb, sb.Capacity);',
    '            if (sb.ToString().Contains(needle)) hits.Add(h);',
    '            return true;',
    '        }, IntPtr.Zero);',
    '        return hits;',
    '    }',
    '    public static string TitleOf(IntPtr h) {',
    '        int n = GetWindowTextLength(h);',
    '        StringBuilder sb = new StringBuilder(n + 2);',
    '        GetWindowText(h, sb, sb.Capacity);',
    '        return sb.ToString();',
    '    }',
    '    // 把与 r 相交的可见窗口最小化，返回被最小化的窗口，供事后恢复',
    '    public static List<IntPtr> MinimizeOverlapping(IntPtr self, RECT r) {',
    '        List<IntPtr> done = new List<IntPtr>();',
    '        EnumWindows(delegate(IntPtr h, IntPtr p) {',
    '            if (h == self) return true;',
    '            if (!IsWindowVisible(h) || IsIconic(h)) return true;',
    '            StringBuilder cn = new StringBuilder(128);',
    '            GetClassName(h, cn, cn.Capacity);',
    '            string cls = cn.ToString();',
    '            if (cls == "Progman" || cls == "WorkerW" || cls == "Shell_TrayWnd" ||',
    '                cls == "Windows.UI.Core.CoreWindow") return true;',
    '            RECT q;',
    '            if (!GetWindowRect(h, out q)) return true;',
    '            bool overlap = q.Left < r.Right && q.Right > r.Left && q.Top < r.Bottom && q.Bottom > r.Top;',
    '            if (!overlap) return true;',
    '            ShowWindow(h, 6);   // SW_MINIMIZE',
    '            done.Add(h);',
    '            return true;',
    '        }, IntPtr.Zero);',
    '        return done;',
    '    }',
    '}'
) -join [Environment]::NewLine
if (-not ('MeOSApi' -as [type])) { Add-Type -TypeDefinition $cs }

$hwnd = [IntPtr]::Zero
for ($i = 0; $i -lt 40; $i++) {
    Start-Sleep -Seconds 1
    $hits = [MeOSApi]::Find($Title)
    if ($hits.Count -gt 0) { $hwnd = $hits[0]; break }
}
if ($hwnd -eq [IntPtr]::Zero) { throw "没有找到标题含 '$Title' 的窗口，虚拟机可能没起来。" }
Write-Host ("  窗口标题：{0}" -f [MeOSApi]::TitleOf($hwnd))

Start-Sleep -Seconds $Wait

Write-Host '[4/4] 清场、置顶、截图 ...'
$rect = New-Object MeOSApi+RECT
$null = [MeOSApi]::GetWindowRect($hwnd, [ref]$rect)
$hidden = [MeOSApi]::MinimizeOverlapping($hwnd, $rect)
Write-Host ("  临时最小化了 {0} 个挡路的窗口" -f $hidden.Count)

$null = [MeOSApi]::ShowWindow($hwnd, 3)
$null = [MeOSApi]::SetWindowPos($hwnd, [MeOSApi]::HWND_TOPMOST, 0, 0, 0, 0,
        ([MeOSApi]::SWP_NOMOVE -bor [MeOSApi]::SWP_NOSIZE -bor [MeOSApi]::SWP_SHOWWINDOW))
$null = [MeOSApi]::SetForegroundWindow($hwnd)
Start-Sleep -Milliseconds 2500

$null = [MeOSApi]::GetWindowRect($hwnd, [ref]$rect)
$width  = $rect.Right - $rect.Left
$height = $rect.Bottom - $rect.Top
if ($width -gt 0 -and $height -gt 0) {
    $bmp = New-Object System.Drawing.Bitmap $width, $height
    $gfx = [System.Drawing.Graphics]::FromImage($bmp)
    $gfx.CopyFromScreen($rect.Left, $rect.Top, 0, 0, (New-Object System.Drawing.Size $width, $height))
    $bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
    $gfx.Dispose(); $bmp.Dispose()
    Write-Host ('  截图：{0}  ({1}x{2}) at({3},{4})' -f $Out, $width, $height, $rect.Left, $rect.Top)
} else {
    Write-Host '  窗口尺寸异常，跳过截图' -ForegroundColor Yellow
}
$null = [MeOSApi]::SetWindowPos($hwnd, [MeOSApi]::HWND_NOTOPMOST, 0, 0, 0, 0,
        ([MeOSApi]::SWP_NOMOVE -bor [MeOSApi]::SWP_NOSIZE))
foreach ($h in $hidden) { $null = [MeOSApi]::ShowWindow($h, 9) }   # SW_RESTORE
Write-Host ("  已恢复 {0} 个窗口" -f $hidden.Count)

Write-Host ''
Write-Host '---- vmware.log 判定 ----'
if (Test-Path -LiteralPath $Log) {
    $screen = Select-String -Path $Log -Pattern 'SVGA enabling SVGA|Screen type changed|SWBScreen: Screen \d+ Defined'
    if ($screen) { $screen | ForEach-Object { '  ' + $_.Line } }
    $panic = Select-String -Path $Log -Pattern 'PANIC|unrecoverable error'
    Write-Host ''
    if ($panic) {
        Write-Host '  结果：虚拟机 PANIC 了' -ForegroundColor Red
        $panic | Select-Object -First 3 | ForEach-Object { '  ' + $_.Line }
    } else {
        Write-Host '  结果：没有 PANIC，虚拟机活着' -ForegroundColor Green
    }
} else {
    Write-Host '  没有日志文件' -ForegroundColor Yellow
}

if (-not $Keep) {
    Write-Host ''
    Write-Host '收尾：关掉虚拟机 ...'
    $null = & $Vmrun -T ws stop $Vmx hard
}