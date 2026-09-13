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
    [string]$ObsPrefix = "C:\Sources\obs-studio\build\libobs;C:\Sources\obs-studio\build\deps\w32-pthreads"
)

$ErrorActionPreference = "Stop"
$repo = $PSScriptRoot
$build = "$repo\build"

Write-Host "=== Konfiguracja CMake ===" -ForegroundColor Cyan
cmake -S $repo -B $build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="$ObsPrefix"
if ($LASTEXITCODE -ne 0) { Write-Host "Konfiguracja nie powiodla sie." -ForegroundColor Red; exit 1 }

Write-Host "=== Budowanie narzedzi testowych (Release) ===" -ForegroundColor Cyan
cmake --build $build --config Release --target fps-selftest resdet-selftest
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
