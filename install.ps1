# SysWidget one-step installer.
# Run from any PowerShell:
#   irm https://raw.githubusercontent.com/el1zabethcodes/SysWidget/main/install.ps1 | iex
# Uninstall:
#   irm https://raw.githubusercontent.com/el1zabethcodes/SysWidget/main/install.ps1 | iex   # with -Uninstall
param([switch]$Uninstall)

$ErrorActionPreference = 'Stop'
$dest = "$env:LOCALAPPDATA\Programs\SysWidget"
$repo = 'el1zabethcodes/SysWidget'

function Uninstall-Widget {
  $exe = "$dest\SysWidget.exe"
  if (Test-Path $exe) { & $exe --uninstall 2>$null | Out-Null }
  Get-Process SysWidget -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
  Remove-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name 'SysWidget' -ErrorAction SilentlyContinue
  Remove-Item $dest -Recurse -Force -ErrorAction SilentlyContinue
  Write-Host 'SysWidget удалён (exe, автозапуск, конфиг остался в %APPDATA%\SysWidget).' -ForegroundColor Green
  exit 0
}

if ($Uninstall) { Uninstall-Widget }

New-Item -ItemType Directory -Path $dest -Force | Out-Null
Write-Host "Скачиваю SysWidget.exe..." -ForegroundColor Cyan
$asset = $null; $tag = $null
try {
  $rel = Invoke-RestMethod "https://api.github.com/repos/$repo/releases/latest"
  $asset = $rel.assets | Where-Object { $_.name -eq 'SysWidget.exe' } | Select-Object -First 1
  $tag = $rel.tag_name
} catch {}
if ($asset) {
  Invoke-WebRequest $asset.browser_download_url -OutFile "$dest\SysWidget.exe"
} else {
  # no release yet: take the exe committed in the repo (main branch)
  Invoke-WebRequest "https://raw.githubusercontent.com/$repo/main/bin/SysWidget.exe" -OutFile "$dest\SysWidget.exe"
  $tag = 'main'
}
Write-Host "Установлено: $dest\SysWidget.exe ($tag)" -ForegroundColor Green

& "$dest\SysWidget.exe" --install
Start-Sleep -Seconds 2
$st = & "$dest\SysWidget.exe" --status 2>$null
if ($LASTEXITCODE -eq 0) {
  Write-Host "Готово! Виджет уже работает — смотрите левый угол панели задач." -ForegroundColor Green
} else {
  Write-Host "Виджет установлен, запустите вручную: $dest\SysWidget.exe" -ForegroundColor Yellow
}
