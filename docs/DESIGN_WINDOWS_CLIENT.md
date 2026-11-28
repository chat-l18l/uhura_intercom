# Ontwerp & Implementatie: Native Windows 11 Client (`uhura.exe`)

**Status:** Geïmplementeerd & Native Geverifieerd op Windows 11 (`pixi run build-win`)  
**Resultaat:** Eén losse, direct startende **native Windows 11 executable (`build-win/uhura.exe`, ~800 KB incl. `libopus`)** zonder installer, zonder externe `.dll`-bestanden en zonder web/Java/.NET-frameworks, gebouwd bovenop exact dezelfde `src/core/` als Linux en ESP32.

---

## 1. Waarom een Native Windows Client Waardevol is (Twee Redenen)

1. **Voor de mantelzorgers overdag (*Edwin* én *Zus*):**
   - Bij Moeder thuis staat straks het fysieke **ESP32-kastje** met echte knoppen in de woonkamer en op de overloop.
   - Maar de familieleden zitten overdag vaak achter een **Windows 11 laptop of pc**. Door simpelweg één mapje met `uhura.exe` en een tekstbestandje `uhura.txt` te delen (zie [HANDLEIDING_VOOR_ZUS.md](./HANDLEIDING_VOOR_ZUS.md)), dubbelklikt je zus op `uhura.exe` en heeft ze zonder enige installatie direct een virtueel intercomkastje op haar scherm.
2. **Praktisch ontwikkelvoordeel op een Windows 11 PC met WSL2:**
   - WSL2 draait onder Windows 11 achter een virtuele NAT-switch en een vertraagde audio-brug (>100 ms vertraging). Een native `uhura.exe` draait **rechtstreeks op Windows 11 zelf**: direct bereikbaar op het LAN-IP van de pc én met echte low-latency `10 ms` audioblokken op de Windows geluidskaart — ideaal als tegenstation tijdens het testen van het ESP32-prototype en met mensen op andere locaties.

---

## 2. De Cross-Compile Architectuur (`pixi run build-win`)

### Waarom `MinGW-w64` GCC (UCRT) vanuit Pixi in plaats van Microsoft `cl.exe`?
Microsofts traditionele `cl.exe` (MSVC) ondersteunt moderne **ISO C23** (`[[nodiscard]]`, `<stdatomic.h>`) nog altijd onvolledig. Door in `pixi.toml` het officiële `conda-forge` pakket `gcc_win-64` (GCC 15/16 UCRT cross-compiler) op te nemen:
- Bouw je vanuit elke Linux- of WSL-terminal zonder `sudo apt` met één commando (`pixi run build-win`) de volledige Windows-versie.
- Compileert [CMakeLists.txt](../CMakeLists.txt) via `FetchContent` automatisch `libopus` statisch mee (`-static`), zodat `uhura.exe` **nul losse `.dll`-bestanden** nodig heeft en uitsluitend standaard Windows 11 systeem-DLL's aanspreekt (`USER32.dll`, `GDI32.dll`, `WINMM.dll`, `WS2_32.dll` en Windows UCRT).
- Draaien alle **7 deterministische unit-testsuites** óók 1-op-1 als native Windows `.exe`-bestanden.

---

## 3. Opbouw van `src/platform/win32/` (`0` `malloc`, `0` globals)

```text
┌──────────────────────────────────────────────────────────────────────────────┐
│                       src/platform/win32/ (Dunne Win32 HAL)                  │
│                                                                              │
│  ┌──────────────────────┐  ┌────────────────────┐  ┌──────────────────────┐  │
│  │ audio_winmm.c        │  │ net_winsock.c      │  │ main_win32.c         │  │
│  │ Event-Driven 4x10ms  │  │ Winsock2 non-block │  │ Dubbelgebufferd GDI  │  │
│  │ 48kHz Mono S16 boven │  │ UDP (InetPtonA,    │  │ knoppenvenster +     │  │
│  │ Windows WASAPI mixer │  │ 0 heap allocaties) │  │ uhura.txt auto-config│  │
│  └──────┬──────────▲────┘  └──────┬────────▲────┘  └──────────┬───────────┘  │
└─────────┼──────────┼──────────────┼────────┼──────────────────┼──────────────┘
          │          │              │        │                  │
          ▼          │              ▼        │                  ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│          Ongewijzigde src/core/ (gedeeld met Linux & ESP32!)                 │
└──────────────────────────────────────────────────────────────────────────────┘
```

1. **Audio (`src/platform/win32/audio_winmm.c`):**
   - Gebruikt vier statisch voorbereide `WAVEHDR`-blokken van exact `480` samples (`10 ms` @ `48 kHz` mono `int16_t`) met `CALLBACK_EVENT` op een `THREAD_PRIORITY_TIME_CRITICAL` worker-thread.
   - Onder water schakelt Windows 11 hiervoor automatisch zijn WASAPI sample-rate/kanaal-converter in (zodat elke USB-headset of laptopmicrofoon zonder formaat-onderhandeling en zonder COM `CoTaskMemFree` heap-allocaties werkt), en blijft weergave ook netjes werken op een pc zonder aangesloten microfoon.
2. **Netwerk (`src/platform/win32/net_winsock.c`):**
   - Non-blocking Winsock2 UDP unicast met `InetPtonA` (IPv4 & IPv6, nul `malloc`/DNS-blokkades).
3. **GUI & Dubbelklik-Configuratie (`src/platform/win32/main_win32.c`):**
   - Geen globale variabelen (`WinAppRuntime_Owned*` hangt aan het venster via `GWLP_USERDATA`), en exacte `10,00 ms` klok-tracking via `QueryPerformanceCounter`.
   - **Echte vasthoud-knoppen met de muis (`SetCapture` op `WM_LBUTTONDOWN` / `WM_LBUTTONUP`):** Knop ingedrukt houden = spreken, loslaten = luisteren (of bij de oranje Welzijns-knop: automatisch 30 seconden gevoelig meeluisteren).
   - **Automatische `uhura.txt` naast `uhura.exe`:** Ondersteunt o.a. meerdere gemachtigde familieleden voor de meeluister-functie op één regel (`allow_listen = 1, 4`) of op losse regels onder elkaar.
