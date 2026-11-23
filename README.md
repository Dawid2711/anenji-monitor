# Anenji Solar Monitor — ESP32 + RS485 (Modbus)

Monitor produkcji PV, zużycia energii i pracy baterii dla falownika **Anenji ANJ-4000W-24V-WIFI** (i kompatybilnych).  
Urządzenie komunikuje się z falownikiem przez **RS485 (Modbus RTU)**, prezentuje dane na **własnym web-serwerze** i potrafi **aktualizować firmware z GitHuba (OTA)**.

---

## 🎯 Co potrafi

| Funkcja | Opis |
|---------|------|
| **Podgląd na żywo (WebSocket)** | Odczyt falownika co 1 s, dane wypychane do przeglądarki przez WebSocket (port 81) — natychmiastowa aktualizacja bez odświeżania |
| **Przepływ energii** | Animowany schemat: PV → bateria / sieć / dom, z kolorowymi strzałkami (dash-dot w trybie jasnym) |
| **Historia 24 h** | Wykres co 5 min (PV / obciążenie / sieć) — zawsze widoczny na pulpicie |
| **Historia rozbudowana** | Zakresy: 15 min, 1 h, 6 h, 24 h, 7 d, 30 d, 90 d, 1 rok + tryby: wszystkie / tylko PV / tylko obciążenie / tylko sieć |
| **Produkcja 7 dni / 30 dni / miesiące** | Słupkowe wykresy dzienne i miesięczne z autarkią |
| **CSV Export** | Pobierz pełną historię dzienną jako CSV |
| **PV → Bateria** | Nowy licznik `pv_bat_wh` — ile energii z paneli poszło do ładowania baterii (kWh) |
| **OTA z GitHuba** | Sprawdzanie wersji: po starcie (~90 s) i codziennie o 12:00 + ręczne „Wgraj z GitHuba” (HTTPS, streaming, 2 KB buffer, 20 s timeout) |
| **Auto-rollback OTA** | Nowa wersja startuje "na próbę" — auto-zatwierdza po 60 s z WiFi lub 10 min bez WiFi; przy awarii bootloader wraca do poprzedniej wersji |
| **Tryb AP / STA** | Jeśli brak WiFi 5 min — podnosi AP `ANENJI-SETUP` / `anenji1234` → `192.168.4.1`; wyłącza AP po 1 min stabilnym połączeniu |
| **NTP + dzienne rollover** | Sumy dzienne zapisywane o północy (do 1000 dni w LittleFS) |
| **Tryb ciemny / jasny** | Przełącznik w nagłówku, zapamiętywany w localStorage |
| **Skaner ID falownika** | Automatyczne szukanie Modbus ID (1, 38, 2–10) |
| **Skaner rejestrów** | Odczyt blokowy rejestrów Holding/Input od dowolnego adresu |
| **Konfigurowalne rejestry** | Każdy sygnał: włączony, adres, typ (Holding/Input), skala, znak |
| **Korekcja prądu baterii** | Offset `bcOffset` dla rejestru 232 |
| **Tryb DEMO** | Symulowane dane sinusoidalne (dzień/noc, chmury, obciążenie) — testy UI bez falownika |

---

## 🛠 Hardware

| Element | Szczegóły |
|---------|-----------|
| **MCU** | ESP32 (np. ESP32-DOIT DevKit V1) |
| **RS485** | MAX485 / SP3485 na UART2 (GPIO16=RX, GPIO17=TX, GPIO4=DE/RE) |
| **Zasilanie** | 5 V z falownika (po step-down) lub USB |
| **Pamięć** | LittleFS (4 MB partycja z OTA) |
| **Falownik** | Anenji ANJ-4000W-24V-WIFI (Modbus ID=1, 9600 8N1, rejestry Holding) |

**Schemat podłączenia:**
```
Falownik (A/B) ──► MAX485 (A/B)
MAX485 RO ──► ESP32 GPIO16 (RX2)
MAX485 DI ◄── ESP32 GPIO17 (TX2)
MAX485 DE/RE ──► ESP32 GPIO4
MAX485 VCC ──► 3.3 V ESP32
MAX485 GND ──► GND ESP32
```

---

## 💻 Software / Stack

