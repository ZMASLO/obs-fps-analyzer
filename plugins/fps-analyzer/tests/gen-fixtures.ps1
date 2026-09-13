# Generuje klipy referencyjne o ZNANEJ kadencji, ktore fps-cli odtwarza przez rdzen.
# Kadencja jest tu niezalezna prawda: wiemy, ile klatek jest nowych i kiedy, wiec
# oczekiwany FPS wynika z samego klipu, a nie z tego, co wtyczka wyliczyla.
#
#   powershell -ExecutionPolicy Bypass -File .\gen-fixtures.ps1
#
# Wymaga ffmpeg w PATH. Klipy sa male (32x18 mono), calosc ponizej 2 MB, wiec
# leza w repozytorium. Po regeneracji sprawdz `git diff` na SHA256SUMS: rozne
# wersje ffmpeg moga dac inne bajty, wtedy trzeba odswiezyc wzorce
# (fps-cli --manifest ... --update-goldens), ale oczekiwania fps@ nadal musza sie
# zgadzac - one opisuja kadencje, nie konkretne piksele.

param(
    # Renderuje powiekszone MP4 do fixtures/preview (gitignore), zeby dalo sie
    # obejrzec klipy w odtwarzaczu. Same y4m maja 32x18 w skali szarosci, wiec
    # w VLC to znaczek pocztowy. Podglad nie jest uzywany przez zadne testy.
    [switch]$Preview,

    # Renderuje wersje 1080p do fixtures/local (gitignore) do testu E2E w OBS.
    # Media Source w OBS jest zrodlem ASYNCHRONICZNYM, czyli ta sama sciezka
    # kodu co karta przechwytujaca (filter_video, NV12). Pozwala wiec nagrac
    # slad ze sciezki async bez karty, i to o ZNANEJ kadencji, wiec oprocz
    # samoodtwarzalnosci mozna sprawdzic, czy odczyt jest poprawny.
    [switch]$Local
)

$ErrorActionPreference = "Stop"
$dir = Join-Path $PSScriptRoot "fixtures"
New-Item -ItemType Directory -Force $dir | Out-Null

if ($Preview) {
    $pv = Join-Path $dir "preview"
    New-Item -ItemType Directory -Force $pv | Out-Null
    Get-ChildItem $dir -Filter *.y4m | Sort-Object Name | ForEach-Object {
        $out = Join-Path $pv ($_.BaseName + ".mp4")
        Write-Host "=== podglad $($_.BaseName)" -ForegroundColor Cyan
        # neighbor zeby piksele zostaly ostre, fps_mode passthrough zeby
        # duplikaty klatek przetrwaly - to wlasnie one niosa kadencje
        & ffmpeg -y -hide_banner -loglevel error -i $_.FullName `
            -vf "scale=640:-2:flags=neighbor" -fps_mode passthrough `
            -c:v libx264 -crf 18 -preset veryfast -pix_fmt yuv420p $out
        if ($LASTEXITCODE -ne 0) { throw "ffmpeg nie powiodl sie dla podgladu $($_.Name)" }
    }
    Write-Host "`nPodglad w $pv" -ForegroundColor Green
    return
}

if ($Local) {
    $lc = Join-Path $dir "local"
    New-Item -ItemType Directory -Force $lc | Out-Null
    Get-ChildItem $dir -Filter *.y4m | Sort-Object Name | ForEach-Object {
        $out = Join-Path $lc ($_.BaseName + "_1080p.mkv")
        Write-Host "=== 1080p $($_.BaseName)" -ForegroundColor Cyan
        # neighbor: kazdy piksel zrodla staje sie jednolitym blokiem, wiec
        # kadencja zostaje dokladnie ta sama. crf 0 = bezstratnie, zeby
        # duplikaty byly bajtowo identyczne - inaczej analiza uznalaby je za
        # nowe klatki. fps_mode passthrough zeby enkoder ich nie wyrzucil.
        & ffmpeg -y -hide_banner -loglevel error -i $_.FullName `
            -vf "scale=1920:1080:flags=neighbor" -fps_mode passthrough `
            -c:v libx264 -crf 0 -preset ultrafast -pix_fmt yuv420p $out
        if ($LASTEXITCODE -ne 0) { throw "ffmpeg nie powiodl sie dla $($_.Name)" }
        $n = (& ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 $out)
        Write-Host "    $n klatek, $([int]((Get-Item $out).Length/1MB)) MB"
    }
    Write-Host "`nKlipy 1080p w $lc" -ForegroundColor Green
    Write-Host "W OBS: dodaj Media Source wskazujacy na plik, odznacz petle, dodaj filtr FPS Analyzer." -ForegroundColor Gray
    Write-Host "Media Source jest zrodlem async, wiec slad bedzie mial path=async, jak z karty." -ForegroundColor Gray
    return
}

