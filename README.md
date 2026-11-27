# UHURA — Robuuste Low-Latency Intercom (Embedded C23 voor ESP32 & Linux)

**UHURA** (vernoemd naar communicatie-officier *Lt. Uhura* van de *USS Enterprise*, met pakket-header `'UHUR'`) is een lichtgewicht, betrouwbaar en razendsnel opstartend **Push-to-Talk intercomsysteem** voor zowel **ESP32-hardware** (met I2S-audio, fysieke knoppen en LEDs) als **Linux** (`x86_64`, Raspberry Pi, NVIDIA Jetson).

---

## Waarom dit project bestaat: Eenvoud & Bruikbaarheid voor Échte Mensen

Moderne smartphones, apps en pc's doen er minuten over om op te starten, zitten vol menu's, wachtwoorden en updates, en zijn voor bijvoorbeeld ouderen of slechthorenden vaak onnodig ingewikkeld of te zacht.

UHURA lost een concreet menselijk probleem op: **direct, helder en zorgeloos stemcontact tussen kamers of huishoudens** (zoals met een ouder op leeftijd, tussen huis en werkplaats, of tussen lab en kantoor):

- **Direct aan in ~2 seconden (niet in minuten):** Stekker erin en vrijwel direct klaar om te praten vanuit het eigen geheugen — geen besturingssysteem dat eerst een minuut moet booten.
- **1 Knop = 1 Persoon, 1 Lampje = 1 Betekenis:** Grote, duidelijke fysieke knoppen en losse LEDs (via een `MCP23017` I2C GPIO-expander). Geen touchscreens of verwarrende knippercodes:
  - **Groen lampje bij een naam:** diegene is online en bereikbaar.
  - **Geel/wit lampje bij een naam:** diegene praat *nú* tegen jou (zodat je direct ziet wie er spreekt).
  - **Rood lampje ("MIC AAN"):** jouw microfoon staat open — 100% transparante privacy in één oogopslag.
  - **Knop van een persoon indrukken = direct spreken; loslaten = klaar.**
- **Luid, helder en afgestemd op minder goed gehoor:** Ontworpen voor krachtige I2S-versterkers, met vaste-komma spraakfiltering en automatische volumeregeling (AGC/limiter) zodat zowel zachte als luide stemmen altijd duidelijk en onvervormd klinken.
- **Unieke "Roep & Luister" Welzijnscheck (Hele-Huis Dekking bij Valgevaar):**
  - Telefoons liggen vaak in een andere kamer en smartwatches of noodkettingen liggen aan de lader of zijn af tijdens het douchen. Specifiek door de bewoner gemachtigde naasten (en *niemand* anders, lokaal op het kastje vergrendeld!) kunnen een **Welzijnsoproep op noodvolume** starten, waarna één of meerdere kastjes in huis (bijv. *Woonkamer* én *Overloop/Badkamer*) synchroon **30 seconden gevoelig terugluisteren** zonder dat er een knop hoeft te worden ingedrukt — zodat zelfs zacht roepen of kloppen op een deur van afstand direct wordt gehoord, met duidelijke indicatie in welke ruimte het geluid klinkt (zie [docs/DESIGN_CARE_LISTEN.md](./docs/DESIGN_CARE_LISTEN.md)).
- **Slimme "Kringen" & Overal Verbinding (van Huiskamer tot Eduroam):**
  - Elke intercom kan lid zijn van één of meerdere **Kringen** (via een simpele koppelcode). Zo kan *Jij* praten met *Ma* en *Zus*, en kan *Zus* daarnaast praten met haar *Schoonmoeder*, zonder dat *Schoonmoeder* bij jou op een knop verschijnt.
  - Werkt thuis stekker-erin zonder VPN (via een eenmalig Wi-Fi Captive Portal en automatische UDP Hole Punching), blijft werken zonder centrale server op bekende IP's/LAN, én heeft een versleutelde poort-443 fallback voor strenge bedrijfs- en universiteitsnetwerken zoals **Eduroam**.

---

## Educatieve Context (Fontys ICT): Van "Code Schrijven" naar Écht Software Engineering