| Warstwa | Technologia |
|---------|-------------|
| **Firmware** | Arduino Core ESP32 (C++) |
| **Modbus** | `ModbusMaster` (Holding/Input Registers) |
| **Web Server** | `WebServer` (ESP32, port 80) + `ESPmDNS` (`anenji.local`) |
| **WebSocket** | `WebSocketsServer` (port 81, heartbeat 15 s) — live push do przeglądarki |
| **System plików** | `LittleFS` (state.bin, hist.bin, histh2.bin, days.bin) |
| **HTTPS OTA** | `WiFiClientSecure` + `HTTPClient` (streaming, 2 KB buffer, 20 s timeout) |
| **Auto-rollback** | `esp_ota_mark_app_valid_cancel_rollback()` po 60 s (WiFi) / 10 min (offline) |
| **Czas** | `configTzTime` (NTP: pool.ntp.org, time.google.com, strefa CET/CEST) |
| **Frontend** | Vanilla JS + inline SVG (brak zewnętrznych zależności, HTML gzip w PROGMEM) |
| **Wykresy** | SVG `<polyline>` + `<polygon>` (wypełnienie PV), tooltip / crosshair, pełny ekran |
| **Tematy** | CSS Custom Properties (`--card`, `--bg`, `--fg`…) + `data-theme` |
| **Konfiguracja** | `Preferences` (NVS) — WiFi, Modbus, rejestry, GitHub OTA, hasło OTA |
| **AP Watchdog** | Brak WiFi 5 min → AP ON; stabilne WiFi 1 min → AP OFF |

---

## 📊 Wygląd panelu (opis UI)

### Pulpit (Dashboard)
- **4 kafelki**: PV (kW), Bateria (%), Obciążenie (W), Sieć (W)
- **Animowany przepływ energii** (SVG):
  - ☀ PV → ⚡ bateria / 🏠 dom / ⚡ sieć
  - Strzałki dash-dot (widoczne w light/dark), kolory: PV=żółty, sieć=różowy, dom=niebieski, bateria=zielony
  - Wskaźnik SOC baterii (pasek napełniany)
  - Autarkia % i PV/obciążenie %
- **Statystyki dzienne**: Produkcja, Zużycie, Pobór z sieci, **PV → Bateria (kWh)**, Autarkia
- **Szczegóły falownika**: Bateria V/A, Temp, Sieć AC V/Hz, Wyjście V
- **Historia 24 h** (wykres SVG, 3 serie, siatka dash-dot, os Y w kW)
- **Produkcja 7 dni** (słupki, dzisiaj = żółty)

### Historia (zakładka)
- **Wybór zakresu**: 15 min → 1 rok
- **Wybór trybu**: Wszystkie / Tylko PV / Tylko Obciążenie / Tylko Sieć
- **Wykres z tooltipem i crosshair** (klik = pełny ekran, dotyk = wartości)
- **Produkcja 7 dni / 30 dni / Miesiące / Dni (tabela)** + przycisk CSV + Wyczyść historię

### Ustawienia
- **WiFi**: skan, SSID, hasło, zapis + restart
- **Modbus**: ID, baud, offset adresów, kierunek prądu baterii, korekta prądu
- **Skaner rejestrów**: od adresu, ile, wyniki w tabeli
- **Rejestry sygnałów**: dla każdego — włączony, adres, typ (Holding/Input), skala, znak
- **GitHub OTA**: user, repo, włącz sprawdzanie, przyciski: Sprawdź / Wgraj
- **OTA plikiem**: wybierz `.ino.bin`, hasło OTA, pasek postępu
- **Zmiana hasła OTA**

---

## 🚀 Instalacja i pierwsza konfiguracja

1. **Wgraj firmware** (ESP32 Dev Module, Partition Scheme: *Default 4MB with spiffs*):
   - Arduino IDE → *Szkic → Eksportuj skompilowany plik binarny* → `firmware.bin`
   - lub `esptool.py write_flash 0x10000 firmware.bin`
2. **Zasilanie** → ESP32 podnosi AP `ANENJI-SETUP` / `anenji1234`
3. **Połącz się** z AP → otwórz `http://192.168.4.1`
4. **Ustawienia → WiFi** → wybierz sieć 2.4 GHz, wpisz hasło → *Zapisz WiFi i restart*
5. Po restarcie ESP łączy się z routerem → IP widoczne w serialu / na ekranie (jeśli masz)
6. **Ustawienia → Modbus** → sprawdź ID (domyślnie 1), baud 9600, ewentualnie *Szukaj ID*
7. **Ustawienia → GitHub OTA** → wpisz `user/repo` (np. `Dawid2711/anenji-monitor`), włącz *sprawdzaj codziennie o 12:00*
8. Gotowe! Panel dostępny pod `http://anenji.local` (mDNS) lub IP ESP.

