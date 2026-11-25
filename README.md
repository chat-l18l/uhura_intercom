# UHURA — Low-Latency Opus PTT Intercom (Embedded-Grade C23)

Een deterministisch, low-latency **Push-to-Talk (PTT) intercomsysteem** voor Linux (`x86_64` & `aarch64` / Raspberry Pi / NVIDIA Jetson) over **UDP unicast & WireGuard**, geïnspireerd op het wandpaneel van de *USS Enterprise* (`'UHUR'` wire-protocol).

Ontwikkeld volgens strikte **NASA Power of 10** en **Zenoh-pico** engineering-principes (zie [AGENTS.md](./AGENTS.md) en [docs/DESIGN.md](./docs/DESIGN.md)):
- **0 dynamische geheugenallocaties (`malloc`/`free`)** — inclusief `libopus` op statisch uitgelijnde buffers.
- **Expliciete Ownership & Borrowing (`_Owned` / `_Loaned`)** met move-invalidatie.
- **Strong Types & Parameter Object Pattern** — geen naked primitieve types op modulegrenzen.
- **0 globale variabelen** — zelfs Linux signaal- en timerafhandeling loopt via `signalfd` + `timerfd` + `poll()`.
- **100% platform-vrije kern (`src/core/`)** — losgekoppeld van sockets en ALSA, klaar voor Fase 2 (Echo Cancellation).

---

## Bouwen & Testen (Pixi + CMake)

Dit project gebruikt [Pixi](https://pixi.sh) om dezelfde reproduceerbare toolchain (`cmake`, `ninja`, `c-compiler`, `libopus`, `alsa-lib`) op zowel `linux-64` (`x86_64`) als `linux-aarch64` (Raspberry Pi / Jetson) te leveren:

```bash
# Configureer, compileer (met -std=c23 -Wall -Wextra -Werror -Wconversion) en draai alle unit-tests:
pixi run test

# Alleen bouwen:
pixi run build
```

De resulterende binary staat in `./build/uhura`.

---

## Gebruik (LAN & WireGuard)

Elke post heeft een uniek `--id` (`1..16`), een leesbare `--name`, en één of meer `--peer <ID:Naam:IP:Poort>` regels met de WireGuard- of LAN-IP's van de andere posten.

### Voorbeeld met 3 posten over WireGuard (`10.8.0.x`)

**Post 1 — Brug (`10.8.0.1`):**
```bash
./build/uhura --id 1 --name Brug --port 4711 \
  --peer 2:Machinekamer:10.8.0.2:4711 \
  --peer 3:Kantine:10.8.0.3:4711
```

**Post 2 — Machinekamer (`10.8.0.2`, bijv. Raspberry Pi met USB-speakerphone):**
```bash
./build/uhura --id 2 --name Machinekamer --port 4711 \
  --capture-dev plughw:1,0 --playback-dev plughw:1,0 \
  --peer 1:Brug:10.8.0.1:4711 \
  --peer 3:Kantine:10.8.0.3:4711
```

**Post 3 — Kantine (`10.8.0.3`, bijv. Jetson):**
```bash
./build/uhura --id 3 --name Kantine --port 4711 \
  --peer 1:Brug:10.8.0.1:4711 \
  --peer 2:Machinekamer:10.8.0.2:4711
```

> **Tip voor Ubuntu 24.04 Desktop met PipeWire:**  
> Omdat Pixi een eigen geïsoleerde `alsa-lib` meelevert, kun je op een desktop met PipeWire óf direct een hardware-device opgeven (`--capture-dev plughw:0,0`), óf het systeem-pluginpad meegeven zodat het `"default"` PipeWire-device werkt:  
> `ALSA_PLUGIN_DIR=/usr/lib/$(uname -m)-linux-gnu/alsa-lib ./build/uhura ...`  
> Zonder geluidskaart testen kan altijd met `--no-audio` (stuurt een 440 Hz testtoon tijdens PTT).

### Interactieve Toetsen in de Terminal
| Toets | Actie |
|---|---|
| `Spatie` of `t` | **PTT Aan / Uit** (open kanaal om te spreken / sluit kanaal) |
| `Enter` | Direct PTT loslaten (veiligheidsstop) |
| `1` .. `9` | Zet post `1..9` aan/uit in de doel-selectie (meerdere tegelijk mogelijk!) |
| `a` | **All-Call** (selecteer alle geconfigureerde posten tegelijk) |
| `c` | Wis doel-selectie |
| `q` | Netjes afsluiten |

---

## Projectstructuur

```text
.
├── AGENTS.md               # Strikte programmeerregels & Design-First werkwijze
├── docs/DESIGN.md          # Goedgekeurd systeem-, 'UHUR'-protocol- & type-ontwerp
├── pixi.toml               # Multi-arch dependencies (linux-64 & linux-aarch64)
├── CMakeLists.txt          # Strikte C23 build & CTest configuratie
├── include/
│   ├── core/               # 100% OS-vrije headers, Strong Types, _Owned & _Loaned
│   └── platform/           # Smalle Linux HAL-interface
├── src/
│   ├── core/               # Pure protocol-, Opus-, jitterbuffer-, PTT- & mixer-logica
│   └── platform/linux/     # UDP unicast, ALSA real-time thread & terminal-UI
└── tests/                  # Deterministische unit-tests (incl. 3-posten simulatie)
```

## Status

- [x] Werkwijze en codeerstandaard vastgelegd ([AGENTS.md](./AGENTS.md))
- [x] Functionele eisen & scope afgestemd (PTT multi-post intercom, Opus 48 kHz 10 ms, UDP unicast / WireGuard)
- [x] Systeem- en type-ontwerp goedgekeurd ([docs/DESIGN.md](./docs/DESIGN.md) — `'UHUR'` header)
- [x] Fase 1 implementatie (Pure Core + Linux HAL + Unit Tests)
- [ ] Fase 2: Acoustic Echo Cancellation (AEC) voor full-duplex gebruik
