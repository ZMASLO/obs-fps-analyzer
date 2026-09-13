# Mierzy, ile zestaw testow naprawde lapie: psuje rdzen po jednej rzeczy naraz
# i sprawdza, czy testy to zauwazaja. Mutacja, ktora przechodzi, wskazuje dziure
# w pokryciu - dokladnie tak znalazlo sie to, ze tablice wykresu nie byly nigdzie
# porownywane.
#
#   powershell -ExecutionPolicy Bypass -File .\mutation-sweep.ps1
#   ... -Build C:\Sources\obs-fps-analyzer\build -Only 3
#
# Kazda mutacja jest sprawdzana trzema warstwami osobno, bo lapia co innego:
#   core     fps-selftest --impl core: wzorce i asercje, BEZ wyroczni v0.5.0.
#            To jest warstwa, ktora zostanie po usunieciu wyroczni (faza 5).
#   diff     fps-selftest: dodatkowo porownanie rdzenia z wyrocznia i fuzz.
#   clips    fps-cli --manifest: klipy referencyjne o znanej kadencji.
#
# Skrypt przywraca plik po kazdej mutacji. Nie uruchamiaj go z niezacommitowanymi
# zmianami w fps-core.cpp.

param(
    [string]$Build = (Join-Path (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))) "build"),
    [int]$Only = 0
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
$core = Join-Path $repo "plugins\fps-analyzer\fps-core.cpp"
$bin = Join-Path $Build "plugins\fps-analyzer\Release"
$manifest = Join-Path $PSScriptRoot "cases.txt"

# Kazda mutacja to jawny slownik, zeby indeksy nie zalezaly od tego, ile linii
# ma zamiennik. `equivalent` opisuje mutacje ROWNOWAZNA, czyli taka, ktora nie
# moze zmienic zachowania - to nie jest dziura w pokryciu, tylko martwy kod.
$nl = "`r`n"
$mutations = @(
    @{ name = "prog czulosci: >= na >";              find = 'if (percent >= c->params.sensitivity)'; repl = 'if (percent > c->params.sensitivity)' },
    @{ name = "prog tearingu: >= 2 na >= 3";         find = 'return recent_tears >= 2;'; repl = 'return recent_tears >= 3;' },
    @{ name = "EMA alpha 0.15 na 0.16";              find = 'const double alpha = 0.15;'; repl = 'const double alpha = 0.16;' },
    @{ name = "dolny clamp inst_fps 10 na 9";        find = 'if (inst_fps < 10) inst_fps = 10;'; repl = 'if (inst_fps < 9) inst_fps = 9;' },
    @{ name = "gorny clamp inst_fps 120 na 121";     find = 'if (inst_fps > 120) inst_fps = 120;'; repl = 'if (inst_fps > 121) inst_fps = 121;' },
    @{ name = "prog stale 2 s na 2.5 s";             find = 'now_ns - c->last_unique_frame_time > 2000000000ULL'; repl = 'now_ns - c->last_unique_frame_time > 2500000000ULL' },
    @{ name = "cap okna 120 na 130";                 find = 'if (window > 120) window = 120;'; repl = 'if (window > 130) window = 130;' },
    @{ name = "sprzezenie okna: > 10 na > 11";       find = 'if (c->last_published_fps > 10 && c->last_published_fps < window)'; repl = 'if (c->last_published_fps > 11 && c->last_published_fps < window)' },
    @{ name = "podloga okna: >= 10 na >= 11";        find = 'if (window < 10 && c->frametime_count >= 10) window = 10;'; repl = 'if (window < 11 && c->frametime_count >= 11) window = 11;';
       equivalent = 'martwa galaz: window startuje od frametime_count, wiec przy count >= 10 jest juz >= 10, a sprzezenie podmienia go tylko na wartosc > 10' },
    @{ name = "srodkowa linia sondy h/2 na h/2+1";   find = 'int line_ys[3] = {0, (int)frame->height / 2, (int)frame->height - 1};'; repl = 'int line_ys[3] = {0, (int)frame->height / 2 + 1, (int)frame->height - 1};' },
    @{ name = "limit szerokosci tearingu > na >=";   find = 'if (roi_width > FPS_CORE_MAX_TEARING_WIDTH)'; repl = 'if (roi_width >= FPS_CORE_MAX_TEARING_WIDTH)' },
    @{ name = "offset lumy +16 na +15";              find = '>> 8) + 16;'; repl = '>> 8) + 15;' },
    @{ name = "zaokraglenie lumy +128 na +127";      find = 'b * 25 + 128) >> 8'; repl = 'b * 25 + 127) >> 8' },
    @{ name = "init EMA: <= 0 na < 0";               find = 'if (c->ema_frametime <= 0.0)'; repl = 'if (c->ema_frametime < 0.0)' },
    @{ name = "ROI ostatniej linii: h-1 na h-2";     find = 'roi_line = height - 1;'; repl = 'roi_line = height - 2;' },
    @{ name = "stale zeruje takze EMA";              find = 'c->frametime_pos = 0;' + $nl + '    }'; repl = 'c->frametime_pos = 0;' + $nl + '        c->ema_frametime = 0.0;' + $nl + '    }' },
    @{ name = "kolejnosc: tearing po agregacji";     find = 'c->tearing_detected = tearing;' + $nl + '    c->dbg.tearing_flag = tearing;'; repl = 'c->dbg.tearing_flag = tearing;' },
    @{ name = "okno per-klatka bez clampu do count"; find = 'if (window > c->frametime_count) window = c->frametime_count;' + $nl + '        if (window < 1) window = 1;'; repl = 'if (window < 1) window = 1;' }
)

