<#
    MeOS · LLM 转发代理：一键启动 + 体检

    背景：内核里的 llm 命令只会说明文 HTTP，它把请求发到宿主机
    192.168.220.1:8080，由这里的 llm_proxy.py 代它走 HTTPS 去调 DeepSeek。
    代理没在跑，MeOS 屏幕上就是 llm: cannot connect to proxy。

    用法：
        .\tools\start-llm-proxy.ps1                 没在跑就拉起来，在跑就体检
        .\tools\start-llm-proxy.ps1 -Check          只看状态，什么也不改
        .\tools\start-llm-proxy.ps1 -Model deepseek-reasoner

    体检三件事：端口有没有人听、探活接口答不答、防火墙放没放行。
    日志落在 build\llm_proxy.out / build\llm_proxy.err。
#>
param(
    [switch]$Check,
    [int]$Port = 8080,
    [string]$Model = "deepseek-chat",
    [string]$HostAddr = ""
)

$ErrorActionPreference = 'Continue'

$Root     = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $Root 'build'
$ProxyPy  = Join-Path $BuildDir 'llm_proxy.py'
$OutLog   = Join-Path $BuildDir 'llm_proxy.out'
$ErrLog   = Join-Path $BuildDir 'llm_proxy.err'

function Write-Step($text) { Write-Host $text -ForegroundColor Cyan }
function Write-Ok($text)   { Write-Host "  OK   $text" -ForegroundColor Green }
function Write-Bad($text)  { Write-Host "  注意 $text" -ForegroundColor Yellow }

Write-Step '[1/4] 找代理脚本 ...'
if (-not (Test-Path -LiteralPath $ProxyPy)) {
    $inTree = Join-Path $Root 'tools\llm_proxy.py'
    if (Test-Path -LiteralPath $inTree) {
        Copy-Item -LiteralPath $inTree -Destination $ProxyPy -Force
        Write-Ok '用了 tools\llm_proxy.py'
    } else {
        Write-Host '  tools\ 里没有，从 llm-inside-apikey 分支导出一份 ...'
        $text = & git -C $Root show llm-inside-apikey:tools/llm_proxy.py 2>$null
        if ($LASTEXITCODE -ne 0 -or -not $text) {
            throw "找不到 llm_proxy.py，也导不出来。先在仓库里执行： git fetch --all"
        }
        $utf8 = New-Object System.Text.UTF8Encoding($false)
        [IO.File]::WriteAllText($ProxyPy, ($text -join "`n"), $utf8)
        Write-Ok '从 llm-inside-apikey 分支导出成功'
    }
} else {
    Write-Ok 'build\llm_proxy.py 已在'
}

if (-not $HostAddr) {
    $HostAddr = '192.168.220.1'
    $ip = Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
          Where-Object { $_.InterfaceAlias -like '*VMnet8*' } |
          Select-Object -First 1
    if ($ip) { $HostAddr = $ip.IPAddress }
}

function Test-Proxy([string]$addr) {
    try {
        $r = Invoke-WebRequest -Uri ("http://{0}:{1}/" -f $addr, $Port) -TimeoutSec 5 -UseBasicParsing
        return ($r.Content -match 'meos-llm-proxy')
    } catch { return $false }
}

Write-Step ("[2/4] 检查 {0}:{1} ..." -f '0.0.0.0', $Port)
$listener = Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue |
            Select-Object -First 1
if ($listener) {
    Write-Ok ("已经有进程在听（PID {0}）" -f $listener.OwningProcess)
} else {
    Write-Host '  端口上没人，正在启动代理 ...'
    if ($Check) {
        Write-Bad '代理没在跑。去掉 -Check 再执行一次就能拉起来。'
        return
    }
    $python = (Get-Command python -ErrorAction SilentlyContinue).Source
    if (-not $python) { $python = 'D:\python\python.exe' }
    if (-not (Test-Path -LiteralPath $python)) { throw '找不到 python，先装 Python 3 并加进 PATH。' }

    Start-Process -FilePath $python -ArgumentList @($ProxyPy, '--port', $Port, '--model', $Model) -WorkingDirectory $Root -WindowStyle Hidden -RedirectStandardOutput $OutLog -RedirectStandardError $ErrLog | Out-Null

    for ($i = 0; $i -lt 20; $i++) {
        Start-Sleep -Milliseconds 500
        $listener = Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue |
                    Select-Object -First 1
        if ($listener) { break }
    }
    if ($listener) {
        Write-Ok ("代理起来了（PID {0}）" -f $listener.OwningProcess)
    } else {
        Write-Bad '代理没起来，看看 build\llm_proxy.err 里的报错。'
        if (Test-Path -LiteralPath $ErrLog) { Get-Content -LiteralPath $ErrLog -Tail 12 }
        return
    }
}

Write-Step '[3/4] 探活 ...'
if (Test-Proxy '127.0.0.1') {
    Write-Ok 'http://127.0.0.1 探活通过'
} else {
    Write-Bad '本机探活没通过，端口上的东西可能不是 MeOS 的代理。'
    if (Test-Path -LiteralPath $ErrLog) { Get-Content -LiteralPath $ErrLog -Tail 12 }
}
if (Test-Proxy $HostAddr) {
    Write-Ok ("http://{0} 探活通过（内核里的 llm_proxy_addr 就是它）" -f $HostAddr)
} else {
    Write-Bad ("http://{0} 探活没通过，可能绑在了别的地址上。" -f $HostAddr)
}
if ($HostAddr -ne '192.168.220.1') {
    Write-Bad ("VMnet8 地址是 {0}，但内核里 llm_proxy_addr 写死 192.168.220.1；" -f $HostAddr)
    Write-Bad '对不上的话，改 src/kernel/llm.inc 结尾那行再重新构建。'
}

Write-Step '[4/4] 检查防火墙入站规则 ...'
$rule = Get-NetFirewallRule -ErrorAction SilentlyContinue |
        Where-Object { $_.Enabled -and $_.Direction -eq 'Inbound' -and $_.DisplayName -match 'MeOS LLM' } |
        Select-Object -First 1
if ($rule) {
    Write-Ok ("放行规则在：{0}" -f $rule.DisplayName)
} else {
    Write-Bad '没有 MeOS 的入站放行规则。用管理员权限的 PowerShell 执行：'
    Write-Host ('        New-NetFirewallRule -DisplayName "MeOS LLM proxy {0}" -Direction Inbound -Action Allow -Protocol TCP -LocalPort {0}' -f $Port)
    Write-Bad '装了火绒 / 360 之类的话，也要允许 python.exe 监听这个端口。'
}

Write-Host ''
if ($listener) {
    Write-Host ("一句话：代理在跑，MeOS 里的地址 {0}:{1}。" -f $HostAddr, $Port) -ForegroundColor White
} else {
    Write-Host ("一句话：代理没在跑，MeOS 里的地址 {0}:{1}。" -f $HostAddr, $Port) -ForegroundColor White
}
Write-Host '然后去虚拟机里敲一次： llm hello' -ForegroundColor White