---

## 🔧 Konfiguracja GitHub OTA (automatyczne aktualizacje)

W repozytorium na GitHubie (branch `main`) muszą być dwa pliki:
- `version.txt` — jedna linia: `2026.11.24.7`
- `firmware.bin` — skompilowany `.ino.bin` (Arduino: *Szkic → Eksportuj skompilowany plik binarny*)

**Kolejność:** najpierw `firmware.bin`, potem `version.txt`.  
ESP codziennie o 12:00 sprawdza `version.txt`; jeśli nowsza → w UI pojawia się *Wgraj z GitHuba*.  
Ręcznie: *Ustawienia → Sprawdź aktualizację* → *Wgraj z GitHuba* (wymaga hasła OTA).

---

## 📂 Struktura plików w LittleFS

| Plik | Treść |
|------|-------|
| `/state.bin` | Stan dzienny: `pv_wh, load_wh, grid_wh, pv_bat_wh, daysKwh[7], curDay, curDate` |
| `/hist.bin` | Historia 5-min × 288 (24 h): `pv[], ld[], gr[]` + epoch |
| `/histh2.bin` | Historia 1-h × 720 (30 dni): `pv[], ld[], gr[]` + epoch |
| `/days.bin` | Sumy dzienne (DayRec): `date, pv, load, grid` (do 1000 rekordów) |

### Trzy bufora historii (RAM → LittleFS)
- **30 sekund × 120 = 1 h** (`hist_s_*`) — tylko RAM, dla wykresów 15 min / 1 h
- **5 min × 288 = 24 h** (`hist_*`) — zapisywane do `/hist.bin` co 30 min
- **1 h × 720 = 30 dni** (`hist_h_*`) — zapisywane do `/histh2.bin` co 30 min
- **Gap filling**: po restarcie uzupełnia zera za czas wyłączony (na podstawie epoch w plikach)

---

## 🧪 Tryb DEMO

W pasku narzędzi przycisk **LIVE/DEMO**. W DEMO generowane są sinusoidalne dane (dzień/noc, chmury, obciążenie) — do testów UI bez falownika.

---

## 🔐 Bezpieczeństwo

- Hasło OTA (domyślnie `anenji`) — zmienia się w *Ustawienia → OTA → Zmiana hasła OTA*
- WiFi hasło nie jest widoczne w UI (tylko przy wpisywaniu)
- GitHub OTA tylko HTTPS (`WiFiClientSecure::setInsecure()` — certyfikaty nie są weryfikowane, ale kanał szyfrowany)

---

## 📝 Changelog (skrót)

| Wersja | Data | Główne zmiany |
|--------|------|---------------|
| **2026.11.24.7** | 2026-11-23 | Light theme: widoczne dash-dot siatki, poprawiony kontrast; firmware.bin z nową ścieżką build |
| **2026.11.24.5** | 2026-11-21 | Siatekka osi Y i linie serii jako dash-dot (5 5 / 5 3) |
| **2026.11.24.4** | 2026-11-21 | Rozdzielone wykresy Pulpit vs Historia (unikalne ID), brak konfliktów |
| **2026.11.24.3** | 2026-11-21 | `pv_bat_wh` (PV→Bateria kWh), fallback zapisu historii przy braku Modbus |
| **2026.11.24.1** | 2026-11-21 | Pełny ekran wykresów z tooltipem, poprawki motywów, OTA robust |
| **2026.11.20** | 2026-11-19 | Pierwsza wersja z GitHub OTA, LittleFS historia, SVG dashboard |

---

## 📄 Licencja

MIT — używaj, modyfikuj, dziel się.  
Jeśli poprawisz coś sensownego — PR mile widziany! 🙌

---

## 🙏 Podziękowania

- **ModbusMaster** — 4-20ma / Doc Walker
- **Arduino ESP32 Core** — Espressif + społeczność
- Wszystkim testerom na żywych instalacjach (rodzice, sąsiedzi, ja 😉)

---

> **Autor:** Dawid (GitHub: [@Dawid2711](https://github.com/Dawid2711))  
> **Repo:** `github.com/Dawid2711/anenji-monitor`  
> **Wersja:** `2026.11.24.7` (format `YYYY.MM.DD.N` — N-ta poprawka tego dnia)