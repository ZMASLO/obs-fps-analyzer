# Pomiar wydajnosci algorytmu (fps-bench) i porownanie z baza z TEJ maszyny.
#
#   powershell -ExecutionPolicy Bypass -File .\run-perf.ps1
#   powershell -ExecutionPolicy Bypass -File .\run-perf.ps1 -UpdateBaseline
#
# Kazdy przypadek to czas na wywolanie (najszybsza z kilku rund i przebiegow) tego, co plugin robi
# na klatke (fps_core_feed dla 1080p/1440p/4K, kazdy format i metoda) i na
# render overlayu (wykresy w czasie). Wynik wolniejszy od bazy o wiecej niz
# -Threshold (domyslnie 50%) to REGRES i konczy skrypt kodem 1.
#
# Prog jest celowo szeroki: na tym komputerze caly przebieg potrafil wyjsc 2x
# wolniej niz nastepny (zegar CPU, temperatura, ruch pamieci przy 4K), nawet
# przy zamknietym OBS. 50% lapie prawdziwe regresje (histogram roznic dodal
# 3,5x przy 4K Full frame), a nie szum. Przy REGRESIE uruchom jeszcze raz.
#
# Baza (plugins\fps-analyzer\tests\perf-baseline.json) jest per maszyna: zapisuje
# CPU i date. Na innym komputerze porownanie nic nie znaczy - skrypt ostrzega.
# -UpdateBaseline zapisuje nowa baze: rob to swiadomie (jak goldeny), np. po
# zamierzonej optymalizacji, i commituj razem ze zmiana.

param(
    [switch]$UpdateBaseline,
    [switch]$Force,
    [int]$Passes = 3,
    [double]$Threshold = 0.50,
    [string]$ObsPrefix = "C:/Soruces/obs-sdk;C:/Soruces/obs-sdk-build/obs-src/.deps/obs-deps-2026-10-23-x64"
)

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    $wingetCmake = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter cmake.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $wingetCmake) { Write-Host "Nie znaleziono cmake. Zainstaluj: winget install Kitware.CMake" -ForegroundColor Red; exit 1 }
    Set-Alias cmake $wingetCmake.FullName
}

$ErrorActionPreference = "Stop"
$repo = $PSScriptRoot
$build = "$repo\build"
$baseline = "$repo\plugins\fps-analyzer\tests\perf-baseline.json"
$current = "$build\perf-current.json"

Write-Host "=== Budowanie fps-bench (Release) ===" -ForegroundColor Cyan
if (-not (Test-Path "$build\CMakeCache.txt")) {
    cmake -S $repo -B $build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="$ObsPrefix"
    if ($LASTEXITCODE -ne 0) { exit 1 }
}
cmake --build $build --config Release --target fps-bench
if ($LASTEXITCODE -ne 0) { Write-Host "Budowanie nie powiodlo sie." -ForegroundColor Red; exit 1 }

if ((Get-Process obs64 -ErrorAction SilentlyContinue) -and -not $Force) {
    # OBS z filtrem sam analizuje klatki i zajmuje rdzen: wyniki potrafia sie
    # roznic 2x miedzy przebiegami, a baza zapisana pod obciazeniem jest bezwartosciowa
    Write-Host "OBS jest uruchomiony - zamknij go przed pomiarem (albo -Force, wyniki beda zaszumione)." -ForegroundColor Yellow
    exit 2
}
Write-Host "=== Pomiar: $Passes przebiegi po ok. 35 s ===" -ForegroundColor Cyan
# Kazdy przypadek: najszybszy z przebiegow (zaklocenia moga tylko spowolnic).
# "normalised" (czas w petlach referencyjnych fps-bench) zostaje w JSON
# informacyjnie - nie okazal sie stabilniejszy od samych mikrosekund.
$runs = @()
for ($k = 1; $k -le $Passes; $k++) {
    $out = "$build\perf-pass$k.json"
    & "$build\plugins\fps-analyzer\Release\fps-bench.exe" --json $out | Out-Null
    if ($LASTEXITCODE -ne 0) { exit 1 }
    $runs += (Get-Content $out -Raw | ConvertFrom-Json)
}
$merged = [ordered]@{ cpu = $runs[0].cpu; date = $runs[0].date; mode = $runs[0].mode; passes = $Passes;
                      results = [ordered]@{}; normalised = [ordered]@{} }
foreach ($p in $runs[0].results.PSObject.Properties) {
    $merged.results[$p.Name] = ($runs | ForEach-Object { $_.results.($p.Name) } | Measure-Object -Minimum).Minimum
    $merged.normalised[$p.Name] = ($runs | ForEach-Object { $_.normalised.($p.Name) } | Measure-Object -Minimum).Minimum
}
$merged | ConvertTo-Json -Depth 4 | Set-Content -Encoding ascii $current

if ($UpdateBaseline) {
    Copy-Item $current $baseline -Force
    Write-Host "Zapisano baze: $baseline" -ForegroundColor Green
    exit 0
}
if (-not (Test-Path $baseline)) {
    Write-Host "Brak bazy - uruchom z -UpdateBaseline, zeby ja zapisac." -ForegroundColor Yellow
    exit 0
}

$cur = Get-Content $current -Raw | ConvertFrom-Json
$base = Get-Content $baseline -Raw | ConvertFrom-Json
if ($cur.cpu -ne $base.cpu) {
    Write-Host ("UWAGA: baza z innego CPU ({0}), porownanie orientacyjne." -f $base.cpu) -ForegroundColor Yellow
}
Write-Host ("Baza: {0}" -f $base.date)

$regress = 0
"{0,-46} {1,10} {2,10} {3,8}" -f "case", "base us", "now us", "zmiana"
foreach ($p in $cur.results.PSObject.Properties) {
    $b = $base.results.($p.Name)
    $bus = $b; $cus = $p.Value
    if ($null -eq $b) { "{0,-46} {1,10} {2,10:N2} {3,8}" -f $p.Name, "-", $cus, "nowy"; continue }
    $ratio = if ($b -gt 0) { $p.Value / $b } else { 1.0 }
    $tag = ""; $colour = "Gray"
    # pomiary ponizej 1 us sa w szumie zegara - tylko informacyjnie
    if ($bus -ge 1.0 -and $ratio -gt 1 + $Threshold) { $tag = "REGRES"; $colour = "Red"; $regress++ }
    elseif ($bus -ge 1.0 -and $ratio -lt 1 - $Threshold) { $tag = "szybciej"; $colour = "Green" }
    Write-Host ("{0,-46} {1,10:N2} {2,10:N2} {3,7:+0%;-0%;0%} {4}" -f $p.Name, $bus, $cus, ($ratio - 1), $tag) -ForegroundColor $colour
}
if ($regress -gt 0) {
    Write-Host "REGRESY WYDAJNOSCI: $regress" -ForegroundColor Red
    exit 1
}
Write-Host "Bez regresow (prog $([int]($Threshold * 100))%)." -ForegroundColor Green
