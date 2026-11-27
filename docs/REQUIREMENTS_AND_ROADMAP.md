# Masterplan, Architectuurbesluiten & Stapsgewijze Roadmap (`UHURA`)

Dit document legt de systeemeisen, ontwerpbeslissingen (met onderbouwing) en het stapsgewijze bouw- en testplan vast voor het volledige **UHURA Intercomsysteem** over meerdere targets (**Linux `x86_64`/`aarch64`** én **ESP32 met 8 MB PSRAM, I2S audio en `MCP23017` bediening**) en meerdere netwerkomgevingen (LAN, thuis-internet én **Eduroam**/CGNAT).

Net als in [Shared_Energy_Telemetry_Device](https://github.com/edwin-oetelaar/Shared_Energy_Telemetry_Device) geldt één vaste regel voor de fasering: **elke stap eindigt met een werkend en testbaar systeem**, en vóór elke stap keuren we eerst het deelontwerp goed conform [AGENTS.md](../AGENTS.md).

---

## 1. Kernvisie & Doelgroep-eisen

Het einddoel op hardware is géén ingewikkelde computer of smartphone-app, maar een **compact, robuust fysiek intercomkastje** dat ook voor ouderen of minder goed horenden volstrekt vanzelfsprekend werkt:

1. **Direct aan in seconden (< 2,5 s), niet in minuten:** Stroom erop = binnen een kwart seconde branden de statuslampjes en binnen ~2 seconden is Wi-Fi verbonden en kan er gesproken worden vanuit de lokale peer-cache.
2. **Luid, helder en verstaanbaar geluid:** Voldoende akoestisch vermogen (I2S DAC/versterker), automatische volumeregeling (AGC/limiter zodat zachte en luide sprekers even duidelijk binnenkomen), filtering voor spraakverstaanbaarheid, en géén inschakel-plop.
3. **Eén Knop = Eén Persoon, Eén Lampje = Eén Betekenis:** Grote fysieke knoppen en losse LEDs via een **I2C GPIO-expander (`MCP23017`)**. Geen menu's, geen touchscreens, geen onbegrijpelijke knippercodes:
   - **Groen bij naam:** Deze persoon is bereikbaar (online).
   - **Geel/Wit bij naam:** Deze persoon praat nú tegen jou.
   - **Rood centraal ("MIC AAN"):** Jouw microfoon staat nu open (100% duidelijke privacy-indicatie).
   - **Systeemlampje:** Verbinding met netwerk is in orde.
4. **Licht & Simpel blijven:** De hele kern compileert op een pc in ~2 seconden, gebruikt nul dynamische geheugenallocatie (`malloc`/`free`), en blijft vrij van zware frameworks.

---

## 2. Hergebruik uit Bestaande Eigen Projecten

We hoeven het wiel niet opnieuw uit te vinden; twee eerdere eigen projecten leveren direct bewezen kennis en C-bouwstenen voor de ESP32- en netwerklaag:

| Bronproject | Wat daar al bewezen is | Hoe we het in `UHURA` benutten |
|---|---|---|
| **[microlink](https://github.com/chat-l18l/microlink)** | ESP32 Tailscale/DERP/WireGuard stack op SRAM + 8 MB PSRAM: `x25519`, `chacha20poly1305`, `blake2s`, `ml_stun.c` (NAT discovery), `ml_peer_nvs.c` (NVS peer-cache), en `ml_derp.c` (TLS relay over TCP `443` door 4G CGNAT heen), zonder gedeelde mutexen tussen FreeRTOS-taken. | Levert de exacte lichtgewicht C-cryptoprimitieven, NVS peer-opslag, STUN hole-punching logica én het bewezen patroon om via TCP/TLS `443` dwars door **Eduroam** en CGNAT te komen. |
| **[Shared_Energy_Telemetry_Device](https://github.com/edwin-oetelaar/Shared_Energy_Telemetry_Device)** | **3-slots Wi-Fi selectie** met éénmalige 2s scan (`wifi_slots.c`, `wifi_storage.c`), **Captive Portal** (`dns_server.c`, `wifi_provisioning.c`, `wifi_web.c`, `uri_decode.c`), host unit-tests op Linux, en scherpe beveiligingsafwegingen (`BESLUIT-c5-h5.md`). | Maakt eenmalige Wi-Fi + Kring-setup bij bijv. je moeder thuis eenvoudig via een telefoon, terwijl het kastje tegelijkertijd jouw eigen werkbank-Wi-Fi én hotspot in de andere 2 slots onthoudt. |

---

## 3. Vastgelegde Architectuurbesluiten (ADR's)

### Besluit 1 — Één Platform-loze C-Core voor zowel Linux als ESP32
- **Keuze:** Alle protocol-, codec-, jitterbuffer-, mixer-, AGC- en pakket-cryptologica leeft in `src/core/` zonder één enkele Linux- of ESP-IDF-header. Linux heeft `src/platform/linux/`; ESP32 krijgt `src/platform/esp32/`.
- **Onderbouwing:** Een bug of randgeval in netwerk-roaming, jitter of audio-mixing testen en reproduceren we in **2 seconden** op Linux met `pixi run test`, in plaats van telkens firmware te flashen en met twee fysieke bordjes te gokken.

### Besluit 2 — Geheugenindeling op ESP32: PSRAM (4 MB gemapt) vs. Interne SRAM
- **Context:** Het doel-board heeft **8 MB PSRAM**, waarvan **4 MB direct in de memory-map** hangt.
- **Keuze:**
  - Grote, niet-ISR contexten zoals `IntercomCore_Owned` (~290 KB voor Opus encoder + 4 gelijktijdige Opus decoders + jitterbuffers) worden statisch in het gemapte **PSRAM-segment** (`EXT_RAM_BSS_ATTR`) geplaatst — wij hoeven de audio- of meerspreker-capaciteit op de ESP32 dus **niet** te verarmen!
  - **I2S DMA-buffers, `SpscPcmRing_Owned` en GPIO/I2C-interruptdata blijven verplicht in interne SRAM** (`.dram0.bss`).
- **Onderbouwing:** Wanneer ESP-IDF naar NVS-flash schrijft (bijv. om een gewijzigd peer-IP of Wi-Fi slot te bewaren), wordt de SPI-bus/cache naar externe flash én PSRAM kort gepauzeerd. Als I2S-DMA of een interrupt op dat moment PSRAM aanraakt, stottert de audio of treedt een cache-error panic op. Met DMA + SPSC-ring in interne SRAM loopt de real-time 10 ms audioklok 100% ongestoord door.

### Besluit 3 — Eigen Slank Switchboard vs. Volledige Tailscale Control-Plane (Opstarttijd!)
- **Afweging:** We zouden `microlink` compleet tegen Tailscale/Headscale kunnen aanhangen, óf we gebruiken een eigen slank UHURA-Switchboard (Django) met alleen de kleine C-bouwstenen uit `microlink` (`x25519` + `chacha20poly1305` + STUN + NVS-cache + simpele poort-443 relay).
- **Keuze:** **Eigen slank UHURA-protocol + Offline-first NVS-cache** (optioneel kan op Linux natuurlijk nog steeds over een bestaande WireGuard/Tailscale-interface gepraat worden).
- **Onderbouwing:**
  1. **Opstarten in ~2 seconden:** Een volledige Tailscale Noise + HTTP/2 + 512 KB JSON map-synchronisatie kost bij koude start al gauw 5 tot 10 seconden en leunt op Tailscale's control-plane. Met onze gecachte NVS-peers (`ml_peer_nvs`) is het kastje zodra Wi-Fi een IP heeft (~1,5–2 s) **meteen live**; verversing bij het Switchboard gebeurt rustig op de achtergrond.
  2. **Nul configuratie voor familie:** Je hoeft geen Tailscale-accounts of ACL-bestanden te beheren voor *Ma / Zus / Schoonmoeder*; één korte Kring-code in het webportaal volstaat.

### Besluit 4 — Asymmetrische "Kringen" & Scheiding `NodeId` vs. Fysieke Knop
- **Context:** *Jij* praat met *Ma* en *Zus*; *Zus* praat met *Jij*, *Ma* én *Schoonmoeder*, zonder dat *Schoonmoeder* bij jou op een knop komt.
- **Keuze:**
  1. Elk kastje kan lid zijn van **meerdere Kringen** (groepscodes) en ziet uitsluitend de leden uit zijn eigen kringen.
  2. Elk kastje genereert bij zijn allereerste boot éénmalig een eigen `X25519` sleutelpaar in NVS/disk; een **64-bit `NodeId`** (fingerprint van de publieke sleutel) identificeert het kastje op het netwerk.
  3. Elk kastje beheert **lokaal** zijn eigen knoppentoewijzing (`Knop 1 -> NodeId Ma`, `Knop 2 -> NodeId Zus`) zodat knoppen nooit spontaan van volgorde wisselen.

### Besluit 5 — De 4-Traps "Eduroam-Proof" Verbindingsladder + Eind-tot-Eind Crypto
- **Context:** Thuis bij Ma & Zus werkt directe UDP Hole Punching zonder VPN uitstekend, maar in een universitair lab op **Eduroam** blokkeren *Symmetric Enterprise NAT* en strenge firewalls inkomend én vaak willekeurig uitgaand UDP.
- **Keuze:**
  1. Elk `'UHUR'` v2 audiopakket wordt **eind-tot-eind versleuteld en geauthenticeerd** (`ChaCha20-Poly1305`, ~16–24 bytes overhead, 0 `malloc`). Onbekende afzenders worden altijd genegeerd; geldige pakketten van een bekende `NodeId` vanaf een nieuw IP werken direct het peer-adres bij (*Authenticated Roaming*).
  2. Per peer kiest de netwerklaag automatisch de hoogst werkende trap:
     - **Trap 1 — Direct LAN / Bekend IP / VPN** (`0 ms` extra, inclusief optionele LAN-broadcast discovery op hetzelfde Wi-Fi).
     - **Trap 2 — Direct Internet via UDP Hole Punching** (`0 ms` extra, via STUN op het Switchboard + 15s keepalives).
     - **Trap 3 — Blinde UDP-443 Relay** op het Switchboard (als hole-punch faalt door Symmetric NAT maar UDP `443` openstaat).
     - **Trap 4 — Uitgaande TCP/TLS-443 Relay ("Eduroam-modus"):** Als UDP geen respons krijgt (precies zoals de 4G CGNAT-situatie in `microlink/ml_derp.c`), tunnelen we dezelfde versleutelde ~80-byte `'UHUR'` pakketten over één uitgaande TCP/TLS verbinding op poort `443` met `TCP_NODELAY`. Omdat de audio al eind-tot-eind versleuteld is, hoeft het Switchboard niets te decoderen en kan het nooit meeluisteren.

### Besluit 6 — Bediening via `MCP23017` (I2C + Interrupt) & Directe Persoonsknoppen
- **Keuze:**
  - Op het ouderen-/huiskamerkastje werkt elke grote naamknop als **directe Push-to-Talk naar die persoon** (indrukken **[EDWIN]** = selecteer Edwin + spreek; loslaten = klaar). Op een uitgebreid paneel blijft meervoudige selectie + groepsomroep eveneens beschikbaar.
  - De `MCP23017` wordt aangesloten met zijn `INTA`/`INTB` hardware-interruptlijn op één ESP32 GPIO, zodat de I2C-bus in rust stil is (geen digitale stoorpulsen bij de audio-printbanen) en direct bij een knopdruk wordt uitgelezen.

### Besluit 7 — Audio-keten Afgestemd op Slechthorenden & "Roep & Luister" Val-check (Pure Core DSP)
- **Keuze:** Voeg in `src/core/` een vaste-komma (integer, 0 `malloc`) spraakkonditionering én een strikt geautoriseerde **30-seconden "Roep & Luister" welzijnsmodus** toe (volledig uitgewerkt in [DESIGN_CARE_LISTEN.md](./DESIGN_CARE_LISTEN.md)):
  1. **150 Hz 2e-orde High-Pass Filter** (verwijdert kast-dreun en plofklanken, spaart versterkervermogen voor het spraakgebied).
  2. **Spraak-AGC + Soft Limiter + Far-Field Boost (+24 dB tijdens 30s welzijns-luisteren):** Tilt zachte sprekers — en tijdens een welzijnscheck zelfs zacht roepen of kloppen op een deur op 6 meter afstand (met `OPUS_SIGNAL_AUTO` zodat geklop niet als ruis wordt weggefilterd!) — op naar een duidelijk verstaanbaar niveau zonder clipping.
  3. **Strikte lokale per-peer luistertoestemming (`LISTEN_PERM_ALLOWED`), Nood-Volume Override & Multi-Kamer Dekking:** Alleen specifiek op het kastje van de bewoner zelf goedgekeurde personen (bijv. zoon en dochter, nooit andere kring-leden) kunnen via een replay-veilige `ChallengeNonce` een Welzijnsoproep doen op **100% noodvolume** gevolgd door een eenmalig **30s Far-Field luistervenster** (altijd met hoorbare toon en rood **MIC AAN**-lampje). Meerdere kastjes in één huis (bijv. *Woonkamer* + *Overloop/Badkamer*) kunnen hierbij synchroon roepen én tegelijk terugluisteren zonder rondzingen, met indicatie uit welke ruimte geluid klinkt.
  4. **Hardware Mute-sturing** voor de I2S DAC/versterker bij opstarten en rust (geen ruis of inschakel-knal in een stille slaapkamer/woonkamer).

### Besluit 8 — Veilig Wi-Fi Captive Portal met 3 Netwerk-slots
- **Keuze:** Neem het 3-slots scan-ontwerp uit `Shared_Energy_Telemetry_Device` over (zodat *Thuis Werkbank*, *Huis Moeder* en *Telefoon-Hotspot* naast elkaar bewaard blijven zonder trage 20-minuten verbindingslussen).
- **Beveiliging (les C5 uit `BESLUIT-c5-h5.md`):** Het configuratie-AP gaat **niet** ongevraagd openstaan als de router van de bewoner even herstart, maar **uitsluitend** wanneer alle 3 slots leeg zijn (gloednieuw kastje) óf wanneer iemand bewust een fysieke Setup-knopcombinatie indrukt, beveiligd met een uniek WPA2-setupwachtwoord per apparaat.

---

## 4. Stapsgewijze Roadmap (Bouw & Test per Fase)

Elke fase heeft een heldere afbakening, een eigen kort detail-ontwerp vooraf, en een concrete praktijktest voordat we naar de volgende fase gaan:

### Fase 1 — Pure C23 Kern & Linux Intercom *(Voltooid ✅)*
- **Opgeleverd:** Platform-vrije `src/core/` (Opus 48 kHz 10 ms, `'UHUR'` v1 protocol, jitterbuffer met FEC/PLC, PTT `DuplexGate` met oproeptoon), Linux ALSA/UDP app en 5 CTest unit-testsuites. Bouwtijd: ~2 seconden.
- **Praktijkvalidatie:** Live spraaktest tussen twee Linux-pc's over LAN / WireGuard om latentie, Opus-kwaliteit en PTT-gedrag met echte microfoon en speaker te verifiëren.

### Fase 2 — Spraak-AGC/Filter in Core + ESP32 Hardware Bring-up (I2S, PSRAM & `MCP23017`)
- **Doel:** Een fysiek werkend ESP32-intercomkastje dat binnen ~2 seconden opstart en op het lokale netwerk luid en kraakhelder praat met de Linux-intercom.
- **Onderdelen:**
  1. Integer **High-Pass (150 Hz) + AGC/Limiter** module in `src/core/` inclusief deterministische Linux unit-tests.
  2. Multi-target build-opzet zodat `src/core/` zonder één regel kopieerwerk zowel in CMake/Pixi (Linux) als in ESP-IDF (`idf.py`) compileert.
  3. `src/platform/esp32/`:
     - Statische plaatsing van `IntercomCore_Owned` in de 4 MB gemapte PSRAM en `SpscPcmRing_Owned` + DMA in interne SRAM.
     - `audio_i2s.c` (ESP-IDF `i2s_std` voor I2S mics + DAC + versterker mute/unmute).
     - `control_mcp23017.c` (I2C expander driver met interrupt, directe persoons-PTT knoppen en "1 lampje = 1 functie" aansturing).
     - `net_lwip_udp.c` (non-blocking UDP unicast).
- **Oplevertest Fase 2:** Stekker in het ESP32-prototype -> binnen 2s brandt het groene lampje bij *Linux PC* -> knop indrukken en luid/helder heen en weer praten.

### Fase 3 — 3-Slots Wi-Fi & Veilig Captive Portal op ESP32
- **Doel:** Het ESP32-kastje kan zonder hercompileren van jouw werkbank naar het huis van je moeder verhuizen.
- **Onderdelen:**
  - Integratie van het 3-slots Wi-Fi scan-model en Captive Portal (gebaseerd op `Shared_Energy_Telemetry_Device`, geschreven volgens onze strikte ownership- en strong-type regels + Linux unit-tests voor de pure slot/form-logica).
  - Setup-modus te activeren via knopcombinatie op de `MCP23017`, met duidelijke LED-indicatie ("Instelmodus actief").
- **Oplevertest Fase 3:** Verbinden via telefoon met het Captive Portal, Wi-Fi + hotspot instellen, en verifiëren dat het kastje na stroomonderbreking binnen ~2 seconden weer online is.

### Fase 4 — `'UHUR'` v2: `NodeId`, Eind-tot-Eind Encryptie, NVS Peer-Cache & Roaming
- **Doel:** Identiteit loskoppelen van IP-adressen en alle spraak cryptografisch beveiligen.
- **Onderdelen:**
  - Uitbreiden wire-protocol naar `'UHUR'` v2 met 64-bit `NodeId` en `X25519` + `ChaCha20-Poly1305` (kleine zero-`malloc` C-module, afgeleid van Monocypher / `microlink` crypto).
  - Lokale knop-naar-`NodeId` tabel en persistente peer-cache in NVS (ESP32) en bestand (Linux).
  - *Authenticated Endpoint Roaming* + LAN-discovery op hetzelfde Wi-Fi subnet.
  - Uitgebreide Linux unit-tests voor corrupte pakketten, replay-protectie, onbekende bellers en IP-wissels tijdens een gesprek.
- **Oplevertest Fase 4:** Twee kastjes wisselen van IP-adres / Wi-Fi naar hotspot zonder dat configuratie hoeft te worden aangepast, terwijl ongeauthenticeerde UDP-pakketten 100% worden geweigerd.

### Fase 5 — Switchboard (Django), "Kringen", UDP Hole-Punching & Eduroam TCP-443 Relay
- **Doel:** Stekker-erin autoconfiguratie tussen huishoudens (*Ma / Zus / Schoonmoeder*) én gegarandeerde verbinding vanuit het **Eduroam**-lab.
- **Onderdelen:**
  1. **Switchboard Server** (strak getypeerd Python 3.12 + Django): apparaat claimen met korte code, Kringen aanmaken/koppelen, peer-lijsten berekenen per kastje.
  2. **STUN + UDP Hole Punching** in de client voor directe 0 ms internetverbindingen tussen thuisrouters.
  3. **Blinde Poort-443 Relay** (UDP `443` + uitgaande TCP/TLS `443` met `TCP_NODELAY`) voor automatische fallback op Eduroam en 4G CGNAT.
- **Oplevertest Fase 5:** Eén kastje thuis achter consumenten-NAT en één node achter **Eduroam** (of 4G hotspot) vinden elkaar automatisch op basis van hun Kring-code en voeren een vertragingsarm gesprek.

### Fase 6 — Acoustic Echo Cancellation (AEC) & Handsfree Optie
- **Doel:** Optionele handsfree terugpraat-modus achter de bestaande `DuplexGate_process_10ms` aansluiting (op Linux via lichte fixed-point AEC, op ESP32-S3 eventueel ondersteund door ESP-SR / hardware-acceleratie).