function Run-Layer([string]$exe, [string[]]$argv) {
    $out = & $exe @argv 2>&1
    return @{ code = $LASTEXITCODE; text = ($out -join "`n") }
}

# Czytaj i pisz bajtami przez .NET: Set-Content -Encoding utf8 w PowerShellu 5.1
# dokleja BOM i przekodowuje znaki spoza ASCII, co po przywroceniu zostawiloby
# uszkodzony plik zrodlowy.
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
function Read-Source([string]$path) { [System.IO.File]::ReadAllText($path, $utf8NoBom) }
function Write-Source([string]$path, [string]$text) { [System.IO.File]::WriteAllText($path, $text, $utf8NoBom) }

$original = Read-Source $core
$results = @()
$i = 0

try {
    foreach ($m in $mutations) {
        $i++
        if ($Only -gt 0 -and $i -ne $Only) { continue }
        $name = $m.name; $find = $m.find; $repl = $m.repl
        $equivalent = $m.equivalent

        # Pliki maja konce linii LF albo CRLF zaleznie od checkoutu - sprobuj obu.
        $text = $original
        $found = $text.Contains($find)
        if (-not $found) {
            $lf = $find.Replace("`r`n", "`n"); $rl = $repl.Replace("`r`n", "`n")
            if ($text.Contains($lf)) { $find = $lf; $repl = $rl; $found = $true }
        }
        if (-not $found) {
            Write-Host ("{0,2}. {1,-42} POMINIETA (nie znaleziono wzorca w kodzie)" -f $i, $name) -ForegroundColor DarkYellow
            $results += [pscustomobject]@{ n = $i; name = $name; core = "-"; diff = "-"; clips = "-"; verdict = "SKIP" }
            continue
        }

        Write-Source $core $text.Replace($find, $repl)
        & cmake --build $Build --config Release --target fps-selftest fps-cli 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) {
            Write-Host ("{0,2}. {1,-42} NIE KOMPILUJE SIE" -f $i, $name) -ForegroundColor DarkYellow
            $results += [pscustomobject]@{ n = $i; name = $name; core = "-"; diff = "-"; clips = "-"; verdict = "NOBUILD" }
            continue
        }

        $rCore  = Run-Layer "$bin\fps-selftest.exe" @("--impl", "core")
        $rDiff  = Run-Layer "$bin\fps-selftest.exe" @()
        $rClips = Run-Layer "$bin\fps-cli.exe" @("--manifest", $manifest)

        $c = if ($rCore.code -ne 0) { "lapie" } else { "PRZECHODZI" }
        $d = if ($rDiff.code -ne 0) { "lapie" } else { "PRZECHODZI" }
        $l = if ($rClips.code -ne 0) { "lapie" } else { "PRZECHODZI" }
        $caught = ($rCore.code -ne 0) -or ($rDiff.code -ne 0) -or ($rClips.code -ne 0)
        if ($equivalent) {
            # Rownowazna ma NIE zostac zlapana. Gdyby nagle byla, znaczy to, ze
            # kod przestal byc martwy i opis w czwartym polu jest juz nieprawda.
            $verdict = if ($caught) { "ZMIANA" } else { "ROWNOWAZNA" }
            $colour = if ($caught) { "Magenta" } else { "DarkGray" }
        } else {
            $verdict = if ($caught) { "OK" } else { "DZIURA" }
            $colour = if (-not $caught) { "Red" } elseif ($rCore.code -eq 0) { "Yellow" } else { "Green" }
        }

        Write-Host ("{0,2}. {1,-42} core:{2,-11} diff:{3,-11} clips:{4,-11} {5}" -f $i, $name, $c, $d, $l, $verdict) -ForegroundColor $colour
        if ($equivalent) { Write-Host ("      {0}" -f $equivalent) -ForegroundColor DarkGray }
        $results += [pscustomobject]@{ n = $i; name = $name; core = $c; diff = $d; clips = $l; verdict = $verdict }
    }
} finally {
    Write-Source $core $original
    & cmake --build $Build --config Release --target fps-selftest fps-cli 2>&1 | Out-Null
}

