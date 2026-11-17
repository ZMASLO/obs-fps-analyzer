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

# CMake z wingeta nie zawsze jest w PATH (alias pojawia sie dopiero w nowej sesji)
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    $wingetCmake = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter cmake.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $wingetCmake) { throw "Nie znaleziono cmake. Zainstaluj: winget install Kitware.CMake" }
    Set-Alias cmake $wingetCmake.FullName
}

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
    @{ name = "okno per-klatka bez clampu do count"; find = 'if (window > c->frametime_count) window = c->frametime_count;' + $nl + '        if (window < 1) window = 1;'; repl = 'if (window < 1) window = 1;' },
    # Detektor dudnienia toru przechwytywania (s23_capture_beat + slady z 4K X)
    @{ name = "dudnienie: bez dopasowania do standardu"; find = 'return; // not the difference of two standard clocks'; repl = '{} // snap ignored' },
    @{ name = "dudnienie: znak sygnalu - na +";      find = 'delivery_hz - 1.0 / period : 0.0;'; repl = 'delivery_hz + 1.0 / period : 0.0;' },
    @{ name = "dudnienie: wygasanie 3 okresy na 30"; find = '> 3.0 * period)'; repl = '> 30.0 * period)' },
    @{ name = "dudnienie: czas klastra = 1. zdarzenie"; find = '(uint64_t)(b.cluster_sum_s / b.cluster_events * 1e9)'; repl = '(uint64_t)0' },
    @{ name = "dudnienie: rodzaje klastrow razem";   find = 'if (b.obs[e] != obs)'; repl = 'if (false)' },
    @{ name = "dudnienie: brak zastoju";             find = 'b.cluster_stall = true;'; repl = '(void)0;' },
    @{ name = "dudnienie: duplikat bez czystego ciagu"; find = 'beat_event(c, now, false, b.clean_frames >= FPS_BEAT_CLEAN_RUN, b.run_dup);'; repl = 'beat_event(c, now, false, true, b.run_dup);' },
    @{ name = "dudnienie: pusty tick nie jest zdarzeniem"; find = 'if (frames == 0) {'; repl = 'if (frames == -5) {' },
    # Sprawdzanie sygnalu: "OK" dopiero, gdy rozjazd 59.94/60 jest wykluczony
    @{ name = "sprawdzanie: powtorka nie wstrzymuje"; find = 'held = now_s - t[i] < 2.0 * p + tol;'; repl = 'held = false;' },
    @{ name = "sprawdzanie: wstrzymanie 1P zamiast 2P"; find = 'held = now_s - t[i] < 2.0 * p + tol;'; repl = 'held = now_s - t[i] < 1.0 * p + tol;' },
    @{ name = "sprawdzanie: OK po pol okna";         find = 'if (!held && out->observed_s >= out->verify_s)'; repl = 'if (!held && out->observed_s >= 0.5 * out->verify_s)' },
    @{ name = "sprawdzanie: brak resetu po utracie sygnalu"; find = 'now - b.last_activity > FPS_BEAT_RESET_GAP_NS'; repl = 'now - b.last_activity > 10 * FPS_BEAT_RESET_GAP_NS' },
    @{ name = "sprawdzanie: uneven nigdy";           find = 'out->uneven = irregular >= FPS_BEAT_UNEVEN_CLUSTERS;'; repl = 'out->uneven = false;' },
    @{ name = "sprawdzanie: uneven liczy dudnienie";  find = 'irregular += regular ? 0 : 1;'; repl = 'irregular += 1;' },
    @{ name = "sprawdzanie: otwarty klaster pominiety"; find = 'if (b.in_cluster && !b.cluster_stall) {'; repl = 'if (false) {' },
    # Karta zmieniajaca klatki (dithering): FPS ukryty, gdy powtorki nigdy nie sa identyczne
    @{ name = "zmienione klatki: drobne do |d|<=4";     find = 'for (int i = FPS_ALTERED_MAX_DELTA_BUCKET + 1; i < FPS_CORE_DELTA_BUCKETS; i++)'; repl = 'for (int i = FPS_ALTERED_MAX_DELTA_BUCKET + 2; i < FPS_CORE_DELTA_BUCKETS; i++)' },
    @{ name = "zmienione klatki: kubelek |d|=2 jako 3-4"; find = 'if (d <= 2)'; repl = 'if (d <= 1)' },
    @{ name = "zmienione klatki: identyczna nie zeruje";  find = "        if (!c->altered)`n            c->altered_noise_run = 0;"; repl = "        if (!c->altered)`n            (void)0;" },
    @{ name = "zmienione klatki: nigdy nie zwalnia";      find = '++c->altered_exact >= FPS_ALTERED_CLEAR'; repl = '++c->altered_exact >= 1000000' },
    @{ name = "zmienione klatki: liczy tez powtorki";     find = 'if (!unique || !tiny)'; repl = 'if (!tiny)' },
    @{ name = "zmienione klatki: prog 1/3";              find = '++c->altered_noise_run >= FPS_ALTERED_TRIGGER'; repl = '++c->altered_noise_run >= FPS_ALTERED_TRIGGER / 3' },
    @{ name = "dudnienie: wylaczenie ignorowane";         find = 'if (!c || !c->params.beat_enabled)'; repl = 'if (!c)' },
    # Frame pacing: czas kazdej klatki gry ze znacznikow karty, utracone przez karte oznaczone
    @{ name = "pacing: zegar analizy zamiast znacznika";  find = 'if (c->src_ts && c->last_unique_src_ts && c->src_ts > c->last_unique_src_ts)'; repl = 'if (false)' },
    @{ name = "pacing: bez oznaczania utraconych";        find = 'if (iv * 2 > median * 3) {'; repl = 'if (false) {' },
    @{ name = "pacing: prog utraty 1.5x na 2.5x";         find = 'if (iv * 2 > median * 3) {'; repl = 'if (iv * 2 > median * 5) {' },
    @{ name = "pacing: bez przeniesienia na nastepna";    find = 'c->carry_gap = c->frame_after_gap;'; repl = 'c->carry_gap = false;' },
    @{ name = "pacing: sync bez zaokraglenia";            find = 'return k * per;'; repl = 'return ft_ms;' },
    @{ name = "reset historii: czysci tez werdykt";       find = "    c->carry_gap = false;`n    c->last_published_fps = 0;"; repl = "    c->carry_gap = false;`n    c->last_published_fps = 0;`n    altered_reset(c);" },
    @{ name = "reset historii: zostawia probki";          find = "    c->frametime_count = 0;`n    c->ema_frametime = 0.0;"; repl = "    c->ema_frametime = 0.0;" }
)

