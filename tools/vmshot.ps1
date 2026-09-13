<#
    MeOS · 虚拟机验证脚本

    重启 VMware 里的 MeOS（图形界面模式），等它引导完成，
    把虚拟机窗口截图存成 PNG，用来确认「屏幕上到底显示了什么」。

    用法：
        .\tools\vmshot.ps1                 默认等 10 秒后截图
        .\tools\vmshot.ps1 -Wait 15        多等一会儿再截
#>
param(
    [int]$Wait = 10,
    [string]$Out = ""
)

# vmrun 会往 stderr 打无害警告，这里不能用 Stop，否则会被当成错误中断
$ErrorActionPreference = 'Continue'

$Root     = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $Root 'build'
$Vmx      = Join-Path $BuildDir 'meos.vmx'
$Vmrun    = 'D:\Program Files\vmrun.exe'
if (-not $Out) { $Out = Join-Path $BuildDir 'screen.png' }

Write-Host '[1/4] 关闭旧实例 ...'
$null = & $Vmrun -T ws stop $Vmx hard
Start-Sleep -Seconds 2

Write-Host '[2/4] 以图形界面启动虚拟机（不阻塞） ...'
Start-Process -FilePath $Vmrun -ArgumentList @('-T', 'ws', 'start', $Vmx, 'gui') -WindowStyle Hidden

Write-Host '[3/4] 等待虚拟机窗口与引导 ...'
$proc = $null
for ($i = 0; $i -lt 40; $i++) {
    Start-Sleep -Seconds 1
    $proc = Get-Process -Name 'vmware' -ErrorAction SilentlyContinue |
            Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if ($proc) { break }
}
if (-not $proc) { throw '没有等到 VMware 窗口，虚拟机可能没起来。' }
Start-Sleep -Seconds $Wait

Write-Host '[4/4] 截图 ...'
Add-Type -AssemblyName System.Drawing
$cs = @(
    'using System;',
    'using System.Runtime.InteropServices;',
    'public class WinApi {',
    '    [StructLayout(LayoutKind.Sequential)]',
    '    public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }',
    '    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);',
    '    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);',
    '    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT r);',
    '}'
) -join [Environment]::NewLine
Add-Type -TypeDefinition $cs

$null = [WinApi]::ShowWindow($proc.MainWindowHandle, 3)     # 3 = 最大化
$null = [WinApi]::SetForegroundWindow($proc.MainWindowHandle)
Start-Sleep -Milliseconds 1500

$rect = New-Object WinApi+RECT
$null = [WinApi]::GetWindowRect($proc.MainWindowHandle, [ref]$rect)
$width  = $rect.Right - $rect.Left
$height = $rect.Bottom - $rect.Top
if ($width -le 0 -or $height -le 0) { throw '窗口尺寸异常，截图失败。' }

$bmp = New-Object System.Drawing.Bitmap $width, $height
$gfx = [System.Drawing.Graphics]::FromImage($bmp)
$gfx.CopyFromScreen($rect.Left, $rect.Top, 0, 0, (New-Object System.Drawing.Size $width, $height))
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$gfx.Dispose()
$bmp.Dispose()

Write-Host ('截图已保存：{0}  ({1}x{2})' -f $Out, $width, $height) -ForegroundColor Green