param(
    [int]$Seconds = 300,
    [string]$Log = "build\netmon_test.log"
)

$deadline = (Get-Date).AddSeconds($Seconds)
$lines = @()
while ((Get-Date) -lt $deadline) {
    $conns = Get-NetTCPConnection -LocalPort 8080 -ErrorAction SilentlyContinue
    foreach ($c in $conns) {
        if ($c.State -eq 'Listen') { continue }
        $lines += ("[{0}] state={1} local={2}:{3} remote={4}:{5} pid={6}" -f
            (Get-Date).ToString('HH:mm:ss.fff'), $c.State, $c.LocalAddress,
            $c.LocalPort, $c.RemoteAddress, $c.RemotePort, $c.OwningProcess)
    }
    Add-Content -LiteralPath $Log -Value ($lines -join "`n")
    $lines = @()
    Start-Sleep -Milliseconds 500
}
Add-Content -LiteralPath $Log -Value ("[{0}] netmon done, window={1}s" -f
    (Get-Date).ToString('HH:mm:ss.fff'), $Seconds)