function Run-Layer([string]$exe, [string[]]$argv) {
    # Smart App Control (Windows 11) moze zablokowac swiezo zlinkowany,
    # niepodpisany plik - kazda nowa wersja dostaje osobna ocene. Wtedy
    # linkujemy go od nowa, zamiast liczyc blokade jako "test zlapal".
    # stderr narzedzia (np. FAIL) nie moze przerwac skryptu w PowerShellu 5.1
    $ErrorActionPreference = "Continue"
    for ($try = 1; $try -le 15; $try++) {
        try {
            if ($argv -and $argv.Count -gt 0) { $out = & $exe @argv 2>&1 } else { $out = & $exe 2>&1 }
            return @{ code = $LASTEXITCODE; text = ($out -join "`n") }
        } catch [System.Management.Automation.ApplicationFailedException] {
            $target = [System.IO.Path]::GetFileNameWithoutExtension($exe)
            Remove-Item $exe -ErrorAction SilentlyContinue
            & cmake --build $Build --config Release --target $target 2>&1 | Out-Null
        }
    }
    throw "$exe jest wciaz blokowany przez kontrole aplikacji"
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
            $crlf = $find.Replace("`r`n", "`n").Replace("`n", "`r`n"); $rcrlf = $repl.Replace("`r`n", "`n").Replace("`n", "`r`n")
            if ($text.Contains($crlf)) { $find = $crlf; $repl = $rcrlf; $found = $true }
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
