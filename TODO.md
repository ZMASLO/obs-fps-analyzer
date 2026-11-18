# TODO

## Testy regresji na nagraniach innych kart

Dzisiejsze nagrania w `plugins/fps-analyzer/tests/traces` (fpstrace v4, PS5) obejmują tylko Elgato 4K X i 4K Mk2. Każda kolejna karta nagrana z grą w 30 FPS zostaje na stałe w zestawie i pilnuje, że plugin ocenia ją poprawnie: blokuje FPS przy karcie, która zmienia klatki, a przy dobrej karcie pokazuje prawidłowe 30.

- [ ] **Elgato HD60 X:** gra w 30 FPS, menu konsoli, zapauzowana gra
- [ ] **Avermedia Live Gamer 4K:** jak wyżej
- [ ] inne karty zgłoszone przez użytkowników (README prosi o ślad z gry w 30 FPS)

Jak nagrać: filtr → Debug options → „Record FPS trace (per-frame CSV)”, ok. 20 s. Plik trafia do `tests/traces/<karta>_<format>_<scena>_v4.csv`. W `tests/cases.txt` dopisz wpis `self, notblocked, fps@3-20=30+-1` (dobra karta) albo `self, blocked=altered` (karta zmieniająca klatki). W README uzupełnij listę „Supported capture devices”.

Pamiętaj, co te nagrania sprawdzają:
- **sprawdzają:** liczenie FPS z decyzji „nowa / powtórzona klatka”, blokadę dla zmienionych klatek (z histogramu różnic) i dudnienie;
- **nie sprawdzają:** samego porównywania pikseli i odczytu formatów. Ślad nie zawiera obrazu, a tę część pilnują `fps-selftest` i klipy `tests/fixtures/*.y4m`;
- **przy świadomej zmianie algorytmu** (inne liczenie, próg czułości) `self` się nie zgodzi. Wtedy nagrania trzeba zrobić na nowo.

## Wykrywanie klatek gubionych przez kartę

17.11.2026: 4K Mk2 chwilami gubi klatki sama, zanim dotrą do OBS. Jej znaczniki czasu skaczą o 33,4 ms zamiast 16,7, od 5 do 9 razy na sekundę. Podgląd się wtedy zacina, a przy grze w 60 FPS odczyt spada, chociaż gra niczego nie traci. Plugin tego dziś nie widzi. OBS renderuje bez strat (0 pominiętych), iCUE nie jest przyczyną, a ponowne uruchomienie karty (zmiana rozdzielczości) usuwa problem na jakiś czas. Nagrania: `fpstrace_lag_mk2_*` (98 i 257 dziur) oraz po restarcie karty (0).

### Kto zgubił klatkę: sprzęt czy OBS

Dwa niezależne źródła danych: znaczniki czasu nadane przez kartę (moment przechwycenia) oraz takty OBS (wiersze K) z licznikiem klatek opuszczonych przez lag renderowania (libobs: `obs_get_lagged_frames()`, `video_output_get_skipped_frames()`).

| Objaw | Znaczniki karty | Takty OBS | Winny |
|---|---|---|---|
| karta zgubiła klatkę | dziura (2 × okres) | w tym miejscu takt bez nowej klatki, OBS równo | sprzęt / sterownik |
| OBS zgubił klatkę | dziura (filtr widzi tylko klatki wybrane przez OBS) | takt spóźniony albo rośnie licznik lagu OBS, bez pustego taktu | OBS |
| rozjazd 59,94 / 60 | bez dziur | pusty takt co ~16,7 s | ustawienia (już wykrywane) |
| nierówny klatkaż gry | bez dziur | równo | gra (to mierzymy) |

Nagranie `lag_mk2` (17.11.2026) to czysty pierwszy wiersz: 98 dziur w znacznikach = 98 pustych taktów, 0 opuszczonych klatek w OBS.

**Brak kolizji z pomiarem klatkażu:** nierówny klatkaż to sprawa treści (gra powtarza klatkę), a karta i tak stempluje sygnał HDMI równo co okres. Wykrywanie gubienia patrzy tylko na znaczniki i takty, nigdy na zawartość klatek. Wykrywanie dudnienia i „uneven” ma już testy z losowym gubieniem klatek, które nie dają alarmu.

**Częściowo zrobione:** wykres frame pacingu już oznacza na szaro próbki z klatką zgubioną przez kartę (dziura ponad 1,5 × mediana odstępu w znacznikach), więc taka klatka nie wygląda jak zacięcie gry. Odczyt FPS i blokada jeszcze tego nie uwzględniają.