$run = @($results | Where-Object { $_.verdict -in @("OK", "DZIURA", "ROWNOWAZNA", "ZMIANA") })
$holes = @($run | Where-Object { $_.verdict -eq "DZIURA" })
$equiv = @($run | Where-Object { $_.verdict -eq "ROWNOWAZNA" })
$changed = @($run | Where-Object { $_.verdict -eq "ZMIANA" })
$onlyRef = @($run | Where-Object { $_.verdict -eq "OK" -and $_.core -eq "PRZECHODZI" })
$real = @($run | Where-Object { $_.verdict -in @("OK", "DZIURA") })

Write-Host ""
Write-Host ("Mutacji uruchomionych: {0}, zlapanych: {1} z {2} mogacych byc zlapane, dziur: {3}, rownowaznych: {4}" -f `
    $run.Count, ($real.Count - $holes.Count), $real.Count, $holes.Count, $equiv.Count)
if ($changed.Count -gt 0) {
    Write-Host "Mutacje opisane jako rownowazne zostaly ZLAPANE - opis jest nieaktualny:" -ForegroundColor Magenta
    $changed | ForEach-Object { Write-Host "  - $($_.name)" -ForegroundColor Magenta }
}
if ($onlyRef.Count -gt 0) {
    Write-Host "Lapane WYLACZNIE dzieki wyroczni v0.5.0 (znikna w fazie 5):" -ForegroundColor Yellow
    $onlyRef | ForEach-Object { Write-Host "  - $($_.name)" -ForegroundColor Yellow }
}
if ($holes.Count -gt 0 -or $changed.Count -gt 0) {
    if ($holes.Count -gt 0) {
        Write-Host "Dziury w pokryciu:" -ForegroundColor Red
        $holes | ForEach-Object { Write-Host "  - $($_.name)" -ForegroundColor Red }
    }
    exit 1
}
Write-Host "Kazda mutacja, ktora moze byc zlapana, zostala zlapana." -ForegroundColor Green
exit 0
