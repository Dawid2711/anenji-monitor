# Anenji Solar Monitor — ESP32 + RS485 (Modbus)

**Language / Język:** [🇬🇧 English](#-english) | [🇵🇱 Polski](#-polski)

---

# 🇬🇧 English

## Overview

PV production, energy consumption, and battery monitoring for **Anenji ANJ-4000W-24V-WIFI** inverter (and compatibles).  
Communicates via **RS485 (Modbus RTU)**, serves a **web UI**, and supports **GitHub OTA firmware updates**.

---

## 🎯 Features

| Feature | Description |
|---------|-------------|
| **Live view (WebSocket)** | Inverter read every 1 s, pushed to browser via WebSocket (port 81) — instant updates without refresh |
| **Energy flow diagram** | Animated SVG: PV → battery / grid / house, colored dash-dot arrows (visible in light/dark mode) |
| **24 h history** | 5-min resolution chart (PV / load / grid) — always visible on Dashboard |
| **Extended history** | Ranges: 15 min, 1 h, 6 h, 24 h, 7 d, 30 d, 90 d, 1 yr + modes: all / PV only / load only / grid only |
| **7/30 day & monthly production** | Bar charts with autarky % |
| **CSV Export** | Download full daily history as CSV |
| **PV → Battery counter** | `pv_bat_wh` — energy from panels used to charge battery (kWh) |
| **GitHub OTA** | Version check: on boot (~90 s) + daily at 12:00 + manual "Flash from GitHub" (HTTPS, streaming, 2 KB buffer, 20 s timeout) |
| **Auto-rollback OTA** | New version runs "on trial" — auto-confirms after 60 s with WiFi or 10 min offline; on failure bootloader rolls back |
| **AP / STA mode** | No WiFi for 5 min → starts AP `ANENJI-SETUP` / `anenji1234` → `192.168.4.1`; disables AP after 1 min stable connection |
| **NTP + daily rollover** | Daily sums stored at midnight (up to 1000 days in LittleFS) |
| **Dark / Light theme** | Toggle in header, persisted in localStorage |
| **Inverter ID scanner** | Auto-search Modbus ID (1, 38, 2–10) |
| **Register scanner** | Block read of Holding/Input registers from any address |
| **Configurable registers** | Per signal: enable, address, type (Holding/Input), scale, sign |
| **Battery current offset** | `bcOffset` for register 232 |
| **DEMO mode** | Simulated sinusoidal data (day/night, clouds, load) — UI testing without inverter |

---

## 🛠 Hardware

| Component | Details |
|-----------|---------|
| **MCU** | ESP32 (e.g., ESP32-DOIT DevKit V1) |
| **RS485** | MAX485 / SP3485 on UART2 (GPIO16=RX, GPIO17=TX, GPIO4=DE/RE) |
| **Power** | 5 V from inverter (via step-down) or USB |
| **Storage** | LittleFS (4 MB partition with OTA) |
| **Inverter** | Anenji ANJ-4000W-24V-WIFI (Modbus ID=1, 9600 8N1, Holding registers) |

**Wiring diagram:**
```
Inverter (A/B) ──► MAX485 (A/B)
MAX485 RO ──► ESP32 GPIO16 (RX2)
MAX485 DI ◄── ESP32 GPIO17 (TX2)
MAX485 DE/RE ──► ESP32 GPIO4
MAX485 VCC ──► 3.3 V ESP32
MAX485 GND ──► GND ESP32
```

---

## 💻 Software Stack

| Layer | Technology |
|-------|------------|
| **Firmware** | Arduino Core ESP32 (C++) |
| **Modbus** | Built-in `MiniModbus` (Modbus RTU 0x03/0x04, short timeout, block read) — no external library since 2026.11.25.1 |
| **Web Server** | `WebServer` (ESP32, port 80) + `ESPmDNS` (`anenji.local`) |
| **WebSocket** | `WebSocketsServer` (port 81, heartbeat 15 s) — live push to browser |
| **Filesystem** | `LittleFS` (state.bin, hist.bin, histh2.bin, days.bin) |
| **HTTPS OTA** | `WiFiClientSecure` + `HTTPClient` (streaming, 2 KB buffer, 20 s timeout) |
| **Auto-rollback** | `esp_ota_mark_app_valid_cancel_rollback()` after 60 s (WiFi) / 10 min (offline) |
| **Time** | `configTzTime` (NTP: pool.ntp.org, time.google.com, CET/CEST) |
| **Frontend** | Vanilla JS + inline SVG (no external deps, HTML gzipped in PROGMEM) |
| **Charts** | SVG `<polyline>` + `<polygon>` (PV fill), tooltip / crosshair, fullscreen |
| **Themes** | CSS Custom Properties (`--card`, `--bg`, `--fg`…) + `data-theme` |
| **Config** | `Preferences` (NVS) — WiFi, Modbus, registers, GitHub OTA, OTA password |
| **AP Watchdog** | No WiFi 5 min → AP ON; stable WiFi 1 min → AP OFF |

---

## 📊 UI Description

### Dashboard
- **4 tiles**: PV (kW), Battery (%), Load (W), Grid (W)
- **Animated energy flow** (SVG):
  - ☀ PV → ⚡ battery / 🏠 house / ⚡ grid
  - Dash-dot arrows (visible in light/dark), colors: PV=yellow, grid=pink, house=blue, battery=green
  - Battery SOC bar
  - Autarky % and PV/Load %
- **Daily stats**: Production, Consumption, Grid import, **PV → Battery (kWh)**, Autarky
- **Inverter details**: Battery V/A, Temp, Grid AC V/Hz, Output V
- **24 h history** (SVG, 3 series, dash-dot grid, Y axis in kW)
- **7 day production** (bars, today = yellow)

### History (tab)
- **Range selector**: 15 min → 1 yr
- **Mode selector**: All / PV only / Load only / Grid only
- **Chart with tooltip & crosshair** (click = fullscreen, touch = values)
- **7 day / 30 day / Monthly / Days table** + CSV button + Clear history

### Settings
- **WiFi**: scan, SSID, password, save + restart
- **Modbus**: ID, baud, address offset, battery current direction, current offset
- **Register scanner**: from address, count, results table
- **Signal registers**: per signal — enabled, address, type (Holding/Input), scale, sign
- **GitHub OTA**: user, repo, enable check, buttons: Check / Flash
- **File OTA**: pick `.ino.bin`, OTA password, progress bar
- **Change OTA password**

---

## 🚀 Installation & First Setup

1. **Flash firmware** (ESP32 Dev Module, Partition Scheme: *Default 4MB with spiffs*):
   - Arduino IDE → *Sketch → Export compiled Binary* → `firmware.bin`
   - or `esptool.py write_flash 0x10000 firmware.bin`
2. **Power on** → ESP32 starts AP `ANENJI-SETUP` / `anenji1234`
3. **Connect** to AP → open `http://192.168.4.1`
4. **Settings → WiFi** → pick 2.4 GHz network, enter password → *Save WiFi & restart*
5. After reboot ESP connects to router → IP visible in serial / on screen (if present)
6. **Settings → Modbus** → verify ID (default 1), baud 9600, optionally *Find ID*
7. **Settings → GitHub OTA** → enter `user/repo` (e.g., `Dawid2711/anenji-monitor`), enable *check daily at 12:00*
8. Done! Panel at `http://anenji.local` (mDNS) or ESP IP.

---

## 🔧 GitHub OTA Configuration

On GitHub (branch `main`) two files are required:
- `version.txt` — single line: `2026.11.24.7`
- `firmware.bin` — compiled `.ino.bin` (Arduino: *Sketch → Export compiled Binary*)

**Order:** upload `firmware.bin` first, then `version.txt`.  
ESP checks `version.txt` daily at 12:00; if newer → *Flash from GitHub* appears in UI.  
Manual: *Settings → Check update* → *Flash from GitHub* (requires OTA password).

---

## 📂 LittleFS File Structure

| File | Content |
|------|---------|
| `/state.bin` | Daily state: `pv_wh, load_wh, grid_wh, pv_bat_wh, daysKwh[7], curDay, curDate` |
| `/hist.bin` | 5-min × 288 (24 h): `pv[], ld[], gr[]` + epoch |
| `/histh2.bin` | 1-h × 720 (30 d): `pv[], ld[], gr[]` + epoch |
| `/days.bin` | Daily sums (DayRec): `date, pv, load, grid` (up to 1000 records) |

### Three History Buffers (RAM → LittleFS)
- **30 s × 120 = 1 h** (`hist_s_*`) — RAM only, for 15 min / 1 h charts
- **5 min × 288 = 24 h** (`hist_*`) — saved to `/hist.bin` every 30 min
- **1 h × 720 = 30 d** (`hist_h_*`) — saved to `/histh2.bin` every 30 min
- **Gap filling**: on restart fills zeros for downtime (based on epoch in files)

---

## 🧪 DEMO Mode

Toolbar button **LIVE/DEMO**. DEMO generates sinusoidal data (day/night, clouds, load) — for UI testing without inverter.

---

## 🔐 Security

- OTA password (default `anenji`) — change in *Settings → OTA → Change OTA password*
- WiFi password not visible in UI (only during entry)
- GitHub OTA over HTTPS only (`WiFiClientSecure::setInsecure()` — certs not verified but channel encrypted)

---

## 📝 Changelog

| Version | Date | Key Changes |
|---------|------|-------------|
| **2026.11.24.7** | 2026-11-23 | Light theme: visible dash-dot grid, improved contrast; firmware.bin from new build path |
| **2026.11.24.5** | 2026-11-21 | Y-axis grid and series lines as dash-dot (5 5 / 5 3) |
| **2026.11.24.4** | 2026-11-21 | Separated Dashboard vs History charts (unique IDs), no conflicts |
| **2026.11.24.3** | 2026-11-21 | `pv_bat_wh` (PV→Battery kWh), history fallback when Modbus unavailable |
| **2026.11.24.1** | 2026-11-21 | Fullscreen charts with tooltip, theme fixes, robust OTA |
| **2026.11.20** | 2026-11-19 | First version with GitHub OTA, LittleFS history, SVG dashboard |

---

## 📄 License

MIT — use, modify, share.  
If you fix something useful — PR welcome! 🙌

---

## 🙏 Acknowledgments

- **ModbusMaster** — 4-20ma / Doc Walker (used until 2026.11.24.7, API kept compatible)
- **Arduino ESP32 Core** — Espressif + community
- All testers on live installs (parents, neighbors, me 😉)

---

> **Author:** Dawid (GitHub: [@Dawid2711](https://github.com/Dawid2711))  
> **Repo:** `github.com/Dawid2711/anenji-monitor`  
> **Version:** `2026.11.24.7` (format `YYYY.MM.DD.N` — N-th patch of that day)

---

# 🇵🇱 Polski

## Przegląd

Monitor produkcji PV, zużycia energii i pracy baterii dla falownika **Anenji ANJ-4000W-24V-WIFI** (i kompatybilnych).  
Komunikacja przez **RS485 (Modbus RTU)**, własny **web-serwer** i **aktualizacje OTA z GitHuba**.

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
| **Modbus** | Wbudowany `MiniModbus` (Modbus RTU 0x03/0x04, krótki timeout, odczyt blokiem) — bez zewnętrznej biblioteki od 2026.11.25.1 |
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

- **ModbusMaster** — 4-20ma / Doc Walker (używany do wersji 2026.11.24.7, API zachowane)
- **Arduino ESP32 Core** — Espressif + społeczność
- Wszystkim testerom na żywych instalacjach (rodzice, sąsiedzi, ja 😉)

---

> **Autor:** Dawid (GitHub: [@Dawid2711](https://github.com/Dawid2711))  
> **Repo:** `github.com/Dawid2711/anenji-monitor`  
> **Wersja:** `2026.11.24.7` (format `YYYY.MM.DD.N` — N-ta poprawka tego dnia)