Dit project is daarnaast opgezet vanuit **Fontys ICT** als praktijkcasus en leerproject voor het effectief bouwen van serieuze systemen met moderne **Agentic AI-tools** (zoals *Google Gemini* en *Claude Opus*).

Het vak van de ICT'er verschuift fundamenteel: **het intypen van regels code is niet langer het knelpunt — vakmanschap in Software Engineering is belangrijker dan ooit.** Een AI-agent kan razendsnel code genereren, maar zonder sterke engineering-sturing ontstaat kwetsbare spaghetti. Dit repository laat stap voor stap zien welke menselijke vaardigheden het verschil maken:

1. **Mens- en domeingerichte analyse:** Altijd vertrekken vanuit de eindgebruiker (wat heeft iemand die slecht hoort of geen techneut is écht nodig?) en vanuit de fysieke werkelijkheid van hardware en netwerken.
2. **De juiste vragen stellen & besluiten onderbouwen:** Randgevallen vóór de bouw blootleggen — zoals *Symmetric NAT* op Eduroam, of waarom I2S-DMA niet in ESP32 PSRAM mag staan tijdens flash-schrijfacties — en keuzes vastleggen in [docs/REQUIREMENTS_AND_ROADMAP.md](./docs/REQUIREMENTS_AND_ROADMAP.md).
3. **Harde kwaliteitskaders opleggen ([AGENTS.md](./AGENTS.md)):** Eerst een goedgekeurd ontwerp ([docs/DESIGN.md](./docs/DESIGN.md)), pas daarna implementatie onder strikte embedded-regels (*NASA Power of 10* & *Zenoh-pico* eigenaarschap).
4. **Deterministisch testen voor veiligheid en betrouwbaarheid:** Door alle kernlogica 100% los te koppelen van besturingssysteem en hardware, wordt elk pakket, elk geheugenpad en elk verlies-scenario in milliseconden geautomatiseerd getest voordat het op echte hardware draait.

> Gebruik deze repo gerust als studiemateriaal: lees eerst [AGENTS.md](./AGENTS.md), bekijk daarna het ontwerp in [docs/DESIGN.md](./docs/DESIGN.md), het welzijnsoproep-ontwerp in [docs/DESIGN_CARE_LISTEN.md](./docs/DESIGN_CARE_LISTEN.md), de architectuurbesluiten in [docs/REQUIREMENTS_AND_ROADMAP.md](./docs/REQUIREMENTS_AND_ROADMAP.md) en de uitgebreide achtergrondverhalen voor **hexnut.net / Elektor** in [docs/PROJECT_LOG_BLOG_NOTES.md](./docs/PROJECT_LOG_BLOG_NOTES.md).

---

## Technische Kenmerken & Doelplatformen

Één gedeelde, platform-vrije C23-kern (`src/core/`) bedient twee werelden en compileert vanaf nul in **~2 seconden**:

| Eigenschap | Invulling |
|---|---|
| **Doelhardware** | **ESP32 / ESP32-S3** (8 MB PSRAM, I2S microfoons + Audio DAC, `MCP23017` I2C knoppen/LEDs) én **Linux** (`x86_64` PC/server, `aarch64` Raspberry Pi & NVIDIA Jetson) |
| **Audioprofiel** | **Opus** (`48 kHz` mono, `10 ms` frames = `480` samples/pakket), inclusief in-band **FEC** en **Packet Loss Concealment (PLC)** |
| **Geheugenmodel** | **0 dynamische allocaties (`malloc`/`free`)** — zelfs `libopus` draait op statisch uitgelijnde buffers (~290 KB totaal) |
| **Codeerstandaard** | Strikte **C23** (`-Wall -Wextra -Werror -Wpedantic -Wconversion -Wvla`), expliciet `_Owned` / `_Loaned` eigenaarschap, Strong Types, Parameter Object Pattern en **0 globale variabelen** |

---

## Snel Bouwen & Testen op Linux (Pixi + CMake)