**Zrobione (18.11.2026, v0.6.0):** ostrzeżenie w overlayu „Capture device drops frames (N/s) - FPS reads low” albo „OBS drops frames (N/s, rendering lag)”, FPS zostaje widoczny i nie jest poprawiany (decyzja użytkownika). Checkbox w filtrze „Warn when the capture device or OBS drops frames”, domyślnie włączony. Przypisanie: pusty takt OBS w promieniu 3 taktów od dziury (przed albo po, bo OBS buforuje klatki) oznacza kartę, brak pustego taktu oznacza OBS. Do tego licznik lagu OBS (`obs_get_lagged_frames`), także dla Game Capture. Źródła zmienne (odstępy niebędące wielokrotnością okresu) są pomijane. Testy: `s27_dropped_frames` i trzy nagrania Mk2 (98/0, 249/1, 113/1 klatek karta/OBS).

- [x] Nagranie prawdziwego lagu OBS: `tests/traces/mk2_yuy2_obslag_v4.csv` (122 klatki pominięte przez OBS, test `drops=obs`).
- [ ] Dokładniejsze przypisanie do OBS: czas wideo OBS (kolumna `video_time_ns` w wierszach K, `obs_get_video_frame_time`) pokazuje każdą pominiętą klatkę jako krok o dwa okresy (w nagraniu 122 = 122 ze statystyk OBS). Rdzeń bierze dziś zegar taktu filtra, który widzi tylko część z nich, więc przy gęstych stratach karty część strat OBS trafia na konto karty (93 OBS / 63 karta zamiast ~122 / ~36). Dać rdzeniowi czas wideo i rozstrzygać: krok czasu wideo przy dziurze → OBS, w przeciwnym razie pusty takt → karta.

## Wydajność (pomiar 18.11.2026, `fps-bench` / `tools/obs-cost.py`)

- [x] **Histogram różnic był za drogi** (18.11.2026): blokada liczy teraz tylko liczbę różnych bajtów i max |Δ| (SSE2), a pełny histogram tylko przy nagrywaniu śladu. 4K NV12 Full frame: 10,6 → 0,98 ms, YUY2 12,0 → 2,4 ms, BGRA 15,7 → 6,9 ms. Wynik bit w bit jak wcześniej (`s28_diff_equivalence`).
- [ ] YUY2 i BGRA: teraz dominuje wyciąganie jasności z pikseli (2,4 / 6,9 ms przy 4K), też da się zwektoryzować.
- [ ] Wykresy kosztują ok. 1,8 ms renderu (3 wykresy). Krzywą FPS/frametime można liczyć raz na takt zamiast przy każdym renderze.

## Inne otwarte sprawy

- [x] **Fałszywe „59,94 Hz” przy seriach zgubionych klatek** (18.11.2026, PC z RTX 5090 → 4K Mk2, OBS 60): naprawione. Pomysł „ignoruj dudnienie, gdy znaczniki się zgadzają” odrzucony, bo 4K X przy prawdziwym 59,94 też podaje równe 60,000. Zamiast tego dwie kontrole z fizyki: łańcuch pustych taktów OBS wymaga, by klatki przychodziły (zegar systemu, regresja względem znaczników, min. 20 s) z częstotliwością różną od OBS o co najmniej 0,5 × 1/P (PC: 59,9993 Hz → odrzucony; 4K X: 59,943 Hz → zostaje); klaster identycznych klatek z dziurą w znacznikach to straty karty, nie przeliczanie. Testy: `s23_capture_beat` (serie strat po stronie OBS i karty, też na prawdziwym 59,94), 5 mutacji. Realne nagranie: punkt niżej.

- [ ] **Realne nagranie fałszywego „59,94 Hz”**: 75 s z domyślnymi kolorami NVIDIA (`mk2_yuy2_pc_nvidia_default_full_v4.csv`) nie złapało serii strat (karta zgubiła 1 klatkę), więc poprawkę potwierdzają na razie tylko symulacje `s23_capture_beat`. Gdy Mk2 znów zacznie gubić klatki seriami, nagrać ≥ 70 s i dodać jako przypadek z `nobeat, drops=card`.

- [ ] **Włączenie nagrywania śladu na chwilę zdejmuje blokadę** (18.11.2026, PC z domyślnymi kolorami NVIDIA): przy starcie śladu log pokazuje „FPS measurement possible again”, a po ok. 5 s znowu „FPS hidden: the capture device alters every frame”. Przez te sekundy overlay może pokazać niewiarygodny FPS. Prawdopodobnie zmiana parametrów rdzenia (`measure_histogram` w `core_sync`) kasuje stan wykrywania zmienionych klatek. Blokada powinna przetrwać zmianę samych opcji debugowania.

- [ ] Elgato 4K Mk2: potwierdzić, że dawny problem z „laggy OBS preview” już nie występuje. README mówi teraz „works” na podstawie testów 0.5.2.
- [ ] Rozważyć obsługę P010 (10 bit). 4K X daje go tylko w 4K30 / 1440p60 / 1080p120, a sama karta i tak zmienia klatki, więc to raczej dla innych kart.
