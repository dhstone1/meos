<#
    MeOS 一键构建脚本（Windows / PowerShell）

    产物全部落在 build\ 目录：
        font.inc     由系统字体渲染出的 32x32 点阵字模（自动生成）
        boot.bin     引导扇区（恰好 512 字节）
        payload.bin  内核载荷（从物理地址 0x8000 开始装入）
        meos.img     1.44MB 软盘镜像（BIOS 可直接引导）
        meos.iso     El Torito 可引导光盘镜像（VMware 挂它启动）
        meos.vmx     VMware 虚拟机配置

    用法：
        .\build.ps1            构建
        .\build.ps1 -Clean     先清空 build\ 再构建
#>
param(
    [switch]$Clean,
    [string]$Message = "Hi，我是meos，很高兴来到这个世界~"
)

$ErrorActionPreference = 'Stop'

# 让子进程（python）也用 UTF-8 输出，避免中文在控制台变成乱码
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$env:PYTHONIOENCODING = 'utf-8'

$Root      = $PSScriptRoot
$BuildDir  = Join-Path $Root 'build'
$IsoDir    = Join-Path $BuildDir 'iso'
$KernelDir = Join-Path $Root 'src\kernel'
$KernelAsm = Join-Path $KernelDir 'kernel.asm'
$FontInc   = Join-Path $KernelDir 'font.inc'

function Find-Tool {
    param([string]$Name, [string[]]$Candidates)
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    foreach ($candidate in $Candidates) {
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    throw "找不到工具 $Name，请安装后重试（或把它加进 build.ps1 的候选路径）。"
}

$Nasm    = Find-Tool 'nasm'    @("$env:LOCALAPPDATA\bin\NASM\nasm.exe", 'C:\Program Files\NASM\nasm.exe')
$Python  = Find-Tool 'python'  @('D:\python\python.exe')
$Mkisofs = Find-Tool 'mkisofs' @('D:\Program Files\mkisofs.exe',
                                 'C:\Program Files (x86)\VMware\VMware Workstation\mkisofs.exe')

Write-Host "NASM    : $Nasm"
Write-Host "Python  : $Python"
Write-Host "mkisofs : $Mkisofs"
Write-Host ''

if ($Clean -and (Test-Path -LiteralPath $BuildDir)) {
    Remove-Item -LiteralPath $BuildDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $BuildDir, $IsoDir | Out-Null

# ---- 1. 生成中文字模 -------------------------------------------------------
Write-Host ('[1/5] 渲染点阵字模（32x32）：' + $Message) -ForegroundColor Cyan
& $Python (Join-Path $Root 'tools\genfont.py') $FontInc $Message 32
if ($LASTEXITCODE -ne 0) { throw '字模生成失败' }

# ---- 2. 引导扇区 -----------------------------------------------------------
Write-Host '[2/5] 汇编引导扇区 ...' -ForegroundColor Cyan
& $Nasm -f bin (Join-Path $Root 'src\boot\boot.asm') -o (Join-Path $BuildDir 'boot.bin')
if ($LASTEXITCODE -ne 0) { throw '引导扇区汇编失败' }

# ---- 3. 内核载荷 -----------------------------------------------------------
Write-Host '[3/5] 汇编内核载荷 ...' -ForegroundColor Cyan
$PayloadPath = Join-Path $BuildDir 'payload.bin'
& $Nasm -f bin -I $KernelDir $KernelAsm -o $PayloadPath -l (Join-Path $BuildDir 'kernel.lst')
if ($LASTEXITCODE -ne 0) { throw '内核载荷汇编失败' }

# ---- 4. 软盘镜像 + 引导 ISO -------------------------------------------------
Write-Host '[4/5] 生成软盘镜像与 ISO ...' -ForegroundColor Cyan
$ImgPath = Join-Path $BuildDir 'meos.img'
& $Python (Join-Path $Root 'tools\mkfloppy.py') (Join-Path $BuildDir 'boot.bin') $PayloadPath $ImgPath
if ($LASTEXITCODE -ne 0) { throw '生成软盘镜像失败' }

Copy-Item -LiteralPath $ImgPath -Destination (Join-Path $IsoDir 'boot.img') -Force
$IsoPath = Join-Path $BuildDir 'meos.iso'
& $Mkisofs -quiet -o $IsoPath -V 'MEOS' -b 'boot.img' $IsoDir
if ($LASTEXITCODE -ne 0) { throw '生成 ISO 失败' }

# ---- 5. 虚拟机配置 ---------------------------------------------------------
Write-Host '[5/5] 准备 VMware 虚拟机配置 ...' -ForegroundColor Cyan
Copy-Item -LiteralPath (Join-Path $Root 'vm\meos.vmx') -Destination (Join-Path $BuildDir 'meos.vmx') -Force

Write-Host ''
Write-Host '构建完成：' -ForegroundColor Green
Get-ChildItem -LiteralPath $BuildDir -File | Sort-Object Name | ForEach-Object {
    Write-Host ('  {0,-14} {1,9} 字节' -f $_.Name, $_.Length)
}