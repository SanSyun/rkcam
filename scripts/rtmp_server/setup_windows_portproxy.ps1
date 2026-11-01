# 在 Windows 上以管理员身份运行：
#   powershell -ExecutionPolicy Bypass -File scripts\rtmp_server\setup_windows_portproxy.ps1
# 作用：Windows 0.0.0.0:1935 -> WSL 内 MediaMTX 1935，并放行局域网防火墙。
param(
    [string]$Distro = "Ubuntu-20.04",
    [int]$Port = 1935,
    [string]$RuleName = "RK3568 RTMP 1935"
)

$ErrorActionPreference = "Stop"

function Assert-Admin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        Write-Host "请以管理员身份运行此脚本。" -ForegroundColor Red
        Write-Host "右键 PowerShell -> 以管理员身份运行，然后执行：" -ForegroundColor Yellow
        Write-Host "  powershell -ExecutionPolicy Bypass -File `"$PSCommandPath`""
        exit 1
    }
}

Assert-Admin

# 1. 取 WSL IP（WSL 重启后会变化，所以每次运行都重新取）
$wslOut = cmd.exe /c "wsl.exe -d $Distro -- hostname -I 2>nul"
$wslIp = ($wslOut -split "\s+") | Where-Object { $_ -match "^\d{1,3}(\.\d{1,3}){3}$" } | Select-Object -First 1
if (-not $wslIp) {
    throw "无法获取 WSL IP，请确认发行版名 $Distro（wsl -l -v）"
}

# 2. 取 Windows 局域网 IP（默认路由所在网卡）
$route = Get-NetRoute -DestinationPrefix "0.0.0.0/0" -ErrorAction SilentlyContinue |
         Sort-Object RouteMetric |
         Select-Object -First 1
$lanIp = $null
if ($route) {
    $lanIp = (Get-NetIPAddress -InterfaceIndex $route.InterfaceIndex -AddressFamily IPv4 -AddressState Preferred -ErrorAction SilentlyContinue |
              Where-Object { $_.IPAddress -notlike "169.254.*" } |
              Select-Object -First 1).IPAddress
}
if (-not $lanIp) {
    $lanIp = "<请用 ipconfig 查看 Windows 局域网 IP>"
}

# 3. 端口转发依赖 IP Helper 服务
$svc = Get-Service iphlpsvc -ErrorAction SilentlyContinue
if ($svc -and $svc.Status -ne "Running") {
    Start-Service iphlpsvc
}

# 4. 如果 1935 已被其他程序占用，先提示
$listeners = Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue
if ($listeners) {
    $names = @()
    foreach ($l in $listeners) {
        $proc = Get-Process -Id $l.OwningProcess -ErrorAction SilentlyContinue
        if ($proc) { $names += $proc.ProcessName }
    }
    $names = $names | Select-Object -Unique
    Write-Host "提示：端口 $Port 当前已有监听进程: $($names -join ', ')" -ForegroundColor Yellow
}

# 5. 建立端口转发
cmd.exe /c "netsh interface portproxy delete v4tov4 listenport=$Port listenaddress=0.0.0.0 2>nul" | Out-Null
$addOut = cmd.exe /c "netsh interface portproxy add v4tov4 listenport=$Port listenaddress=0.0.0.0 connectport=$Port connectaddress=$wslIp 2>&1"
if ($LASTEXITCODE -ne 0) {
    throw "netsh 端口转发添加失败：$addOut"
}

# 6. 防火墙只放行局域网来源
$rule = Get-NetFirewallRule -DisplayName $RuleName -ErrorAction SilentlyContinue
if (-not $rule) {
    New-NetFirewallRule -DisplayName $RuleName `
        -Direction Inbound -Action Allow -Protocol TCP -LocalPort $Port `
        -RemoteAddress LocalSubnet -Profile Any | Out-Null
}

Write-Host ""
Write-Host "端口转发已建立：" -ForegroundColor Green
Write-Host "  Windows ${lanIp}:${Port}  ->  WSL ${wslIp}:${Port}"
Write-Host ""
Write-Host "板端推流：" -ForegroundColor Green
Write-Host "  ./rkCam -o rtmp://${lanIp}:${Port}/live/imx415"
Write-Host ""
Write-Host "WSL 预览：" -ForegroundColor Green
Write-Host "  ./scripts/rtmp_preview/run_preview.sh rtmp://127.0.0.1:${Port}/live/imx415"
Write-Host ""
Write-Host "注意：WSL 重启后 IP 会变，需要重新运行本脚本。" -ForegroundColor Yellow