Dit project gebruikt [Pixi](https://pixi.sh) zodat de volledige C23-toolchain (`cmake`, `ninja`, `c-compiler`, `libopus`, `alsa-lib`) op zowel `linux-64` (`x86_64`) als `linux-aarch64` (Raspberry Pi / Jetson) met één commando identiek werkt:

```bash
# Configureer, compileer (onder strikte -Werror vlaggen) en draai alle 5 unit-testsuites:
pixi run test

# Alleen de release/debug-met-symbolen binary bouwen:
pixi run build
```

De gecompileerde Linux-intercom staat daarna in `./build/uhura`.

---

## Gebruik van het Linux Prototype (LAN & WireGuard)

Elke post krijgt een `--id` (`1..16`), een leesbare `--name`, en één of meer `--peer <ID:Naam:IP:Poort>` opties.

### Voorbeeld met 3 posten

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

**Post 3 — Kantine (`10.8.0.3`, bijv. Jetson of tweede PC):**
```bash
./build/uhura --id 3 --name Kantine --port 4711 \
  --peer 1:Brug:10.8.0.1:4711 \
  --peer 2:Machinekamer:10.8.0.2:4711
```

> **Tip voor Ubuntu 24.04 Desktop met PipeWire:**  
> Omdat Pixi een geïsoleerde `alsa-lib` gebruikt, kun je op een desktop met PipeWire óf een direct hardware-device kiezen (`--capture-dev plughw:0,0`), óf Ubuntu's PipeWire-pluginpad meegeven:  
> `ALSA_PLUGIN_DIR=/usr/lib/$(uname -m)-linux-gnu/alsa-lib ./build/uhura ...`  
> Testen zonder geluidskaart of microfoon kan altijd met `--no-audio` (zendt een 440 Hz testtoon tijdens PTT).

### Praktijkscenario Testen: "Roep & 30s Luisteren" tussen 2 PC's

Met dit scenario test je direct het volledige val-check scenario (noodvolume-override, replay-veilige `ChallengeNonce`, 30s gevoelig terugluisteren op afstand en weigering van onbevoegden):

**1. Op PC A ("Huis Moeder", bijv. IP `192.168.1.50`):**
Zet het volume bewust laag (`--volume 10` avondstand) en geef **uitsluitend** post `#1` (`Edwin`) luistertoestemming met `--allow-listen 1` (post `#4` `Buurman` krijgt die vlag *niet*):
```bash
./build/uhura --id 2 --name Ma-Woonkamer --port 4711 --volume 10 \
  --peer 1:Edwin:192.168.1.10:4711 --allow-listen 1 \
  --peer 4:Buurman:192.168.1.99:4711
```

**2. Op PC B ("Huis Edwin", bijv. IP `192.168.1.10`):**
```bash
./build/uhura --id 1 --name Edwin --port 4711 \
  --peer 2:Ma-Woonkamer:192.168.1.50:4711
```

**3. Wat je stap voor stap kunt beproeven:**
- **Gewone oproep (`Spatie` op PC B):** Je stem speelt op PC A netjes zacht af op `10%` kamer-volume. Zodra je loslaat blijft de microfoon op PC A dicht (`LUISTEREN`).
- **Welzijnsoproep (`w` op PC B):**
  1. Druk op `w` en roep *"Ma, is alles goed? Klop even op de deur!"* — op PC A springt de weergave direct naar **`Vol:100%(NOOD!)`**.
  2. Druk nogmaals op `w` (of `Enter`) om los te laten. Binnen enkele milliseconden rondt de eenmalige 64-bit `ChallengeNonce`-handshake af, klinkt op PC A de korte aankondigingstoon en verschijnt **`>>> MIC OPEN 30s->#1 [ROOD] <<<`**.
  3. Loop 5 tot 6 meter weg van PC A en roep zachtjes of klop op een deur: dankzij `OPUS_AUTO` en de `+24 dB` *Far-Field AGC* hoor je dit op PC B helder binnenkomen en licht **`2:Ma-Woonkamer[L]*(GELUID!)`** op.
  4. Na exact **30 seconden** (of direct bij indrukken van `x` op PC A of PC B) sluit de microfoon met een sluittoon en keert het volume op PC A terug naar `10%`.

### Interactieve Toetsen in de Terminal
| Toets | Actie |
|---|---|
| `Spatie` of `t` | **Normale PTT Aan / Uit** (gewone oproep op het ingestelde kamer-volume) |
| `w` | **Welzijnsoproep ("Roep & 30s Luisteren")** — roept op **100% noodvolume** en opent na loslaten gedurende 30s het gevoelige luistervenster (alleen indien door ontvanger toegestaan met `--allow-listen <ID>`) |
| `x` | **Sluit 30s luistervenster direct** (werkt zowel bij de beller als lokaal bij de bewoner) |
| `-` / `+` | Verlaag / verhoog het lokale speaker-volume (`0% .. 100%`) |
| `Enter` | Direct PTT loslaten |
| `1` .. `9` | Zet post `1..9` aan/uit in de doel-selectie (meerdere kamers tegelijk mogelijk) |
| `a` | **All-Call** (selecteer alle geconfigureerde posten tegelijk) |
| `c` | Wis doel-selectie |
| `q` | Netjes afsluiten |

---

## Projectstructuur

```text
.
├── AGENTS.md                        # Strikte embedded C-regels & Design-First werkwijze
├── docs/
│   ├── DESIGN.md                    # Systeem-, 'UHUR'-protocol- & type-ontwerp (Fase 1)
│   ├── DESIGN_CARE_LISTEN.md        # Ontwerp & beveiliging 30s "Roep & Luister" welzijnscheck
│   ├── REQUIREMENTS_AND_ROADMAP.md  # Architectuurbesluiten, Kringen, Eduroam & ESP32-plan
│   └── PROJECT_LOG_BLOG_NOTES.md    # Verhaallijnen & ontwerpafwegingen (hexnut.net / Elektor)
├── pixi.toml                        # Multi-arch dependencies (linux-64 & linux-aarch64)
├── CMakeLists.txt                   # Strikte C23 build & CTest configuratie
├── include/
│   ├── core/                        # 100% OS-vrije headers, Strong Types, _Owned & _Loaned
│   └── platform/                    # Smalle platform HAL-interface
├── src/
│   ├── core/                        # Pure protocol-, Opus-, jitterbuffer-, PTT- & mixer-logica
│   └── platform/linux/              # Linux UDP unicast, ALSA real-time thread & terminal-UI
└── tests/                           # Deterministische unit-tests (incl. 3-posten simulatie)
```

---

## Stapsgewijze Roadmap

Zie [docs/REQUIREMENTS_AND_ROADMAP.md](./docs/REQUIREMENTS_AND_ROADMAP.md) voor de volledige onderbouwing per fase:

- [x] **Fase 1 — Pure C23 Kern & Linux Intercom:** Zero-`malloc` Opus 48 kHz / 10 ms, `'UHUR'` v1 protocol, jitterbuffer met FEC/PLC, PTT `DuplexGate` met oproeptoon, Linux ALSA/UDP app en unit-tests.
- [ ] **Fase 2 — Spraak-AGC & ESP32 Hardware Bring-up:** Spraakverstaanbaarheidsfilter/AGC in `core/` + `platform/esp32/` (8 MB PSRAM, I2S mic/DAC, `MCP23017` knoppen & LEDs).
- [ ] **Fase 3 — 3-Slots Wi-Fi & Veilig Captive Portal (ESP32):** Eenvoudig eenmalig Wi-Fi instellen via telefoon en automatisch kiezen uit 3 onthouden netwerken.
- [ ] **Fase 4 — `'UHUR'` v2 Crypto, `NodeId` & Roaming:** Eind-tot-eind encryptie (`X25519` + `ChaCha20-Poly1305`), losse knoppentoewijzing en naadloze IP-wissels.
- [ ] **Fase 5 — Switchboard, "Kringen" & Eduroam-Fallback:** Webbeheer voor Kringen, automatische UDP Hole Punching én poort-443 TCP/TLS fallback voor restrictieve netwerken.
- [ ] **Fase 6 — Acoustic Echo Cancellation (AEC):** Handsfree full-duplex optie achter de reeds voorbereide 10 ms DSP-poort.
