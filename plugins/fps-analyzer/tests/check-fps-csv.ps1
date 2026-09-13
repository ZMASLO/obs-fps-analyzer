# Sprawdza CSV zapisany przez wtyczke podczas testu E2E w OBS.
#
#   powershell -ExecutionPolicy Bypass -File .\check-fps-csv.ps1 -Csv fps.csv -ExpectFps 60
#   ... -Skip 2 -Tol 1 -IgnoreZeros
#
# Format pliku: timestamp,fps,frametime_ms (po jednym wierszu na publikacje).
# -Skip odrzuca pierwsze N sekund, bo analiza startuje na zimno i przez okolo
# sekunde dochodzi do wlasciwej wartosci. -IgnoreZeros pomija ogon z zerami,
# ktory pojawia sie po zatrzymaniu odtwarzania (reset po 2 s bez nowych klatek).
#
# Uzycie razem z klipami z gen-fixtures.ps1 -Local: Media Source w OBS odtwarza
# klip o znanej kadencji, wiec -ExpectFps jest niezalezna prawda.

param(
    [Parameter(Mandatory = $true)][string]$Csv,
    [int]$ExpectFps = 0,
    [double]$Skip = 2.0,
    [double]$Tol = 1.0,
    [switch]$IgnoreZeros
)

$ErrorActionPreference = "Stop"
if (-not (Test-Path $Csv)) { Write-Host "Nie znaleziono $Csv" -ForegroundColor Red; exit 2 }

$rows = @()
foreach ($line in Get-Content $Csv) {
    $f = $line.Split(',')
    if ($f.Count -lt 3) { continue }
    $t = 0L; $fps = 0
    if (-not [long]::TryParse($f[0].Trim(), [ref]$t)) { continue }
    if (-not [int]::TryParse($f[1].Trim(), [ref]$fps)) { continue }
    $ft = 0.0
    [void][double]::TryParse($f[2].Trim(), [System.Globalization.NumberStyles]::Float,
                             [System.Globalization.CultureInfo]::InvariantCulture, [ref]$ft)
    $rows += [pscustomobject]@{ t = $t; fps = $fps; ft = $ft }
}

if ($rows.Count -eq 0) { Write-Host "Brak danych w $Csv" -ForegroundColor Red; exit 2 }

$t0 = ($rows | Measure-Object t -Minimum).Minimum
$sel = $rows | Where-Object { ($_.t - $t0) -ge $Skip }
if ($IgnoreZeros) { $sel = $sel | Where-Object { $_.fps -gt 0 } }

Write-Host "Plik:      $Csv"
Write-Host "Wierszy:   $($rows.Count) lacznie, $($sel.Count) po odrzuceniu pierwszych $Skip s$(if ($IgnoreZeros) { ' i zer' })"
if ($sel.Count -eq 0) { Write-Host "Nic nie zostalo do sprawdzenia." -ForegroundColor Red; exit 2 }

$stats = $sel | Measure-Object fps -Minimum -Maximum -Average
$sorted = ($sel | Sort-Object fps).fps
$median = $sorted[[int]($sorted.Count / 2)]
$mode = ($sel | Group-Object fps | Sort-Object Count -Descending | Select-Object -First 1)

Write-Host ("FPS:       min {0}, max {1}, srednia {2:F2}, mediana {3}, dominanta {4} ({5} z {6} wierszy)" -f `
    $stats.Minimum, $stats.Maximum, $stats.Average, $median, $mode.Name, $mode.Count, $sel.Count)
$ftStats = $sel | Measure-Object ft -Minimum -Maximum -Average
Write-Host ("Frametime: min {0:F2} ms, max {1:F2} ms, srednia {2:F2} ms" -f `
    $ftStats.Minimum, $ftStats.Maximum, $ftStats.Average)

$zeros = ($rows | Where-Object { $_.fps -eq 0 }).Count
if ($zeros -gt 0) { Write-Host "Wierszy z zerem: $zeros (reset po 2 s bez nowej klatki)" -ForegroundColor Gray }

if ($ExpectFps -le 0) { exit 0 }

$bad = $sel | Where-Object { [Math]::Abs($_.fps - $ExpectFps) -gt $Tol }
if ($bad.Count -eq 0) {
    Write-Host "OK: kazdy wiersz miesci sie w $ExpectFps +- $Tol" -ForegroundColor Green
    exit 0
}
$worst = $bad | Sort-Object { [Math]::Abs($_.fps - $ExpectFps) } -Descending | Select-Object -First 1
Write-Host ("BLAD: {0} z {1} wierszy poza {2} +- {3}, najgorszy {4} w t+{5:F1} s" -f `
    $bad.Count, $sel.Count, $ExpectFps, $Tol, $worst.fps, ($worst.t - $t0)) -ForegroundColor Red
exit 1
