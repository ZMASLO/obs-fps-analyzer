# Buduje plugin (Release) i instaluje go w retail OBS Studio.
# Uruchom:  powershell -ExecutionPolicy Bypass -File .\build-and-install.ps1

$ErrorActionPreference = "Stop"
$repo = $PSScriptRoot
# SDK libobs 33.0.1 (cmake --install --component Development) + obs-deps pobrane przez CMake OBS-a
$obsPrefix = "C:/Soruces/obs-sdk;C:/Soruces/obs-sdk-build/obs-src/.deps/obs-deps-2026-10-23-x64"

# CMake z wingeta nie zawsze jest w PATH (alias pojawia sie dopiero w nowej sesji)
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    $wingetCmake = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter cmake.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $wingetCmake) { Write-Host "Nie znaleziono cmake. Zainstaluj: winget install Kitware.CMake" -ForegroundColor Red; exit 1 }
    Set-Alias cmake $wingetCmake.FullName
}
$dll = "$repo\build\plugins\fps-analyzer\Release\fps-analyzer.dll"
$dest = "C:\Program Files\obs-studio\obs-plugins\64bit"

Write-Host "=== Konfiguracja CMake ===" -ForegroundColor Cyan
cmake -S $repo -B "$repo\build" -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="$obsPrefix"
if ($LASTEXITCODE -ne 0) { Write-Host "Konfiguracja nie powiodla sie." -ForegroundColor Red; exit 1 }

Write-Host "=== Budowanie (Release) ===" -ForegroundColor Cyan
cmake --build "$repo\build" --config Release
if ($LASTEXITCODE -ne 0) { Write-Host "Build nie powiodl sie." -ForegroundColor Red; exit 1 }

if (-not (Test-Path $dll)) { Write-Host "Nie znaleziono $dll" -ForegroundColor Red; exit 1 }

# OBS blokuje DLL — musi byc zamkniety przed kopiowaniem
while (Get-Process obs64 -ErrorAction SilentlyContinue) {
    Write-Host "OBS jest uruchomiony. Zamknij OBS, aby kontynuowac instalacje..." -ForegroundColor Yellow
    Start-Sleep -Seconds 2
}

Write-Host "=== Instalacja do $dest ===" -ForegroundColor Cyan
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if ($isAdmin) {
    Copy-Item $dll $dest -Force
} else {
    # Kopiowanie do Program Files wymaga uprawnien administratora — elevacja tylko dla kopiowania
    Start-Process powershell -Verb RunAs -Wait -ArgumentList "-NoProfile", "-Command", "Copy-Item '$dll' '$dest' -Force"
}

if ((Get-Item "$dest\fps-analyzer.dll").LastWriteTime -ge (Get-Item $dll).LastWriteTime) {
    Write-Host "OK: zainstalowano $dest\fps-analyzer.dll" -ForegroundColor Green
} else {
    Write-Host "Instalacja nie powiodla sie (plik docelowy jest stary)." -ForegroundColor Red
    exit 1
}
