# 删除 Windows 上为 WSL RTMP 建立的端口转发和防火墙规则。
# 以管理员身份运行：
#   powershell -ExecutionPolicy Bypass -File scripts\rtmp_server\remove_windows_portproxy.ps1
param(
    [int]$Port = 1935,
    [string]$RuleName = "RK3568 RTMP 1935"
)

$ErrorActionPreference = "Stop"
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "请以管理员身份运行此脚本。" -ForegroundColor Red
    exit 1
}

cmd.exe /c "netsh interface portproxy delete v4tov4 listenport=$Port listenaddress=0.0.0.0 2>nul" | Out-Null
Remove-NetFirewallRule -DisplayName $RuleName -ErrorAction SilentlyContinue

Write-Host "已删除 ${Port} 端口转发和防火墙规则 $RuleName" -ForegroundColor Green