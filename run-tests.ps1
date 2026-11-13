# Buduje narzedzia testowe i uruchamia caly zestaw testow (ctest).
# Jedna komenda do przepuszczenia przed pushem.
#
#   powershell -ExecutionPolicy Bypass -File .\run-tests.ps1
#   powershell -ExecutionPolicy Bypass -File .\run-tests.ps1 -UpdateGoldens
#
# -UpdateGoldens przepisuje wzorce w plugins\fps-analyzer\tests\golden. Rob to
# tylko po SWIADOMEJ zmianie algorytmu i commituj diff wzorcow razem ze zmiana,
# zeby wplyw na odczyty FPS byl widoczny w review.

param(
    [switch]$UpdateGoldens,
    [string]$ObsPrefix = "C:/Soruces/obs-sdk;C:/Soruces/obs-sdk-build/obs-src/.deps/obs-deps-2026-10-23-x64"
)

# CMake z wingeta nie zawsze jest w PATH (alias pojawia sie dopiero w nowej sesji)
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    $wingetCmake = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter cmake.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $wingetCmake) { Write-Host "Nie znaleziono cmake. Zainstaluj: winget install Kitware.CMake" -ForegroundColor Red; exit 1 }
    Set-Alias cmake $wingetCmake.FullName
    Set-Alias ctest (Join-Path $wingetCmake.DirectoryName "ctest.exe")
}

$ErrorActionPreference = "Stop"
$repo = $PSScriptRoot
$build = "$repo\build"

Write-Host "=== Konfiguracja CMake ===" -ForegroundColor Cyan
cmake -S $repo -B $build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="$ObsPrefix"
if ($LASTEXITCODE -ne 0) { Write-Host "Konfiguracja nie powiodla sie." -ForegroundColor Red; exit 1 }

Write-Host "=== Budowanie narzedzi testowych (Release) ===" -ForegroundColor Cyan
cmake --build $build --config Release --target fps-selftest fps-cli resdet-selftest srate-selftest
if ($LASTEXITCODE -ne 0) { Write-Host "Build nie powiodl sie." -ForegroundColor Red; exit 1 }

if ($UpdateGoldens) {
    Write-Host "=== Aktualizacja wzorcow (fps-selftest --update-goldens) ===" -ForegroundColor Yellow
    & "$build\plugins\fps-analyzer\Release\fps-selftest.exe" --update-goldens
    if ($LASTEXITCODE -ne 0) { Write-Host "Aktualizacja wzorcow zglosila bledy." -ForegroundColor Red; exit 1 }
    Write-Host "Sprawdz 'git diff plugins/fps-analyzer/tests/golden' przed commitem." -ForegroundColor Yellow
}

Write-Host "=== ctest ===" -ForegroundColor Cyan
ctest --test-dir $build -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { Write-Host "TESTY NIE PRZESZLY" -ForegroundColor Red; exit 1 }
Write-Host "OK: wszystkie testy przeszly" -ForegroundColor Green