$sz = "32x18"
$noise = "noise=alls=40:allf=t+u:all_seed=1234"

function Gen([string]$name, [string]$filter, [string]$dur) {
    $out = Join-Path $dir $name
    Write-Host "=== $name" -ForegroundColor Cyan
    $args = @("-y", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", $filter)
    if ($dur) { $args += @("-t", $dur) }
    $args += @("-pix_fmt", "gray", $out)
    & ffmpeg @args
    if ($LASTEXITCODE -ne 0) { throw "ffmpeg nie powiodl sie dla $name" }
    $frames = (& ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 $out)
    Write-Host "    $frames klatek, $((Get-Item $out).Length) bajtow"
}

# --- Stale tempo: kazda klatka nowa ---
Gen "gen_60.y4m"  "testsrc2=size=${sz}:rate=60,$noise"  "5"
Gen "gen_120.y4m" "testsrc2=size=${sz}:rate=120,$noise" "5"

# --- Duplikaty: tresc wolniejsza niz strumien, jak gra wolniejsza od przechwytywania ---
Gen "gen_30in60.y4m" "testsrc2=size=${sz}:rate=30,$noise,fps=60" "5"
Gen "gen_24in60.y4m" "testsrc2=size=${sz}:rate=24,$noise,fps=60" "5"

# --- Zamrozenie na 100 ms w t=2 s: klatki 121-126 wypadaja, fps=60 klonuje 120. ---
Gen "gen_stutter.y4m" "testsrc2=size=${sz}:rate=60,$noise,select='not(between(n\,121\,126))',fps=60" "5"

# --- Ruch przez 1 s, potem bezruch: po 2 s ciszy analiza zeruje odczyt ---
Gen "gen_static.y4m" "testsrc2=size=${sz}:rate=60,$noise,trim=end=1,setpts=PTS-STARTPTS,tpad=stop_mode=clone:stop_duration=3" ""

# --- Zmiana tempa 60 -> 30 w t=3 s ---
Gen "gen_60to30.y4m" "testsrc2=size=${sz}:rate=60,$noise,trim=end=3,setpts=PTS-STARTPTS[a];testsrc2=size=${sz}:rate=30,noise=alls=40:allf=t+u:all_seed=99,fps=60,trim=end=3,setpts=PTS-STARTPTS[b];[a][b]concat=n=2:v=1" ""

# --- Tearing: gorna polowa aktualizuje sie w innych klatkach niz dolna, wiec
#     linie sondujace {0, h/2, h-1} nie zmieniaja sie razem ---
Gen "gen_tear.y4m" "testsrc2=size=${sz}:rate=30,$noise,fps=60,split[a][b];[a]crop=32:9:0:0[t];[b]crop=32:9:0:9,tpad=start=1:start_mode=clone[bb];[t][bb]vstack" "5"

# --- Sumy kontrolne, zeby regeneracja byla widoczna w diffie ---
$sums = Join-Path $dir "SHA256SUMS"
Get-ChildItem $dir -Filter *.y4m | Sort-Object Name | ForEach-Object {
    "$((Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower())  $($_.Name)"
} | Set-Content -Encoding ascii $sums
Write-Host "`nZapisano $sums" -ForegroundColor Green
Get-Content $sums
